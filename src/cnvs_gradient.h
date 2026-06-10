#pragma once

// Linear and radial colour gradients.  Stops are kept sorted by offset in a small
// fixed array, so a gradient is a plain value copied with the canvas state on
// save/restore.  Fills and strokes sample it per pixel on the CPU into the tile the
// compositor blends.

#include "cnvs_math.h"

#include <ptrcheck.h>

#define CNVS_MAX_STOPS 16

typedef enum { CNVS_GRAD_LINEAR, CNVS_GRAD_RADIAL, CNVS_GRAD_CONIC } cnvs_grad_kind;

typedef struct {
    float offset;  // in [0,1]
    cnvs_unpremul color;
} cnvs_stop;

// Coordinates and radii are device space (the CTM is baked in when the gradient
// is created).
typedef struct {
    cnvs_grad_kind kind;
    cnvs_vec2 p0, p1;
    float r0, r1;
    float angle;   // conic: start angle (radians), device space; unused otherwise
    cnvs_stop stops[CNVS_MAX_STOPS];
    int stop_count;
} cnvs_gradient;

// Insert a stop in offset order (offset clamped to [0,1]); a no-op once full.
void cnvs_gradient_add_stop(cnvs_gradient *gr, float offset, cnvs_unpremul color);

// Colour at parameter t (clamped to [0,1]), piecewise-linear across the stops.
// With no stops the result is transparent black.
cnvs_unpremul cnvs_gradient_color_at(cnvs_gradient const *gr, float t);

// Gradient parameter for a device-space point, written to *t (clamped to
// [0,1]).  Returns false when a radial point lies outside the gradient (no
// circle in the family passes through it) -- such samples paint transparent.
bool cnvs_gradient_param(cnvs_gradient const *gr, cnvs_vec2 p, float *__single t);

// Vectorized parameter solve for a horizontal run of `n` pixel centres
// (x0 + i + 0.5, y).  Fills t_out[i] with the parameter in [0,1], or -1 where the
// point has no parameter (radial "outside") so the caller paints transparent.
// Equivalent to calling cnvs_gradient_param per pixel, 8 wide along the row.
void cnvs_gradient_param_row(cnvs_gradient const *gr, int x0, float y, int n,
                             float *__counted_by(n) t_out);

// Vectorized colour evaluation for a row of solved parameters (the other half
// of the fill path; t is cnvs_gradient_param_row's output).  out[i] is
// bit-identical to cnvs_gradient_color_at(gr, t[i]) -- the exact piecewise-
// linear stop colour, not a quantized ramp sample -- except that t[i] < 0 (the
// row solver's "outside" sentinel) paints transparent black.  Eight pixels per
// step: the stop search runs as compares + lane selects across the pixels
// (docs/decisions/gradient-eval.md), so per-pixel cost is flat in practice for
// the 2-5 stops gradients actually have, and there is no per-fill ramp build
// or table to size.
void cnvs_gradient_color_row(cnvs_gradient const *gr,
                             float const *__counted_by(n) t, int n,
                             cnvs_unpremul *__counted_by(n) out);

// Unpremultiplied colour to paint at a device-space point, with `alpha` (global
// alpha) folded into the result's alpha.
cnvs_unpremul cnvs_gradient_sample(cnvs_gradient const *gr, cnvs_vec2 p, float alpha);

// Number of entries in a precomputed colour ramp.  1024 keeps the max deviation
// from the exact piecewise-linear ramp under ~0.4/255 of nearest-entry
// quantization, plus <~0.2/255 now that the stop lerp itself runs in _Float16
// (docs/decisions/color-axis.md) -- together still at the 8-bit rounding step,
// so a nearest-entry lookup is visually identical to evaluating
// cnvs_gradient_color_at per pixel, but skips the per-pixel stop search.
//
// Why nearest-entry and not interpolated: measured max error vs the exact ramp,
// for these stops, in 1/255 units --
//
//     entries   nearest   lerp
//        256      1.49     1.12
//        512      0.75     0.50
//       1024      0.37     0.12
//
// Lerp buys ~3x precision at a given size (or matches a size at half the entries:
// lerp@512 ~= nearest@1024).  We don't, because at 8-bit output nearest@1024 is
// already below the rounding step -- lerp would refine an error you can't see --
// while costing ~1.4x per lookup (two reads + a 4-channel _Float16 interpolation),
// and the ramp is only built for fills >= this many pixels, where that per-pixel
// cost outweighs the cheaper build of a smaller table.  Flip to lerp (and shrink N)
// if the output ever goes higher than 8-bit, or to halve the ramp's memory.
#define CNVS_GRAD_RAMP_N 1024

// Fill `ramp` with `n` evenly spaced samples of the gradient's colour ramp:
// ramp[i] == cnvs_gradient_color_at(gr, i / (n - 1)).  Built once per fill so the
// per-pixel paint loop can index it instead of rescanning the stops.
void cnvs_gradient_build_ramp(cnvs_gradient const *gr,
                              cnvs_unpremul *__counted_by(n) ramp, int n);
