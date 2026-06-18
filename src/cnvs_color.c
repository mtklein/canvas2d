#include "cnvs_color.h"

#include <math.h>

// All arithmetic here is f32: the transfer's pow and Oklab's cube root/cube are
// the precision-sensitive nonlinear math docs/decisions/color-axis.md reserves
// f32 for.  A caller holding _Float16 colour widens at the call and narrows the
// result -- the narrowing is the storage boundary, not these kernels' business.
//
// Vectorization deferred: every kernel below is a per-lane transcendental
// (powf / cbrtf) behind a data-dependent branch, with no portable vector
// spelling -- libm has no half8 pow or cbrt, and a branch-to-select rewrite
// would carry both arms of the transfer through the slow path for no measured
// win (these conversions are not on a profiled hot loop today; the planar
// pipeline in cnvs_planar.h is the place to add a slab variant if one ever
// is).  Scalar correctness and totality are the deliverable here.

// The standard sRGB piecewise thresholds and constants.
#define SRGB_DECODE_KNEE 0.04045f
#define SRGB_ENCODE_KNEE 0.0031308f
#define SRGB_SLOPE       12.92f
#define SRGB_ALPHA       1.055f
#define SRGB_OFFSET      0.055f
#define SRGB_GAMMA       2.4f

// sign(x)*f(|x|): the odd extension that makes the transfer total over R.  The
// magnitude goes through the standard curve; the sign rides back on at the end.
// copysignf carries the sign even for x == 0 (signed zero round-trips), and f is
// finite for all m >= 0, so the result is finite for all x.
float cnvs_srgb_to_linear(float c) {
    float const m = fabsf(c);
    float const l = m <= SRGB_DECODE_KNEE
                  ? m / SRGB_SLOPE
                  : powf((m + SRGB_OFFSET) / SRGB_ALPHA, SRGB_GAMMA);
    return copysignf(l, c);
}

float cnvs_linear_to_srgb(float l) {
    float const m = fabsf(l);
    float const c = m <= SRGB_ENCODE_KNEE
                  ? SRGB_SLOPE * m
                  : SRGB_ALPHA * powf(m, 1.0f / SRGB_GAMMA) - SRGB_OFFSET;
    return copysignf(c, l);
}

cnvs_rgb cnvs_rgb_srgb_to_linear(cnvs_rgb c) {
    return (cnvs_rgb){ .r = cnvs_srgb_to_linear(c.r),
                       .g = cnvs_srgb_to_linear(c.g),
                       .b = cnvs_srgb_to_linear(c.b) };
}

cnvs_rgb cnvs_rgb_linear_to_srgb(cnvs_rgb c) {
    return (cnvs_rgb){ .r = cnvs_linear_to_srgb(c.r),
                       .g = cnvs_linear_to_srgb(c.g),
                       .b = cnvs_linear_to_srgb(c.b) };
}

// Oklab (Ottosson).  M1 to LMS, cube ROOT each, M2 to Lab; the inverse runs the
// transposed pipeline.  cbrtf, never powf(x, 1/3): cbrtf is total and
// sign-preserving, so a negative extended-linear LMS value (out-of-gamut input)
// cube-roots to a real negative rather than NaN -- the totality the header
// promises and the round-trip test exercises.
cnvs_oklab cnvs_linear_srgb_to_oklab(cnvs_rgb c) {
    float const l = 0.4122214708f * c.r + 0.5363325363f * c.g + 0.0514459929f * c.b;
    float const m = 0.2119034982f * c.r + 0.6806995451f * c.g + 0.1073969566f * c.b;
    float const s = 0.0883024619f * c.r + 0.2817188376f * c.g + 0.6299787005f * c.b;

    float const l_ = cbrtf(l);
    float const m_ = cbrtf(m);
    float const s_ = cbrtf(s);

    return (cnvs_oklab){
        .L = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
        .a = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
        .b = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_,
    };
}

cnvs_rgb cnvs_oklab_to_linear_srgb(cnvs_oklab c) {
    float const l_ = c.L + 0.3963377774f * c.a + 0.2158037573f * c.b;
    float const m_ = c.L - 0.1055613458f * c.a - 0.0638541728f * c.b;
    float const s_ = c.L - 0.0894841775f * c.a - 1.2914855480f * c.b;

    float const l = l_ * l_ * l_;
    float const m = m_ * m_ * m_;
    float const s = s_ * s_ * s_;

    return (cnvs_rgb){
        .r =  4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
        .g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
        .b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s,
    };
}
