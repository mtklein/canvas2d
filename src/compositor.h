#pragma once

// Rendering backend boundary.  The canvas produces finished premultiplied tiles;
// the compositor only composites them.  The Metal and CPU backends implement this
// same ABI; nothing here is GPU-specific.
//
// Tiles and the target are premultiplied RGBA16F (cnvs_premul), row-major, top row
// first; read() returns the premultiplied target verbatim.  putImageData is blend
// with COPY, clearRect is blend with DESTINATION_OUT over a unit-alpha tile.  All
// regions must lie within the target (the caller clips to it).

#include "cnvs_math.h"  // cnvs_premul

#include <ptrcheck.h>
#include <stdint.h>

typedef struct compositor compositor;

// globalCompositeOperation.  These values are shared with canvas_composite_op
// (canvas.h) and the integer cases in shaders/compositor.metal -- keep all three in
// sync.  0..10 Porter-Duff operators, 11..21 separable blend modes, 22..25
// non-separable.
typedef enum {
    COMPOSITOR_SRC_OVER = 0,
    COMPOSITOR_SRC_IN,
    COMPOSITOR_SRC_OUT,
    COMPOSITOR_SRC_ATOP,
    COMPOSITOR_DST_OVER,
    COMPOSITOR_DST_IN,
    COMPOSITOR_DST_OUT,
    COMPOSITOR_DST_ATOP,
    COMPOSITOR_XOR,
    COMPOSITOR_LIGHTER,
    COMPOSITOR_COPY,
    COMPOSITOR_MULTIPLY,
    COMPOSITOR_SCREEN,
    COMPOSITOR_OVERLAY,
    COMPOSITOR_DARKEN,
    COMPOSITOR_LIGHTEN,
    COMPOSITOR_COLOR_DODGE,
    COMPOSITOR_COLOR_BURN,
    COMPOSITOR_HARD_LIGHT,
    COMPOSITOR_SOFT_LIGHT,
    COMPOSITOR_DIFFERENCE,
    COMPOSITOR_EXCLUSION,
    COMPOSITOR_HUE,
    COMPOSITOR_SATURATION,
    COMPOSITOR_COLOR,
    COMPOSITOR_LUMINOSITY,
    COMPOSITOR_MODE_COUNT,
} compositor_blend_mode;

// NULL on failure; the target starts transparent black and the clip starts open.
compositor *__single compositor_create(int width, int height);
void compositor_destroy(compositor *__single c);

// Set the clip mask: one coverage byte per pixel (0..255), length width*height,
// row-major top-first.  Subsequent blends are multiplied by it.  NULL opens the
// clip (everything passes).
void compositor_set_clip(compositor *__single c,
                         uint8_t const *__counted_by(len) mask, int len);

// Composite a w*h premultiplied tile at (x,y) onto the target under `mode`,
// attenuated by the current clip mask.  COMPOSITOR_SRC_OVER is the fast path.
void compositor_blend(compositor *__single c, int x, int y, int w, int h,
                      cnvs_premul const *__counted_by(w * h) tile,
                      compositor_blend_mode mode);

// Read the premultiplied target back, row-major top-first; len must be
// width*height (pixels).  Conversion to unpremultiplied RGBA8 is the caller's job.
void compositor_read(compositor *__single c, cnvs_premul *__counted_by(len) out, int len);

// Coarse GPU profiling: the total GPU execution time accumulated across every
// command buffer committed since creation (nanoseconds), and the number of those
// command buffers (dispatches).  ns/dispatch = total_ns/dispatches.  The CPU
// backend does no GPU work and reports 0/0.  The Metal backend collects this only
// when the CANVAS_GPU_TIMING environment variable is set at compositor_create time
// -- otherwise it skips the per-command-buffer instrumentation and also reports
// 0/0.  Either output pointer may be NULL.
void compositor_gpu_timing(compositor *__single c,
                           double *__single total_ns, long *__single dispatches);
