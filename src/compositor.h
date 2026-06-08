#pragma once

// The rendering backend boundary.  The canvas does all the interesting work --
// geometry, analytic coverage, gradient evaluation, clip intersection, and the
// sRGB <-> linear transfer at the 8-bit edges -- in bounds-checked C, and hands
// the compositor only finished tiles to blend.  A Metal backend implements this
// today; nothing here is GPU-specific, so a pure CPU backend could too.
//
// Everything here is linear-light, straight-alpha RGBA16F (_Float16 channels),
// row-major, top row first: the compositor is a pure *linear* blender and never
// touches a transfer function.  All regions must lie within the target.

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

// Overwrite a w*h region at (x,y) with the RGBA16F tile (no blend, ignores the
// clip).  This is putImageData (the caller has already decoded sRGB -> linear).
void compositor_replace(compositor *__single c, int x, int y, int w, int h,
                        _Float16 const *__counted_by(w * h * 4) tile);

// Erase a rectangle toward transparent black, weighted by the clip mask.  This
// is clearRect (with no clip set it fully clears).
void compositor_clear(compositor *__single c, int x, int y, int w, int h);

// Read the linear-light target as tightly packed RGBA16F, top row first; len
// must be width*height*4.  The caller encodes to sRGB 8-bit for output.
void compositor_read_f16(compositor *__single c, _Float16 *__counted_by(len) out, int len);
