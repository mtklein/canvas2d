// Metal backend for compositor.h: source-over / replace / erase of RGBA8 tiles
// onto a single-sample target, masked by a clip-coverage texture.  All the
// interesting rendering work happens in the C core; this just blends tiles.  Not
// under -fbounds-safety (it is Objective-C); the ABI uses plain-C pointers.

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
@property (nonatomic, strong) id<MTLTexture> target;   // 1x RGBA8, readback
@property (nonatomic, strong) id<MTLTexture> clip;      // R8 coverage, 255 = open
@property (nonatomic, strong) id<MTLRenderPipelineState> blendPipe;
@property (nonatomic, strong) id<MTLRenderPipelineState> replacePipe;
@property (nonatomic, strong) id<MTLRenderPipelineState> clearPipe;
@property (nonatomic) int width;
@property (nonatomic) int height;
// Open render pass that consecutive ops batch into; flushed before a clip
// change or readback.  The command buffer retains the per-op tile textures
// until it completes, so they stay alive across the deferred commit.
@property (nonatomic, strong) id<MTLCommandBuffer> batchCB;
@property (nonatomic, strong) id<MTLRenderCommandEncoder> batchEnc;
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

static void flush_batch(CmpImpl *o) {
    if (!o.batchEnc) {
        return;
    }
    [o.batchEnc endEncoding];
    [o.batchCB commit];
    [o.batchCB waitUntilCompleted];
    o.batchEnc = nil;
    o.batchCB = nil;
}

typedef enum { TILE_BLEND, TILE_REPLACE, TILE_CLEAR } tile_mode;

static id<MTLRenderPipelineState> make_pipeline(id<MTLDevice> device,
                                                id<MTLLibrary> lib,
                                                tile_mode mode) {
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [lib newFunctionWithName:@"tile_vs"];
    pd.fragmentFunction = [lib newFunctionWithName:mode == TILE_BLEND ? @"tile_blend_fs"
                                            : mode == TILE_REPLACE ? @"tile_replace_fs"
                                                                   : @"clear_fs"];
    MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[0];
    ca.pixelFormat = MTLPixelFormatRGBA8Unorm;
    if (mode == TILE_BLEND) {
        ca.blendingEnabled = YES;
        ca.rgbBlendOperation = MTLBlendOperationAdd;
        ca.alphaBlendOperation = MTLBlendOperationAdd;
        ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
        ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    } else if (mode == TILE_CLEAR) {
        ca.blendingEnabled = YES;
        ca.rgbBlendOperation = MTLBlendOperationAdd;
        ca.alphaBlendOperation = MTLBlendOperationAdd;
        ca.sourceRGBBlendFactor = MTLBlendFactorZero;
        ca.sourceAlphaBlendFactor = MTLBlendFactorZero;
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

// Upload `bytes` (255 everywhere if NULL) into the R8 clip texture.
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
    [o.clip replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)o.width, (NSUInteger)o.height)
              mipmapLevel:0
                withBytes:bytes
              bytesPerRow:(NSUInteger)o.width];
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
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:(NSUInteger)width
                                                              height:(NSUInteger)height
                                                           mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;

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
        o.target = [device newTextureWithDescriptor:td];
        o.clip = [device newTextureWithDescriptor:cd];
        o.blendPipe = make_pipeline(device, lib, TILE_BLEND);
        o.replacePipe = make_pipeline(device, lib, TILE_REPLACE);
        o.clearPipe = make_pipeline(device, lib, TILE_CLEAR);
        o.width = width;
        o.height = height;
        if (!o.queue || !o.target || !o.clip || !o.blendPipe ||
            !o.replacePipe || !o.clearPipe) {
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
        flush_batch(o);  // drain any open batch before the encoder is released
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
        flush_batch(o);  // pending blends must use the old clip before it changes
        set_clip_bytes(o, mask);
    }
}

// Draw a quad over [x,x+w] x [y,y+h] with the given pipeline.  `tile` (if any) is
// bound at texture 0 and sampled relative to (x,y); the clip is always at 1.
static void draw_tile(CmpImpl *o, id<MTLRenderPipelineState> pipe,
                      int x, int y, int w, int h, id<MTLTexture> tile) {
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
    if (tile) {
        [enc setFragmentTexture:tile atIndex:0];
    }
    [enc setFragmentTexture:o.clip atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
}

// Upload an RGBA8 tile to a sampleable texture.
static id<MTLTexture> upload_tile(CmpImpl *o, int w, int h, uint8_t const *tile) {
    MTLTextureDescriptor *itd =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:(NSUInteger)w
                                                          height:(NSUInteger)h
                                                       mipmapped:NO];
    itd.usage = MTLTextureUsageShaderRead;
    itd.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [o.device newTextureWithDescriptor:itd];
    [tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
           mipmapLevel:0
             withBytes:tile
           bytesPerRow:(NSUInteger)w * 4];
    return tex;
}

void compositor_blend(compositor *c, int x, int y, int w, int h, uint8_t const *tile) {
    if (!c || !tile || w <= 0 || h <= 0) {
        return;
    }
    @autoreleasepool {
        CmpImpl *o = (__bridge CmpImpl *)c;
        if (x < 0 || y < 0 || x + w > o.width || y + h > o.height) {
            return;
        }
        draw_tile(o, o.blendPipe, x, y, w, h, upload_tile(o, w, h, tile));
    }
}

void compositor_replace(compositor *c, int x, int y, int w, int h, uint8_t const *tile) {
    if (!c || !tile || w <= 0 || h <= 0) {
        return;
    }
    @autoreleasepool {
        CmpImpl *o = (__bridge CmpImpl *)c;
        if (x < 0 || y < 0 || x + w > o.width || y + h > o.height) {
            return;
        }
        draw_tile(o, o.replacePipe, x, y, w, h, upload_tile(o, w, h, tile));
    }
}

void compositor_clear(compositor *c, int x, int y, int w, int h) {
    if (!c || w <= 0 || h <= 0) {
        return;
    }
    @autoreleasepool {
        CmpImpl *o = (__bridge CmpImpl *)c;
        if (x < 0 || y < 0 || x + w > o.width || y + h > o.height) {
            return;
        }
        draw_tile(o, o.clearPipe, x, y, w, h, nil);
    }
}

void compositor_read_rgba(compositor *c, uint8_t *out, int len) {
    if (!c || !out) {
        return;
    }
    CmpImpl *o = (__bridge CmpImpl *)c;
    if (len < o.width * o.height * 4) {
        return;
    }
    flush_batch(o);  // execute pending ops so the readback sees them
    [o.target getBytes:out
           bytesPerRow:(NSUInteger)o.width * 4
            fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)o.width, (NSUInteger)o.height)
           mipmapLevel:0];
}
