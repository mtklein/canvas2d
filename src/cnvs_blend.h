#pragma once

// The blend stage's seam, internal to the canvas (canvas.c implements it):
// composite a premultiplied tile onto the canvas's premultiplied RGBA16F
// target under an enum canvas_composite_op.  Declared here so the blend oracles
// (test_blend, test_coverage_lerp) can drive the kernels directly;
// canvas.c's own paint paths are the production callers.
//
// Tiles and the target are premultiplied RGBA16F (cnvs_premul), row-major, top
// row first.  putImageData is blend with COPY, clearRect is blend with
// DESTINATION_OUT over a unit-alpha splat.  All regions must lie within the
// target (the caller clips to it).

#include "canvas.h"
#include "cnvs_math.h"  // cnvs_premul

#include <ptrcheck.h>
#include <stdint.h>

// One past the last enum canvas_composite_op (the public, web-named enum carries no
// count member): 0..10 Porter-Duff operators, 11..21 separable blend modes,
// 22..25 non-separable.
#define CNVS_BLEND_MODE_COUNT ((int)CANVAS_OP_LUMINOSITY + 1)

// Composite a w*h premultiplied tile at (x,y) onto the target under `mode`.
// `cov` is the op's coverage, one byte per tile pixel (0..255) row-major, or
// NULL for full coverage; `clip` is the clip mask, one coverage byte per
// CANVAS pixel (clip_len >= width*height) or NULL for an open clip -- callers
// pass the canvas's own mask straight through (putImageData, which ignores
// the clip, passes NULL).  The effective coverage is cov x clip, applied per
// coverage_folds -- folded into src for the over-family, lerped after the
// blend for the rest.  Callers whose tile already carries its coverage (the
// folded shade path) pass cov = NULL.  SOURCE_OVER is the fast path.
void cnvs_blend(struct canvas *__single cv, int x, int y, int w, int h,
                cnvs_premul const *__counted_by(w * h) tile,
                uint8_t const *__counted_by_or_null(w * h) cov,
                uint8_t const *__counted_by_or_null(clip_len) clip, int clip_len,
                enum canvas_composite_op mode);

// cnvs_blend of a tile whose every pixel is `color`, without the tile: the
// caller passes the one premultiplied colour and the blend stage splats it
// across the lanes -- byte-identical output to materializing the constant
// tile.  `cov` and `clip` apply exactly as in cnvs_blend.
void cnvs_blend_solid(struct canvas *__single cv, int x, int y, int w, int h,
                      cnvs_premul color,
                      uint8_t const *__counted_by_or_null(w * h) cov,
                      uint8_t const *__counted_by_or_null(clip_len) clip, int clip_len,
                      enum canvas_composite_op mode);

// Copy the premultiplied target out, row-major top-first; pixels must be
// at least width*height.  The oracle tests' bit-exact view of the target --
// canvas_read_rgba is the production readback (unpremultiplied RGBA8).
void cnvs_blend_read(struct canvas *__single cv,
                     cnvs_premul *__counted_by(pixels) out, int pixels);
