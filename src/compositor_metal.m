// Metal backend for compositor.h: blends tiles onto an RGBA16Float target masked by
// a clip-coverage texture.  source-over uses fixed-function blending; the other
// modes use a framebuffer-fetch fragment shader.  Not under -fbounds-safety
// (Objective-C); the ABI uses plain-C pointers.

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include "compositor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char const compositor_metal_src[] = {
#embed <compositor.metal>
    , 0
};

@interface CmpImpl : NSObject
@property (nonatomic, strong) id<MTLDevice> device;
@property (nonatomic, strong) id<MTLCommandQueue> queue;
@property (nonatomic, strong) id<MTLTexture> target;   // RGBA16Float, read back
@property (nonatomic, strong) id<MTLBuffer> targetBuffer;  // backs `target` (linear, zero-copy readback)
@property (nonatomic) NSUInteger targetRowBytes;       // backing stride (>= width*8, aligned)
@property (nonatomic, strong) id<MTLTexture> clip;      // R8 coverage, 255 = open
@property (nonatomic, strong) id<MTLRenderPipelineState> blendPipe;      // source-over
@property (nonatomic, strong) id<MTLRenderPipelineState> compositePipe;  // other GCO modes
@property (nonatomic) int width;
@property (nonatomic) int height;
// Open render pass that consecutive ops batch into.  Committed (without a CPU
// wait) before a clip change, and drained (committed + waited) before a readback.
// The command buffer retains the per-op tile textures -- and the clip texture in
// effect when it was encoded -- until it completes, so they stay alive across the
// deferred commit; a clip change swaps in a fresh clip texture rather than
// overwriting the old one, so no in-flight batch races it.
@property (nonatomic, strong) id<MTLCommandBuffer> batchCB;
@property (nonatomic, strong) id<MTLRenderCommandEncoder> batchEnc;
@property (nonatomic, strong) id<MTLCommandBuffer> lastCB;  // most recent commit, for the readback wait
@end

@implementation CmpImpl
@end

// Lazily open (or reuse) the render pass that consecutive ops batch into.
static id<MTLRenderCommandEncoder> open_batch(CmpImpl *o) {
    if (o.batchEnc) {
        return o.batchEnc;
    }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = o.target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    o.batchCB = [o.queue commandBuffer];
    o.batchEnc = [o.batchCB renderCommandEncoderWithDescriptor:rp];
    return o.batchEnc;
}

// Close and commit the open batch WITHOUT blocking the CPU.  Ops stay ordered (the
// queue runs command buffers in commit order), the GPU pipelines them, and the CPU
// keeps building the next batch -- so a clip change no longer stalls on the GPU.
// Only the readback waits (see drain_batch).
static void submit_batch(CmpImpl *o) {
    if (!o.batchEnc) {
        return;
    }
    [o.batchEnc endEncoding];
    [o.batchCB commit];
    o.lastCB = o.batchCB;
    o.batchEnc = nil;
    o.batchCB = nil;
}

// Commit the open batch and block until all submitted work has completed -- the one
// real CPU<->GPU sync, used before reading the target back.
static void drain_batch(CmpImpl *o) {
    submit_batch(o);
    [o.lastCB waitUntilCompleted];
}

typedef enum { TILE_BLEND, TILE_COMPOSITE } tile_mode;

static id<MTLRenderPipelineState> make_pipeline(id<MTLDevice> device,
                                                id<MTLLibrary> lib,
                                                tile_mode mode) {
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [lib newFunctionWithName:@"tile_vs"];
    pd.fragmentFunction = [lib newFunctionWithName:mode == TILE_BLEND ? @"tile_blend_fs"
                                                                      : @"tile_composite_fs"];
    MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[0];
    ca.pixelFormat = MTLPixelFormatRGBA16Float;
    // TILE_BLEND is premultiplied source-over via fixed-function blending
    // (out = src + dst*(1 - srcAlpha); the tile is premultiplied, so the source
    // factor is One).  TILE_COMPOSITE reads the backdrop via framebuffer fetch and
    // writes the finished colour, so it leaves blending disabled.
    if (mode == TILE_BLEND) {
        ca.blendingEnabled = YES;
        ca.rgbBlendOperation = MTLBlendOperationAdd;
        ca.alphaBlendOperation = MTLBlendOperationAdd;
        ca.sourceRGBBlendFactor = MTLBlendFactorOne;
        ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
        ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    }
    NSError *err = nil;
    id<MTLRenderPipelineState> ps =
        [device newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!ps) {
        fprintf(stderr, "canvas2d: pipeline creation failed: %s\n",
                err.localizedDescription.UTF8String);
    }
    return ps;
}

