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
