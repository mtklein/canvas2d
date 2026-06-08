// Objective-C Metal backend behind the C ABI in gpu.h, and the project's single
// unsafe boundary: -fbounds-safety is C-only, so this talks to Metal without it.

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include "gpu.h"

#include <stdio.h>
#include <stdlib.h>

// The Metal shader source, embedded at build time (C23 #embed) and compiled at
// runtime -- so the build needs no offline Metal toolchain component.
static char const canvas_metal_src[] = {
#embed <canvas.metal>
    , 0  // NUL-terminate to form a valid C string
};

static MTLPixelFormat const kStencilFormat = MTLPixelFormatStencil8;
static NSUInteger const kSampleCount = 4;  // MSAA: draws antialias at 4x, resolve to target

@interface GpuImpl : NSObject
@property (nonatomic, strong) id<MTLDevice> device;
@property (nonatomic, strong) id<MTLCommandQueue> queue;
@property (nonatomic, strong) id<MTLTexture> target;     // 1x resolve dest + readback
@property (nonatomic, strong) id<MTLTexture> msaaColor;  // 4x colour, resolves to target
@property (nonatomic, strong) id<MTLTexture> stencil;    // 4x, matches msaaColor
@property (nonatomic, strong) id<MTLRenderPipelineState> blendPipe;
@property (nonatomic, strong) id<MTLRenderPipelineState> replacePipe;
@property (nonatomic, strong) id<MTLRenderPipelineState> clipPipe;
@property (nonatomic, strong) id<MTLRenderPipelineState> gradPipe;   // per-vertex colour
@property (nonatomic, strong) id<MTLRenderPipelineState> imgPipe;    // putImageData blit
@property (nonatomic, strong) id<MTLDepthStencilState> dsClip;       // incr where ==ref
@property (nonatomic, strong) id<MTLDepthStencilState> dsDrawClip;   // pass where ==ref
@property (nonatomic) int width;
@property (nonatomic) int height;
@property (nonatomic) int clipLevel;  // 0 = no clip; else draws must match stencil==clipLevel
// Open render pass that consecutive draws batch into; nil when none.  The
// command buffer retains the vertex buffers its draws reference until it
// completes (Metal's default), so deferred buffers stay alive until flush.
@property (nonatomic, strong) id<MTLCommandBuffer> batchCB;
@property (nonatomic, strong) id<MTLRenderCommandEncoder> batchEnc;
@end

@implementation GpuImpl
@end

// The three colour-attachment behaviours a pipeline can have: overwrite the
// target, composite source-over, or write only the stencil (the clip-mask pass,
// no colour) -- mutually exclusive, hence one enum rather than two bools.
typedef enum { PIPE_REPLACE, PIPE_BLEND, PIPE_CLIP } pipe_mode;