// Install a fresh R8 clip texture holding `bytes` (255 everywhere if NULL).  A new
// texture (not an overwrite of o.clip) because an already-committed batch may still
// be sampling the previous clip on the GPU; the command buffer keeps that one alive
// until it completes, while subsequent ops sample this one.
static void set_clip_bytes(CmpImpl *o, uint8_t const *bytes) {
    int n = o.width * o.height;
    uint8_t *tmp = NULL;
    if (!bytes) {
        tmp = malloc((size_t)n);
        if (!tmp) {
            return;
        }
        memset(tmp, 0xFF, (size_t)n);
        bytes = tmp;
    }
    MTLTextureDescriptor *cd =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                           width:(NSUInteger)o.width
                                                          height:(NSUInteger)o.height
                                                       mipmapped:NO];
    cd.usage = MTLTextureUsageShaderRead;
    cd.storageMode = MTLStorageModeShared;
    id<MTLTexture> clip = [o.device newTextureWithDescriptor:cd];
    [clip replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)o.width, (NSUInteger)o.height)
            mipmapLevel:0
              withBytes:bytes
            bytesPerRow:(NSUInteger)o.width];
    o.clip = clip;
    free(tmp);
}

compositor *compositor_create(int width, int height) {
    if (width <= 0 || height <= 0) {
        return NULL;
    }
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            fprintf(stderr, "canvas2d: no Metal device\n");
            return NULL;
        }
        NSError *err = nil;
        NSString *src = [NSString stringWithUTF8String:compositor_metal_src];
        id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil error:&err];
        if (!lib) {
            fprintf(stderr, "canvas2d: shader compile failed: %s\n",
                    err.localizedDescription.UTF8String);
            return NULL;
        }

        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                               width:(NSUInteger)width
                                                              height:(NSUInteger)height
                                                           mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;  // never sampled, only resolved-to + read back
        td.storageMode = MTLStorageModeShared;
        // Back the target with a buffer so it's linear (no GPU twiddle/compression)
        // and its bytes are CPU-visible directly -- the readback reads buffer.contents
        // instead of getBytes (which de-twiddles).  bytesPerRow must hit the device's
        // linear-texture alignment.
        NSUInteger align = [device minimumLinearTextureAlignmentForPixelFormat:
                                       MTLPixelFormatRGBA16Float];
        if (align == 0) { align = 256; }
        NSUInteger rowBytes = (NSUInteger)width * sizeof(cnvs_premul);
        rowBytes = (rowBytes + align - 1) / align * align;

        MTLTextureDescriptor *cd =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                               width:(NSUInteger)width
                                                              height:(NSUInteger)height
                                                           mipmapped:NO];
        cd.usage = MTLTextureUsageShaderRead;
        cd.storageMode = MTLStorageModeShared;

        CmpImpl *o = [[CmpImpl alloc] init];
        o.device = device;
        o.queue = [device newCommandQueue];
        o.targetRowBytes = rowBytes;
        o.targetBuffer = [device newBufferWithLength:rowBytes * (NSUInteger)height
                                             options:MTLResourceStorageModeShared];
        o.target = [o.targetBuffer newTextureWithDescriptor:td offset:0 bytesPerRow:rowBytes];
        o.clip = [device newTextureWithDescriptor:cd];
        o.blendPipe = make_pipeline(device, lib, TILE_BLEND);
        o.compositePipe = make_pipeline(device, lib, TILE_COMPOSITE);
        o.width = width;
        o.height = height;
        if (!o.queue || !o.targetBuffer || !o.target || !o.clip ||
            !o.blendPipe || !o.compositePipe) {
            return NULL;
        }
        set_clip_bytes(o, NULL);  // open clip

        // Start the target transparent black.
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = o.target;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [o.queue commandBuffer];
        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        [cb commit];
        [cb waitUntilCompleted];

        return (__bridge_retained compositor *)o;
    }
}

void compositor_destroy(compositor *c) {
    if (c) {
        CmpImpl *o = (__bridge_transfer CmpImpl *)c;
        drain_batch(o);  // finish any in-flight work before resources are released
        // ARC releases at scope end.
    }
}

void compositor_set_clip(compositor *c, uint8_t const *mask, int len) {
    if (!c) {
        return;
    }
    @autoreleasepool {
        CmpImpl *o = (__bridge CmpImpl *)c;
        if (mask && len < o.width * o.height) {
            return;
        }
        submit_batch(o);  // commit pending blends (no CPU wait); they keep the old
                          // clip texture alive while the new one is installed
        set_clip_bytes(o, mask);
    }
}

