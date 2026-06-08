#pragma once

// C ABI to the Metal backend.  The core (pure C23 + -fbounds-safety) talks to
// the Objective-C shim (src/metal_backend.m) only through this header.  The
// shim is compiled WITHOUT -fbounds-safety, so the annotations below expand to
// nothing there; that is sound because __single/__counted_by pointers have the
// ABI of a plain C pointer.

#include <ptrcheck.h>
#include <stdint.h>

typedef struct gpu gpu;

// A vertex in canvas pixel space: origin top-left, +x right, +y down.
typedef struct {
    float x, y;
} gpu_vert;

typedef struct {
    float r, g, b, a;
} gpu_rgba;

// Create an offscreen RGBA8 render target of the given size, initialised to
// transparent black.  Returns NULL on failure.
gpu *__single gpu_create(int width, int height);

void gpu_destroy(gpu *__single g);

// Replace the entire target with a single colour.
void gpu_clear(gpu *__single g, gpu_rgba color);

// Draw solid-coloured triangles: every 3 vertices form one triangle.  When
// `blend` is true the triangles composite with source-over alpha blending;
// when false they overwrite the target (used to implement clearRect).
void gpu_draw_solid(gpu *__single g,
                    gpu_vert const *__counted_by(count) verts, int count,
                    gpu_rgba color, bool blend);

// Copy the rendered image into `out` as tightly packed RGBA8, top row first.
// `len` must equal width*height*4.
void gpu_read_rgba(gpu *__single g, uint8_t *__counted_by(len) out, int len);