static id<MTLRenderPipelineState> make_pipeline(id<MTLDevice> device,
                                                id<MTLLibrary> lib,
                                                NSString *vs, NSString *fs,
                                                pipe_mode mode) {
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [lib newFunctionWithName:vs];
    pd.fragmentFunction = [lib newFunctionWithName:fs];
    pd.rasterSampleCount = kSampleCount;  // render into the 4x attachments
    pd.stencilAttachmentPixelFormat = kStencilFormat;
    MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[0];
    ca.pixelFormat = MTLPixelFormatRGBA8Unorm;
    if (mode == PIPE_CLIP) {
        ca.writeMask = MTLColorWriteMaskNone;  // stencil only
    } else if (mode == PIPE_BLEND) {
        ca.blendingEnabled = YES;
        ca.rgbBlendOperation = MTLBlendOperationAdd;
        ca.alphaBlendOperation = MTLBlendOperationAdd;
        ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
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

static id<MTLDepthStencilState> make_stencil_state(id<MTLDevice> device,
                                                   MTLStencilOperation pass_op) {
    MTLStencilDescriptor *sd = [[MTLStencilDescriptor alloc] init];
    sd.stencilCompareFunction = MTLCompareFunctionEqual;  // vs the reference value
    sd.stencilFailureOperation = MTLStencilOperationKeep;
    sd.depthFailureOperation = MTLStencilOperationKeep;
    sd.depthStencilPassOperation = pass_op;
    sd.readMask = 0xFF;
    sd.writeMask = 0xFF;
    MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = NO;
    dd.frontFaceStencil = sd;
    dd.backFaceStencil = sd;
    return [device newDepthStencilStateWithDescriptor:dd];
}

// Lazily open (or reuse) the render pass that consecutive colour draws batch
// into.  loadAction=Load preserves the multisample colour/stencil across batches;
// each flush resolves the 4x colour down into the 1x target for readback.
static id<MTLRenderCommandEncoder> open_batch(GpuImpl *o) {
    if (o.batchEnc) {
        return o.batchEnc;
    }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = o.msaaColor;
    rp.colorAttachments[0].resolveTexture = o.target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    rp.stencilAttachment.texture = o.stencil;
    rp.stencilAttachment.loadAction = MTLLoadActionLoad;
    rp.stencilAttachment.storeAction = MTLStoreActionStore;
    o.batchCB = [o.queue commandBuffer];
    o.batchEnc = [o.batchCB renderCommandEncoderWithDescriptor:rp];
    return o.batchEnc;
}

// Submit the open batch (if any) and block until it finishes.  Called before any
// operation that must observe prior draws -- a readback, a region write, or a
// stencil/clip change that can't be expressed inside the open pass.
static void flush_batch(GpuImpl *o) {
    if (!o.batchEnc) {
        return;
    }
    [o.batchEnc endEncoding];
    [o.batchCB commit];
    [o.batchCB waitUntilCompleted];
    o.batchEnc = nil;
    o.batchCB = nil;
}

gpu *gpu_create(int width, int height) {
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
        NSString *src = [NSString stringWithUTF8String:canvas_metal_src];
        id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil error:&err];
        if (!lib) {
            fprintf(stderr, "canvas2d: shader compile failed: %s\n",
                    err.localizedDescription.UTF8String);
            return NULL;
        }

        // 1x readback/resolve target.
        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:(NSUInteger)width
                                                              height:(NSUInteger)height
                                                           mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeShared;

        // 4x multisample colour + stencil that draws render into.
        MTLTextureDescriptor *md =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:(NSUInteger)width
                                                              height:(NSUInteger)height
                                                           mipmapped:NO];
        md.textureType = MTLTextureType2DMultisample;
        md.sampleCount = kSampleCount;
        md.usage = MTLTextureUsageRenderTarget;
        md.storageMode = MTLStorageModePrivate;

        MTLTextureDescriptor *sd =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kStencilFormat
                                                               width:(NSUInteger)width
                                                              height:(NSUInteger)height
                                                           mipmapped:NO];
        sd.textureType = MTLTextureType2DMultisample;
        sd.sampleCount = kSampleCount;
        sd.usage = MTLTextureUsageRenderTarget;
        sd.storageMode = MTLStorageModePrivate;

        GpuImpl *o = [[GpuImpl alloc] init];
        o.device = device;
        o.queue = [device newCommandQueue];
        o.target = [device newTextureWithDescriptor:td];
        o.msaaColor = [device newTextureWithDescriptor:md];
        o.stencil = [device newTextureWithDescriptor:sd];
        o.blendPipe = make_pipeline(device, lib, @"solid_vs", @"solid_fs", PIPE_BLEND);
        o.replacePipe = make_pipeline(device, lib, @"solid_vs", @"solid_fs", PIPE_REPLACE);
        o.clipPipe = make_pipeline(device, lib, @"solid_vs", @"solid_fs", PIPE_CLIP);
        o.gradPipe = make_pipeline(device, lib, @"grad_vs", @"grad_fs", PIPE_BLEND);
        o.imgPipe = make_pipeline(device, lib, @"solid_vs", @"img_fs", PIPE_REPLACE);
        o.dsClip = make_stencil_state(device, MTLStencilOperationIncrementClamp);
        o.dsDrawClip = make_stencil_state(device, MTLStencilOperationKeep);
        o.width = width;
        o.height = height;
        o.clipLevel = 0;
        if (!o.queue || !o.target || !o.msaaColor || !o.stencil || !o.blendPipe ||
            !o.replacePipe || !o.clipPipe || !o.gradPipe || !o.imgPipe ||
            !o.dsClip || !o.dsDrawClip) {
            return NULL;
        }

        // Clear the multisample colour to transparent (and resolve it through to
        // target so a readback before any draw sees transparent black) and zero
        // the stencil.
        @autoreleasepool {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = o.msaaColor;
            rp.colorAttachments[0].resolveTexture = o.target;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
            rp.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
            rp.stencilAttachment.texture = o.stencil;
            rp.stencilAttachment.loadAction = MTLLoadActionClear;
            rp.stencilAttachment.clearStencil = 0;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [o.queue commandBuffer];
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
        }
        return (__bridge_retained gpu *)o;
    }
}