// Draw a quad over [x,x+w] x [y,y+h] with the given pipeline.  `tile` (if any) is
// bound at texture 0 and sampled relative to (x,y); the clip is always at 1.
// `mode >= 0` binds a blend-mode uniform at fragment buffer 1 (for the composite
// shader); pass -1 for pipelines that don't take one.
static void draw_tile(CmpImpl *o, id<MTLRenderPipelineState> pipe,
                      int x, int y, int w, int h, id<MTLTexture> tile, int mode) {
    float fx = (float)x, fy = (float)y, fx1 = (float)(x + w), fy1 = (float)(y + h);
    float quad[12] = { fx, fy, fx1, fy, fx1, fy1, fx, fy, fx1, fy1, fx, fy1 };
    id<MTLBuffer> vbuf =
        [o.device newBufferWithBytes:quad length:sizeof(quad)
                             options:MTLResourceStorageModeShared];
    float viewport[2] = { (float)o.width, (float)o.height };
    float origin[2] = { fx, fy };

    id<MTLRenderCommandEncoder> enc = open_batch(o);
    [enc setRenderPipelineState:pipe];
    [enc setVertexBuffer:vbuf offset:0 atIndex:0];
    [enc setVertexBytes:viewport length:sizeof(viewport) atIndex:1];
    [enc setFragmentBytes:origin length:sizeof(origin) atIndex:0];
    if (mode >= 0) {
        uint32_t m = (uint32_t)mode;
        [enc setFragmentBytes:&m length:sizeof(m) atIndex:1];
    }
    if (tile) {
        [enc setFragmentTexture:tile atIndex:0];
    }
    [enc setFragmentTexture:o.clip atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
}

// Upload a tile to a sampleable texture (read as float4 by the shaders).
static id<MTLTexture> upload_tile(CmpImpl *o, MTLPixelFormat fmt, int w, int h,
                                  void const *bytes, NSUInteger bytesPerRow) {
    MTLTextureDescriptor *itd =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                           width:(NSUInteger)w
                                                          height:(NSUInteger)h
                                                       mipmapped:NO];
    itd.usage = MTLTextureUsageShaderRead;
    itd.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [o.device newTextureWithDescriptor:itd];
    [tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
           mipmapLevel:0
             withBytes:bytes
           bytesPerRow:bytesPerRow];
    return tex;
}

void compositor_blend(compositor *c, int x, int y, int w, int h,
                      cnvs_premul const *tile, compositor_blend_mode mode) {
    if (!c || !tile || w <= 0 || h <= 0) {
        return;
    }
    @autoreleasepool {
        CmpImpl *o = (__bridge CmpImpl *)c;
        if (x < 0 || y < 0 || x + w > o.width || y + h > o.height) {
            return;
        }
        id<MTLTexture> tex = upload_tile(o, MTLPixelFormatRGBA16Float, w, h, tile,
                                         (NSUInteger)w * sizeof(cnvs_premul));
        // Source-over takes the fixed-function fast path; every other mode goes
        // through the framebuffer-fetch composite shader with a mode uniform.
        if (mode == COMPOSITOR_SRC_OVER) {
            draw_tile(o, o.blendPipe, x, y, w, h, tex, -1);
        } else {
            draw_tile(o, o.compositePipe, x, y, w, h, tex, (int)mode);
        }
    }
}

void compositor_read(compositor *c, cnvs_premul *out, int len) {
    if (!c || !out) {
        return;
    }
    @autoreleasepool {
        CmpImpl *o = (__bridge CmpImpl *)c;
        int n = o.width * o.height;
        if (len < n) {
            return;
        }
        drain_batch(o);  // the one real sync: finish all pending GPU work first
        // The target is linear and CPU-visible (buffer-backed), so read its bytes
        // directly -- no getBytes de-twiddle.  Copy row by row because the backing
        // stride is aligned up from width*8; it collapses to one copy when they match.
        // Premultiplied, handed back verbatim (straight-alpha + 8-bit on the canvas side).
        uint8_t const *base = (uint8_t const *)o.targetBuffer.contents;
        NSUInteger tight = (NSUInteger)o.width * sizeof(cnvs_premul);
        if (o.targetRowBytes == tight) {
            memcpy(out, base, tight * (NSUInteger)o.height);
        } else {
            for (int row = 0; row < o.height; row++) {
                memcpy((uint8_t *)out + (NSUInteger)row * tight,
                       base + (NSUInteger)row * o.targetRowBytes, tight);
            }
        }
    }
}
