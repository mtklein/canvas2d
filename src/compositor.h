#pragma once

// Compositor boundary.  The canvas produces finished premultiplied tiles; the
// compositor only composites them (compositor_cpu.c implements this ABI).
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
// (canvas.h) -- keep the two in sync.  0..10 Porter-Duff operators, 11..21
// separable blend modes, 22..25 non-separable.
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

// Coverage semantics (docs/rasterization.md §3.8, ruled 2026-06-10): partial
// coverage applies in principle as a lerp between the destination and the
// full-strength blend, out = lerp(dst, blend(src, dst), cov) -- a pixel the
// shape doesn't cover keeps its destination.  Folding coverage into source
// alpha instead (src *= cov, premultiplied) is identical math only where the
// Porter-Duff form co = Fa*s + Fb*d has Fa free of sa and Fb affine in sa
// with Fb(0) = 1: the over-family below.  Those modes fold (cheaper, and
// bit-compatible with the established source-over pipeline); every other mode
// takes the lerp in compositor_blend.
static inline bool compositor_coverage_folds(compositor_blend_mode m) {
    switch ((int)m) {
        case COMPOSITOR_SRC_OVER:   // Fa = 1,      Fb = 1 - sa
        case COMPOSITOR_SRC_ATOP:   // Fa = da,     Fb = 1 - sa
        case COMPOSITOR_DST_OVER:   // Fa = 1 - da, Fb = 1
        case COMPOSITOR_DST_OUT:    // Fa = 0,      Fb = 1 - sa
        case COMPOSITOR_XOR:        // Fa = 1 - da, Fb = 1 - sa
        case COMPOSITOR_LIGHTER:    // Fa = 1,      Fb = 1
            return true;
        default:                    // copy, the in/out family, dst-atop, blends
            return false;
    }
}

// NULL on failure; the target starts transparent black and the clip starts open.
compositor *__single compositor_create(int width, int height);
void compositor_destroy(compositor *__single c);

// Set the clip mask: one coverage byte per pixel (0..255), length width*height,
// row-major top-first.  Subsequent blends are multiplied by it.  NULL opens the
// clip (everything passes).
void compositor_set_clip(compositor *__single c,
                         uint8_t const *__counted_by(len) mask, int len);

// Composite a w*h premultiplied tile at (x,y) onto the target under `mode`.
// `cov` is the op's coverage, one byte per tile pixel (0..255) row-major, or
// NULL for full coverage; the effective coverage is cov x the current clip
// mask, applied per compositor_coverage_folds -- folded into src for the
// over-family, lerped after the blend for the rest.  Callers whose tile
// already carries its coverage (the folded shade path) pass NULL.
// COMPOSITOR_SRC_OVER is the fast path.
void compositor_blend(compositor *__single c, int x, int y, int w, int h,
                      cnvs_premul const *__counted_by(w * h) tile,
                      uint8_t const *__counted_by_or_null(w * h) cov,
                      compositor_blend_mode mode);

// Read the premultiplied target back, row-major top-first; len must be
// width*height (pixels).  Conversion to unpremultiplied RGBA8 is the caller's job.
void compositor_read(compositor *__single c, cnvs_premul *__counted_by(len) out, int len);
