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

@interface GpuImpl : NSObject
@property (nonatomic, strong) id<MTLDevice> device;
@property (nonatomic, strong) id<MTLCommandQueue> queue;
@property (nonatomic, strong) id<MTLTexture> target;
@property (nonatomic, strong) id<MTLRenderPipelineState> blendPipe;
@property (nonatomic, strong) id<MTLRenderPipelineState> replacePipe;
@property (nonatomic) int width;
@property (nonatomic) int height;
@end

@implementation GpuImpl
@end

static id<MTLRenderPipelineState> make_pipeline(id<MTLDevice> device,
                                                id<MTLLibrary> lib,
                                                bool blend) {
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [lib newFunctionWithName:@"solid_vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"solid_fs"];
    MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[0];
    ca.pixelFormat = MTLPixelFormatRGBA8Unorm;
    if (blend) {
        ca.blendingEnabled = YES;
        ca.rgbBlendOperation = MTLBlendOperationAdd;
        ca.alphaBlendOperation = MTLBlendOperationAdd;
        ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
        ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    } else {
        ca.blendingEnabled = NO;
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

        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:(NSUInteger)width
                                                              height:(NSUInteger)height
                                                           mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeShared;

        GpuImpl *o = [[GpuImpl alloc] init];
        o.device = device;
        o.queue = [device newCommandQueue];
        o.target = [device newTextureWithDescriptor:td];
        o.blendPipe = make_pipeline(device, lib, true);
        o.replacePipe = make_pipeline(device, lib, false);
        o.width = width;
        o.height = height;
        if (!o.queue || !o.target || !o.blendPipe || !o.replacePipe) {
            return NULL;
        }
        return (__bridge_retained gpu *)o;
    }
}

void gpu_destroy(gpu *g) {
    if (g) {
        GpuImpl *o = (__bridge_transfer GpuImpl *)g;
        (void)o;  // ARC releases the transferred reference at scope end
    }
}

void gpu_clear(gpu *g, gpu_rgba color) {
    if (!g) {
        return;
    }
    @autoreleasepool {
        GpuImpl *o = (__bridge GpuImpl *)g;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = o.target;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor =
            MTLClearColorMake(color.r, color.g, color.b, color.a);
        id<MTLCommandBuffer> cb = [o.queue commandBuffer];
        id<MTLRenderCommandEncoder> enc =
            [cb renderCommandEncoderWithDescriptor:rp];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
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

        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = o.target;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;  // preserve existing
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLCommandBuffer> cb = [o.queue commandBuffer];
        id<MTLRenderCommandEncoder> enc =
            [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:blend ? o.blendPipe : o.replacePipe];

        // Layout matches MSL float2 (8 bytes) and float4 (16 bytes).
        float viewport[2] = { (float)o.width, (float)o.height };
        float col[4] = { color.r, color.g, color.b, color.a };
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
    [o.target getBytes:out
           bytesPerRow:(NSUInteger)o.width * 4
            fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)o.width, (NSUInteger)o.height)
           mipmapLevel:0];
}