void gpu_destroy(gpu *g) {
    if (g) {
        GpuImpl *o = (__bridge_transfer GpuImpl *)g;
        flush_batch(o);  // drain any open batch before the encoder is released
        // ARC releases the transferred reference at scope end.
    }
}

void gpu_draw_solid(gpu *g, gpu_vert const *verts, int count,
                    gpu_rgba color, bool blend) {
    if (!g || count <= 0 || !verts) {
        return;
    }
    @autoreleasepool {
        GpuImpl *o = (__bridge GpuImpl *)g;

        id<MTLBuffer> vbuf =
            [o.device newBufferWithBytes:verts
                                  length:(NSUInteger)count * sizeof(gpu_vert)
                                 options:MTLResourceStorageModeShared];

        id<MTLRenderCommandEncoder> enc = open_batch(o);
        [enc setRenderPipelineState:blend ? o.blendPipe : o.replacePipe];
        if (o.clipLevel > 0) {
            [enc setDepthStencilState:o.dsDrawClip];
            [enc setStencilReferenceValue:(uint32_t)o.clipLevel];
        }

        // Layout matches MSL float2 (8 bytes) and float4 (16 bytes).
        float viewport[2] = { (float)o.width, (float)o.height };
        float col[4] = { color.r, color.g, color.b, color.a };
        [enc setVertexBuffer:vbuf offset:0 atIndex:0];
        [enc setVertexBytes:viewport length:sizeof(viewport) atIndex:1];
        [enc setFragmentBytes:col length:sizeof(col) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:(NSUInteger)count];
    }
}

void gpu_draw_verts(gpu *g, gpu_cvert const *verts, int count) {
    if (!g || count <= 0 || !verts) {
        return;
    }
    @autoreleasepool {
        GpuImpl *o = (__bridge GpuImpl *)g;

        id<MTLBuffer> vbuf =
            [o.device newBufferWithBytes:verts
                                  length:(NSUInteger)count * sizeof(gpu_cvert)
                                 options:MTLResourceStorageModeShared];

        id<MTLRenderCommandEncoder> enc = open_batch(o);
        [enc setRenderPipelineState:o.gradPipe];
        if (o.clipLevel > 0) {
            [enc setDepthStencilState:o.dsDrawClip];
            [enc setStencilReferenceValue:(uint32_t)o.clipLevel];
        }

        float viewport[2] = { (float)o.width, (float)o.height };
        [enc setVertexBuffer:vbuf offset:0 atIndex:0];
        [enc setVertexBytes:viewport length:sizeof(viewport) atIndex:1];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:(NSUInteger)count];
    }
}

void gpu_clip_reset(gpu *g) {
    if (!g) {
        return;
    }
    @autoreleasepool {
        GpuImpl *o = (__bridge GpuImpl *)g;
        flush_batch(o);  // pending draws must use the old clip before it resets
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = o.msaaColor;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;  // keep the image
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.stencilAttachment.texture = o.stencil;
        rp.stencilAttachment.loadAction = MTLLoadActionClear;
        rp.stencilAttachment.clearStencil = 0;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [o.queue commandBuffer];
        id<MTLRenderCommandEncoder> enc =
            [cb renderCommandEncoderWithDescriptor:rp];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        o.clipLevel = 0;
    }
}

