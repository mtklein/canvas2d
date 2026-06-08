#pragma once

// The rendering backend boundary.  The canvas does all the interesting work --
// geometry, analytic coverage, gradient evaluation, clip intersection -- in
// bounds-checked C, and hands the compositor only finished RGBA8 tiles to blend.
// A Metal backend implements this today; nothing here is GPU-specific, so a pure
// CPU backend could implement the same ABI.
//
// Colour tiles are tightly packed straight-alpha RGBA16F (_Float16 channels),
// row-major, top row first; putImageData tiles are RGBA8.  All regions must lie
// within the target (the caller clips to it).

#include <ptrcheck.h>
#include <stdint.h>

typedef struct compositor compositor;

// NULL on failure; the target starts transparent black and the clip starts open.
compositor *__single compositor_create(int width, int height);
void compositor_destroy(compositor *__single c);

// Set the clip mask: one coverage byte per pixel (0..255), length width*height,
// row-major top-first.  Subsequent blend/clear are multiplied by it.  NULL opens
// the clip (everything passes).
void compositor_set_clip(compositor *__single c,
                         uint8_t const *__counted_by(len) mask, int len);

// Source-over a w*h RGBA16F tile at (x,y), multiplied by the current clip mask.
// This is every painted fill and stroke (colour and coverage already baked into
// alpha).
void compositor_blend(compositor *__single c, int x, int y, int w, int h,
                      _Float16 const *__counted_by(w * h * 4) tile);

// Overwrite a w*h region at (x,y) with the RGBA8 tile (no blend, ignores the
// clip).  This is putImageData, whose source is 8-bit.
void compositor_replace(compositor *__single c, int x, int y, int w, int h,
                        uint8_t const *__counted_by(w * h * 4) tile);

// Erase a rectangle toward transparent black, weighted by the clip mask.  This
// is clearRect (with no clip set it fully clears).
void compositor_clear(compositor *__single c, int x, int y, int w, int h);

// Tightly packed RGBA8, top row first; len must be width*height*4.
void compositor_read_rgba(compositor *__single c, uint8_t *__counted_by(len) out, int len);
