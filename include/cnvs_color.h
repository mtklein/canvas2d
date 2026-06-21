#pragma once

// Colour-space conversion kernels: the sRGB transfer function and the
// linear-sRGB <-> Oklab pair.  These are pure functions on RGB triples; alpha
// is a separate coordinate and never passes through them.
//
// Every function here is TOTAL over the reals -- finite in, finite out, for
// negative and >1 inputs alike.  That is the deliberate property of an extended
// (scRGB-style) colour hub: extended linear sRGB carries colours outside the
// [0,1] gamut, so the transfer must be defined there too.  The transfer is the
// odd extension sign(x)*f(|x|), and Oklab uses cbrtf (total, sign-preserving)
// rather than powf(x, 1/3) (NaN on x<0) so the round trip survives out-of-gamut
// colour.
//
// Precision discipline (docs/decisions/color-axis.md, float16-color-type.md):
// _Float16 is storage and the bulk-compute type, but the transfer function's
// pow and Oklab's cube root/cube are precision-sensitive nonlinear math, so they
// run in f32 internally and narrow only at the storage boundary.  These scalar
// kernels are therefore f32 in / f32 out; a caller that holds _Float16 colour
// widens at the call and narrows the result.

#include "cnvs_math.h"

// Colour channels are _Float16 -- the narrowest storage type for which the
// spec's 8-bit edges round-trip exactly: every (u8 colour, u8 alpha) pair
// survives premultiply -> f16 store -> unpremultiply -> 8-bit quantize
// unchanged (all 65,280; a u8 premultiplied store corrupts half of them), at
// half float32's footprint -- see docs/decisions/float16-color-type.md.
// Per docs/decisions/color-axis.md, _Float16 is the COMPUTE type too: the
// blend, filter, gradient-lerp, premultiply, and readback kernels do their
// arithmetic in f16, with no widen/narrow converts at the load/store
// boundaries.  The bulk kernels are PLANAR over that type (cnvs_planar.h);
// the per-pixel converters stay one pixel's four lanes.
//
// Two types so premultiplied and unpremultiplied colour can't be mixed up:
// cnvs_unpremul is what the Canvas API speaks (r,g,b independent of a); cnvs_premul
// is what internal pixel buffers hold (r,g,b scaled by a).  Convert only through
// cnvs_premultiply / cnvs_unpremultiply, never by reinterpreting the shared layout.
typedef struct cnvs_unpremul {
    _Float16 r, g, b, a;
} cnvs_unpremul;

typedef struct cnvs_premul {
    _Float16 r, g, b, a;
} cnvs_premul;

// Build an unpremultiplied colour; the float -> _Float16 narrowing site.
cnvs_unpremul cnvs_unpremul_of(float r, float g, float b, float a);

// premultiply scales rgb by a; unpremultiply divides it back (a == 0 -> all-zero).
// Both clamp to [0,1].
cnvs_premul cnvs_premultiply(cnvs_unpremul c);
cnvs_unpremul cnvs_unpremultiply(cnvs_premul c);

typedef struct {
    float r, g, b;
} cnvs_rgb;

typedef struct {
    float L, a, b;
} cnvs_oklab;

// sRGB transfer, the standard piecewise curve, extended to the whole real line
// as the odd function sign(x)*f(|x|).  decode maps encoded (gamma) sRGB to
// linear; encode is its inverse.  Total over R: f(-x) == -f(x), and both are
// finite everywhere (no pow of a negative).
//
//   decode:  c <= 0.04045 ? c/12.92 : ((c+0.055)/1.055)^2.4
//   encode:  l <= 0.0031308 ? 12.92*l : 1.055*l^(1/2.4) - 0.055
//
// decode(encode(l)) == l and encode(decode(c)) == c to f32 rounding.
float cnvs_srgb_to_linear(float c);
float cnvs_linear_to_srgb(float l);

// Per-channel transfer over an RGB triple (alpha is the caller's, untouched).
cnvs_rgb cnvs_rgb_srgb_to_linear(cnvs_rgb c);
cnvs_rgb cnvs_rgb_linear_to_srgb(cnvs_rgb c);

// linear sRGB <-> Oklab (Bjorn Ottosson, https://bottosson.github.io/posts/oklab/).
// Total over R^3: the cube root is cbrtf, so negative extended-linear inputs
// stay finite rather than turning into NaN, and the pair round-trips in- and
// out-of-gamut colour to f32 tolerance.  Linear white (1,1,1) maps to
// L=1, a=0, b=0; black maps to the origin.
cnvs_oklab cnvs_linear_srgb_to_oklab(cnvs_rgb c);
cnvs_rgb   cnvs_oklab_to_linear_srgb(cnvs_oklab c);

// linear sRGB (Rec.709 primaries) -> linear Rec.2020 primaries (BT.2087).  The
// sRGB gamut sits inside Rec.2020, so in-gamut inputs land in [0,1]; extended
// (wide-gamut / HDR) inputs carry through unclamped.
cnvs_rgb cnvs_linear_srgb_to_rec2020(cnvs_rgb c);

// Rec.2020 primaries -> linear sRGB (Rec.709), the inverse of the above.  A
// Rec.2020-gamut colour maps to extended (out-of-[0,1]) linear sRGB -- the way
// to author wide-gamut content on a linear canvas.  Round-trips with the forward
// to f32 tolerance.
cnvs_rgb cnvs_rec2020_to_linear_srgb(cnvs_rgb c);

// SMPTE ST 2084 (PQ) opto-electrical transfer: display luminance normalized to
// [0,1] over 0..10000 cd/m^2 -> encoded [0,1].  Unlike the kernels above this is
// a bounded display transfer, not total over R: out-of-range inputs clamp to
// [0,1] (PQ has no meaning outside it).  cnvs_pq_oetf(0)=0, (1)=1, monotone.
float cnvs_pq_oetf(float y);

// 8-wide cnvs_pq_oetf: lane-for-lane bit-identical to the scalar (mirrors its
// exact expression tree -- same coefficients, same Horner, same boundary
// selects), so the encoder runs it on whole planes without shifting an output
// byte.  This is the vectorizable form the scalar's polynomial design exists for.
float8 cnvs_pq_oetf8(float8 y);
