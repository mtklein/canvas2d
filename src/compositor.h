#pragma once

// The rendering backend boundary.  The canvas does all the interesting work --
// geometry, analytic coverage, gradient evaluation, clip intersection -- in
// bounds-checked C, and hands the compositor only finished RGBA8 tiles to blend.
// A Metal backend implements this today; nothing here is GPU-specific, so a pure
// CPU backend could implement the same ABI.
//
// Colour tiles are tightly packed *premultiplied* RGBA16F (_Float16 channels),
// row-major, top row first; the target is premultiplied too, and read_rgba
// un-premultiplies to the straight RGBA8 the Canvas API speaks.  putImageData
// (replace) tiles are straight RGBA8 (premultiplied on entry).  All regions must
// lie within the target (the caller clips to it).

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
// row-major top-first.  Subsequent blend/clear are multiplied by it.  NULL opens
// the clip (everything passes).
void compositor_set_clip(compositor *__single c,
                         uint8_t const *__counted_by(len) mask, int len);

// Composite a w*h premultiplied RGBA16F tile at (x,y) onto the target under
// `mode`, attenuated by the current clip mask.  This is every painted fill and
// stroke (colour and coverage already baked in).  COMPOSITOR_SRC_OVER is the fast
// path.
void compositor_blend(compositor *__single c, int x, int y, int w, int h,
                      _Float16 const *__counted_by(w * h * 4) tile,
                      compositor_blend_mode mode);

// Overwrite a w*h region at (x,y) with the RGBA8 tile (no blend, ignores the
// clip).  This is putImageData, whose source is 8-bit.
void compositor_replace(compositor *__single c, int x, int y, int w, int h,
                        uint8_t const *__counted_by(w * h * 4) tile);

// Erase a rectangle toward transparent black, weighted by the clip mask.  This
// is clearRect (with no clip set it fully clears).
void compositor_clear(compositor *__single c, int x, int y, int w, int h);

// Tightly packed RGBA8, top row first; len must be width*height*4.
void compositor_read_rgba(compositor *__single c, uint8_t *__counted_by(len) out, int len);
