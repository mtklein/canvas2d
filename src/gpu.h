#pragma once

// C ABI between the bounds-safe C core and the Objective-C Metal shim.
// -fbounds-safety is C-only, so the shim is built without it; sound because
// __single/__counted_by share the plain-C-pointer ABI.

#include <ptrcheck.h>
#include <stdint.h>

typedef struct gpu gpu;

// Canvas pixel space: origin top-left, +y down.
typedef struct {
    float x, y;
} gpu_vert;

typedef struct {
    float r, g, b, a;
} gpu_rgba;

// NULL on failure; the target starts transparent black.
gpu *__single gpu_create(int width, int height);
void gpu_destroy(gpu *__single g);

void gpu_clear(gpu *__single g, gpu_rgba color);

// Triangle list (every 3 vertices = 1 triangle).  blend=false overwrites the
// target (for clearRect) rather than compositing source-over.
void gpu_draw_solid(gpu *__single g,
                    gpu_vert const *__counted_by(count) verts, int count,
                    gpu_rgba color, bool blend);

// Clip stack via the stencil buffer.  gpu_clip_reset clears it (no clip);
// gpu_clip_add intersects the triangles (which must be non-overlapping, e.g. the
// scanline fill output) into the current clip.  Subsequent draws are masked to
// the intersection of all added clips.
void gpu_clip_reset(gpu *__single g);
void gpu_clip_add(gpu *__single g, gpu_vert const *__counted_by(count) verts, int count);

// Tightly packed RGBA8, top row first; len must be width*height*4.
void gpu_read_rgba(gpu *__single g, uint8_t *__counted_by(len) out, int len);

// Overwrite a w x h region of the target at (x,y) with tightly packed RGBA8.
// The region must lie within the target (the caller clips).
void gpu_write_region(gpu *__single g, int x, int y, int w, int h,
                      uint8_t const *__counted_by(w * h * 4) pixels);
