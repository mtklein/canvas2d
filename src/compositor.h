#pragma once

// The rendering backend boundary.  The canvas does all the interesting work --
// geometry, analytic coverage, gradient evaluation, clip intersection -- in
// bounds-checked C, and hands the compositor only finished premultiplied tiles to
// composite.  A Metal backend implements this today; nothing here is GPU-specific,
// so a pure CPU backend could implement the same ABI.
//
// The compositor is a pure premultiplied-pixel store: tiles in and the target are
// premultiplied RGBA16F (cnvs_premul), row-major, top row first; read hands the
// premultiplied target straight back.  The whole interface is just three verbs --
// set the clip, composite a tile under a blend mode, read the target.  putImageData
// (blend with COPY) and clearRect (blend with DESTINATION_OUT over a unit-alpha
// tile) fall out of blend; the straight<->premultiplied and 8-bit conversions the
// Canvas API needs all live in checked C on the canvas side.  All regions must lie
// within the target (the caller clips to it).

#include "cnvs_math.h"  // cnvs_premul

#include <ptrcheck.h>
#include <stdint.h>

typedef struct compositor compositor;

// globalCompositeOperation.  The order is canonical: it is shared by value with
// canvas_composite_op (canvas.h) and with the integer cases in the Metal
// composite shader (shaders/compositor.metal) -- keep all three in sync.  Modes
// 0..10 are the Porter-Duff compositing operators (linear in src/dst); 11..21 are
// the separable blend modes; 22..25 are the non-separable ones.
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
// width*height (pixels).  Conversion to straight RGBA8 is the caller's job.
void compositor_read(compositor *__single c, cnvs_premul *__counted_by(len) out, int len);