void gpu_clip_add(gpu *g, gpu_vert const *verts, int count) {
    if (!g) {
        return;
    }
    @autoreleasepool {
        GpuImpl *o = (__bridge GpuImpl *)g;
        flush_batch(o);  // pending draws must use the old clip before it tightens
        if (count > 0 && verts) {
            id<MTLBuffer> vbuf =
                [o.device newBufferWithBytes:verts
                                      length:(NSUInteger)count * sizeof(gpu_vert)
                                     options:MTLResourceStorageModeShared];

            MTLRenderPassDescriptor *rp =
                [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = o.msaaColor;
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;  // no colour write
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.stencilAttachment.texture = o.stencil;
            rp.stencilAttachment.loadAction = MTLLoadActionLoad;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;

            id<MTLCommandBuffer> cb = [o.queue commandBuffer];
            id<MTLRenderCommandEncoder> enc =
                [cb renderCommandEncoderWithDescriptor:rp];
            [enc setRenderPipelineState:o.clipPipe];
            [enc setDepthStencilState:o.dsClip];
            // Increment stencil from clipLevel to clipLevel+1 where this path
            // covers pixels already inside all prior clips (stencil == clipLevel).
            [enc setStencilReferenceValue:(uint32_t)o.clipLevel];

            float viewport[2] = { (float)o.width, (float)o.height };
            float col[4] = { 0.0f, 0.0f, 0.0f, 0.0f };  // discarded (writeMask none)
            [enc setVertexBuffer:vbuf offset:0 atIndex:0];
            [enc setVertexBytes:viewport length:sizeof(viewport) atIndex:1];
            [enc setFragmentBytes:col length:sizeof(col) atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:0
                    vertexCount:(NSUInteger)count];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
        }
        // Even an empty clip advances the level: with no pixel raised to the new
        // level, draws testing stencil == clipLevel are fully clipped out.
        o.clipLevel += 1;
    }
}

void gpu_read_rgba(gpu *g, uint8_t *out, int len) {
    if (!g || !out) {
        return;
    }
    GpuImpl *o = (__bridge GpuImpl *)g;
    int need = o.width * o.height * 4;
    if (len < need) {
        return;
    }
    flush_batch(o);  // execute pending draws so the readback sees them
    [o.target getBytes:out
           bytesPerRow:(NSUInteger)o.width * 4
            fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)o.width, (NSUInteger)o.height)
           mipmapLevel:0];
}

void gpu_write_region(gpu *g, int x, int y, int w, int h, uint8_t const *pixels) {
    if (!g || !pixels || w <= 0 || h <= 0) {
        return;
    }
    @autoreleasepool {
        GpuImpl *o = (__bridge GpuImpl *)g;
        if (x < 0 || y < 0 || x + w > o.width || y + h > o.height) {
            return;  // caller must clip to the target
        }
        flush_batch(o);  // the region write must land after pending draws

        // Upload the pixels to a sampleable texture, then overwrite the
        // pixel-aligned region in the multisample colour and resolve through to
        // target.  This goes through MSAA like every other draw, but because the
        // quad covers whole pixels the samples are identical -- the bytes survive
        // the resolve exactly.  putImageData ignores the clip, so the default
        // (always-pass) stencil state is used, not dsDrawClip.
        MTLTextureDescriptor *itd =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:(NSUInteger)w
                                                              height:(NSUInteger)h
                                                           mipmapped:NO];
        itd.usage = MTLTextureUsageShaderRead;
        itd.storageMode = MTLStorageModeShared;
        id<MTLTexture> img = [o.device newTextureWithDescriptor:itd];
        [img replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
               mipmapLevel:0
                 withBytes:pixels
               bytesPerRow:(NSUInteger)w * 4];

        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = o.msaaColor;
        rp.colorAttachments[0].resolveTexture = o.target;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
        rp.stencilAttachment.texture = o.stencil;
        rp.stencilAttachment.loadAction = MTLLoadActionLoad;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;

        float fx = (float)x, fy = (float)y;
        float fx1 = (float)(x + w), fy1 = (float)(y + h);
        gpu_vert quad[6] = {
            { fx, fy }, { fx1, fy }, { fx1, fy1 },
            { fx, fy }, { fx1, fy1 }, { fx, fy1 },
        };
        id<MTLBuffer> vbuf =
            [o.device newBufferWithBytes:quad length:sizeof(quad)
                                 options:MTLResourceStorageModeShared];
        float viewport[2] = { (float)o.width, (float)o.height };
        float origin[2] = { fx, fy };

        id<MTLCommandBuffer> cb = [o.queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:o.imgPipe];
        [enc setVertexBuffer:vbuf offset:0 atIndex:0];
        [enc setVertexBytes:viewport length:sizeof(viewport) atIndex:1];
        [enc setFragmentBytes:origin length:sizeof(origin) atIndex:0];
        [enc setFragmentTexture:img atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }
}
