#pragma once

// Linear and radial colour gradients.  Stops are kept sorted by offset in a small
// fixed array, so a gradient is a plain value copied with the canvas state on
// save/restore.  Fills and strokes sample it per pixel on the CPU into the tile the
// blend stage composites.

#include "canvas.h"     // enum canvas_color_space, enum canvas_alpha_type
#include "cnvs_color.h"  // cnvs_unpremul
#include "cnvs_matrix.h" // cnvs_mat, cnvs_vec2

#include <ptrcheck.h>

#define CNVS_STOPS_MAX 16

enum cnvs_grad_kind { CNVS_GRAD_LINEAR, CNVS_GRAD_RADIAL, CNVS_GRAD_CONIC };

typedef struct {
    float offset;  // in [0,1]
    cnvs_unpremul color;
} cnvs_stop;

// Coordinates and radii are device space (the CTM is baked in when the gradient
// is created) -- the affine fast path solves the parameter directly in device
// space, so the device-space p0/p1/r0/r1/angle drive it.
//
// Under a PERSPECTIVE CTM the device->user map is not affine, so a device pixel
// cannot be mapped to a single user point by a baked-in matrix.  The perspective
// path instead keeps the gradient in USER space (up0/up1/ur0/ur1/uangle, the raw
// API arguments, no CTM baked in) plus the device->user inverse homography
// (to_user); per pixel it maps the device centre back to user space (the
// perspective-correct u/w, v/w) and solves the parameter there.  `persp` is set
// when the creating CTM was non-affine and selects that path; affine gradients
// leave it false and run the device-space solver bit for bit as before.
struct cnvs_gradient {
    enum cnvs_grad_kind kind;
    cnvs_vec2 p0, p1;
    float r0, r1;
    float angle;   // conic: start angle (radians), device space; unused otherwise
    bool persp;            // creating CTM was perspective: solve in user space
    cnvs_vec2 up0, up1;    // user-space endpoints (perspective path)
    float ur0, ur1;        // user-space radii (perspective path)
    float uangle;          // user-space conic start angle (perspective path)
    cnvs_mat to_user;      // device -> user inverse homography (perspective path)
    cnvs_stop stops[CNVS_STOPS_MAX];
    int stop_count;
    // Interpolation is TWO orthogonal knobs (a 2D grid, not a single mode):
    //   interp -- the SPACE the colour coords lerp in: sRGB, linear sRGB, or
    //             Oklab.  All three are valid.
    //   interp_alpha -- whether the colour coords are premultiplied by alpha
    //             before the lerp (PREMUL: a transparent stop contributes no
    //             colour) or lerped directly (UNPREMUL).  Alpha itself always
    //             lerps linearly, on its own.
    // The DEFAULT (CANVAS_CS_SRGB + CANVAS_ALPHA_UNPREMUL, the zero values for
    // a designated-initializer gradient) reproduces the legacy straight stored-
    // value lerp byte-for-byte on an sRGB working canvas.
    //
    // The non-default combinations take the stored stop colours (which live in
    // the canvas WORKING space) to the interpolation space and back at eval
    // time.  `space` is the canvas working space, stamped on at create time --
    // it is immutable on the canvas, so reading cv->space once at set time is
    // exactly as correct as threading the canvas into every eval call, and
    // keeps the colour kernels (cnvs_gradient_color_at/_row) dependent only on
    // the gradient, not the canvas.
    enum canvas_color_space interp;       // the gradient lerp SPACE
    enum canvas_alpha_type  interp_alpha; // premultiply colour coords before lerp?
    enum canvas_color_space space;        // the canvas working space (sRGB or linear)
};

// Insert a stop in offset order (offset clamped to [0,1]); a no-op once full.
void cnvs_gradient_add_stop(struct cnvs_gradient *gr, float offset, cnvs_unpremul color);

// Colour at parameter t (clamped to [0,1]), piecewise-linear across the stops.
// With no stops the result is transparent black.
cnvs_unpremul cnvs_gradient_color_at(struct cnvs_gradient const *gr, float t);

// Per spec, a zero-length linear gradient and a radial gradient whose two
// circles coincide paint NOTHING (the whole draw is skipped).  Exact ==, not
// epsilon: a tiny-but-nonzero gradient still paints.
bool cnvs_gradient_paints_nothing(struct cnvs_gradient const *gr);

// Gradient parameter for a device-space point, written to *t (clamped to
// [0,1]).  Returns false when a radial point lies outside the gradient (no
// circle in the family passes through it) -- such samples paint transparent.
bool cnvs_gradient_param(struct cnvs_gradient const *gr, cnvs_vec2 p, float *__single t);

// Gradient parameter for a USER-space point, solved against the gradient's
// user-space def (up0/up1/ur0/ur1/uangle) -- the perspective path, where a
// device pixel is mapped back to user space (perspective-correct) before the
// solve.  Same math as cnvs_gradient_param, only the geometry coords differ.
bool cnvs_gradient_param_user(struct cnvs_gradient const *gr, cnvs_vec2 p,
                              float *__single t);

// Vectorized parameter solve for a horizontal run of `n` pixel centres
// (x0 + i + 0.5, y).  Fills t_out[i] with the parameter in [0,1], or -1 where the
// point has no parameter (radial "outside") so the caller paints transparent.
// Equivalent to calling cnvs_gradient_param per pixel, 8 wide along the row.
void cnvs_gradient_param_row(struct cnvs_gradient const *gr, int x0, float y, int n,
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
void cnvs_gradient_color_row(struct cnvs_gradient const *gr,
                             float const *__counted_by(n) t, int n,
                             cnvs_unpremul *__counted_by(n) out);

// Unpremultiplied colour to paint at a device-space point, with `alpha` (global
// alpha) folded into the result's alpha.
cnvs_unpremul cnvs_gradient_sample(struct cnvs_gradient const *gr, cnvs_vec2 p, float alpha);
