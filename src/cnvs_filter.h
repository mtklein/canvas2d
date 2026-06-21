#pragma once

// The CSS filter colour functions (Filter Effects spec), as per-pixel kernels
// over a premultiplied RGBA16F tile.  Every colour function has a closed
// premultiplied form -- a linear rgb matrix commutes with premultiplication, and
// the offset terms scale by alpha -- so no unpremultiply pass is ever needed:
// with p = premultiplied rgb and a = alpha, each function is
//     rgb' = M * rgb + off * a,    a' = ka * a   (ka == 1 except opacity()).
//
// Each function compiles to that (M, off, ka) triple once, at
// canvas_add_filter_* time, not per apply: the same list typically paints many
// ops, and precompiling keeps the per-pixel loop one matrix-multiply-add per
// function.  Adjacent functions are NOT fused into one matrix: the spec clamps
// each function's result to [0,1] unpremultiplied (here: rgb to [0,a]) before
// the next function runs, and that clamp is nonlinear.

#include "cnvs_color.h"  // cnvs_premul

#include <ptrcheck.h>

// One compiled filter function (see above).  m is row-major: row i of rgb' is
// m[3i]*r + m[3i+1]*g + m[3i+2]*b + off[i]*a.  blur() and drop-shadow() do not
// fit the matrix form -- they are spatial kernels, not per-pixel ones -- so an
// entry carries kind tags: blur > 0 alone marks a blur() entry of that box
// radius, which the canvas pipeline runs as separable box passes over the
// whole tile, and shadow marks a drop-shadow() entry (blur is then its blur
// radius, possibly 0, with dx/dy/color below).  cnvs_filter_apply handles only
// colour entries; both spatial kinds keep an identity matrix so a mishandled
// one degrades to a no-op rather than blacking the tile out.
typedef struct {
    float m[9];      // 3x3 rgb matrix
    float off[3];    // offset column, scaled by the pixel's (incoming) alpha
    float ka;        // alpha scale: 1 for everything but opacity()
    int blur;        // box radius of blur() / drop-shadow(); 0 = no blurring
    bool shadow;     // marks a drop-shadow() entry: dx/dy/color apply
    int dx, dy;      // drop-shadow offset, whole-device-pixel floor
    int kx, ky;      // ...and its 1/256th-px fraction numerators, [0, 256)
    float color[4];  // drop-shadow colour, unpremultiplied RGBA in [0,1]
} cnvs_filter;

// Compile one filter function.  Amounts arrive already validated and clamped by
// the canvas API (finite; >= 0; <= 1 where the spec caps), so the builders are
// pure math.  brightness/contrast/saturate/opacity are identity at amount 1,
// grayscale/invert/sepia at 0, hue_rotate at 0 radians.
cnvs_filter cnvs_filter_brightness(float amount);
cnvs_filter cnvs_filter_contrast(float amount);
cnvs_filter cnvs_filter_grayscale(float amount);
cnvs_filter cnvs_filter_hue_rotate(float radians);
cnvs_filter cnvs_filter_invert(float amount);
cnvs_filter cnvs_filter_opacity(float amount);
cnvs_filter cnvs_filter_saturate(float amount);
cnvs_filter cnvs_filter_sepia(float amount);

// A blur() entry with box radius `radius` (> 0; the canvas API maps the CSS
// stdDev to a radius and skips zero-radius blurs).  Its matrix part is the
// identity -- see the struct comment.
cnvs_filter cnvs_filter_blur(int radius);

// A drop-shadow() entry: shadow offset split per axis into a whole-device-
// pixel floor and a [0, 256) fraction numerator on the 1/256th-px grid (the
// canvas API's shadow_offset_split, shared with shadowOffset{X,Y}), blur box
// radius `radius` (>= 0 -- a sharp shadow is valid, unlike blur()), and the
// shadow colour, unpremultiplied and already clamped to [0,1] by the canvas
// API.  Matrix part identity, like blur().
cnvs_filter cnvs_filter_drop_shadow(int dx, int kx, int dy, int ky, int radius,
                                    float r, float g, float b, float a);

// Apply `count` *colour* filter functions, in list order, to the n
// premultiplied pixels in place (the caller splits the list around blur()
// entries).  After each function the alpha is clamped to [0,1] and the rgb
// lanes to [0, a] -- the premultiplied image of the spec's per-function [0,1]
// unpremultiplied clamp.
void cnvs_filter_apply(cnvs_filter const *__counted_by(count) list, int count,
                       cnvs_premul *__counted_by(n) px, int n);
