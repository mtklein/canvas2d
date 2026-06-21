#include "canvas2d_color.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

// All arithmetic here is f32: the transfer's pow and Oklab's cube root/cube are
// the precision-sensitive nonlinear math docs/decisions/color-axis.md reserves
// f32 for.  A caller holding _Float16 colour widens at the call and narrows the
// result -- the narrowing is the storage boundary, not these kernels' business.
//
// Vectorization deferred: every kernel below is a per-lane transcendental
// (powf / cbrtf) behind a data-dependent branch, with no portable vector
// spelling -- libm has no f16x8 pow or cbrt, and a branch-to-select rewrite
// would carry both arms of the transfer through the slow path for no measured
// benefit (these conversions are not on a profiled hot loop today; the planar
// pipeline in canvas2d_planar.h is the place to add a slab variant if one ever
// is).  Scalar correctness and totality are what these kernels provide.
//
// Exception: canvas2d_pq_oetf uses only arithmetic and integer bit manipulation
// (no libm calls), so the compiler can vectorize a loop over it -- see that
// function's comment for the design.

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
float canvas2d_srgb_to_linear(float c) {
    float const m = fabsf(c);
    float const l = m <= SRGB_DECODE_KNEE
                  ? m / SRGB_SLOPE
                  : powf((m + SRGB_OFFSET) / SRGB_ALPHA, SRGB_GAMMA);
    return copysignf(l, c);
}

float canvas2d_linear_to_srgb(float l) {
    float const m = fabsf(l);
    float const c = m <= SRGB_ENCODE_KNEE
                  ? SRGB_SLOPE * m
                  : SRGB_ALPHA * powf(m, 1.0f / SRGB_GAMMA) - SRGB_OFFSET;
    return copysignf(c, l);
}

canvas2d_rgb canvas2d_rgb_srgb_to_linear(canvas2d_rgb c) {
    return (canvas2d_rgb){ .r = canvas2d_srgb_to_linear(c.r),
                       .g = canvas2d_srgb_to_linear(c.g),
                       .b = canvas2d_srgb_to_linear(c.b) };
}

canvas2d_rgb canvas2d_rgb_linear_to_srgb(canvas2d_rgb c) {
    return (canvas2d_rgb){ .r = canvas2d_linear_to_srgb(c.r),
                       .g = canvas2d_linear_to_srgb(c.g),
                       .b = canvas2d_linear_to_srgb(c.b) };
}

// Oklab (Ottosson).  M1 to LMS, cube ROOT each, M2 to Lab; the inverse runs the
// transposed pipeline.  cbrtf, never powf(x, 1/3): cbrtf is total and
// sign-preserving, so a negative extended-linear LMS value (out-of-gamut input)
// cube-roots to a real negative rather than NaN -- the totality the header
// promises and the round-trip test exercises.
canvas2d_oklab canvas2d_linear_srgb_to_oklab(canvas2d_rgb c) {
    float const l = 0.4122214708f * c.r + 0.5363325363f * c.g + 0.0514459929f * c.b;
    float const m = 0.2119034982f * c.r + 0.6806995451f * c.g + 0.1073969566f * c.b;
    float const s = 0.0883024619f * c.r + 0.2817188376f * c.g + 0.6299787005f * c.b;

    float const l_ = cbrtf(l);
    float const m_ = cbrtf(m);
    float const s_ = cbrtf(s);

    return (canvas2d_oklab){
        .L = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
        .a = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
        .b = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_,
    };
}

canvas2d_rgb canvas2d_oklab_to_linear_srgb(canvas2d_oklab c) {
    float const l_ = c.L + 0.3963377774f * c.a + 0.2158037573f * c.b;
    float const m_ = c.L - 0.1055613458f * c.a - 0.0638541728f * c.b;
    float const s_ = c.L - 0.0894841775f * c.a - 1.2914855480f * c.b;

    float const l = l_ * l_ * l_;
    float const m = m_ * m_ * m_;
    float const s = s_ * s_ * s_;

    return (canvas2d_rgb){
        .r =  4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
        .g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
        .b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s,
    };
}

// The BT.2087 709->2020 matrix: a basis change through XYZ folded to one 3x3.
// Each row sums to 1, so D65 white (1,1,1) maps to itself.  Linear in, linear
// out, so it is total over R like the kernels above.
canvas2d_rgb canvas2d_linear_srgb_to_rec2020(canvas2d_rgb c) {
    return (canvas2d_rgb){
        .r = 0.62740390f * c.r + 0.32928304f * c.g + 0.04331307f * c.b,
        .g = 0.06909729f * c.r + 0.91954040f * c.g + 0.01136232f * c.b,
        .b = 0.01639144f * c.r + 0.08801331f * c.g + 0.89559525f * c.b,
    };
}

// The BT.2087 2020->709 matrix, the inverse of the above.  Off-diagonals are
// negative, so a saturated Rec.2020 colour lands outside [0,1] in linear sRGB --
// exactly the extended values the linear working space is for.
canvas2d_rgb canvas2d_rec2020_to_linear_srgb(canvas2d_rgb c) {
    return (canvas2d_rgb){
        .r =  1.66049100f * c.r - 0.58764114f * c.g - 0.07284986f * c.b,
        .g = -0.12455047f * c.r + 1.13289990f * c.g - 0.00834942f * c.b,
        .b = -0.01815076f * c.r - 0.10057890f * c.g + 1.11872966f * c.b,
    };
}

// PQ (SMPTE ST 2084) OETF.  y is display luminance normalized so 1.0 == 10000
// cd/m^2; clamp into [0,1] (PQ is undefined outside) and apply the standard
// rational-power curve.  E' = ((c1 + c2 y^m1) / (1 + c3 y^m1))^m2.
//
// The original two-powf form is not vectorizable: libm powf has no SIMD
// spelling.  This implementation uses the identity a^b = exp2(b*log2(a)) and
// replaces both transcendentals with minimax polynomial approximations over a
// restricted range, built from IEEE 754 bit manipulation alone (frexp/ldexp
// style via memcpy, integer shifts and masks).  All operations have vector
// equivalents, so a caller loop that inlines this function (e.g., with LTO)
// can be auto-vectorized by the compiler.
//
// The computation uses two log2 helpers with different domains:
//
//   pq_log2_wide(y): log2 for y in (0, 1] (the display luminance).
//     Frexpf decomposition: y = m * 2^e, m in [0.5, 1.0).
//     log2(y) = e + log2(m).
//     log2(m) = (m - 0.5) * Q(m) - 1  -- Q is a degree-5 fit for
//       (log2(m)+1)/(m-0.5), exact at m=0.5 (i.e. exact for y=1.0 which
//       yields e=1, m=0.5 -> log2(y) = 1 + (-1) = 0).
//
//   pq_log2_frac(frac): log2 for frac = (c1+c2*t)/(1+c3*t) in [0.836, 1.0].
//     frac is always in [0.836, 1.0] for t in (0, 1] (the biased exponent is
//     always 126, so no frexp decomposition is needed).
//     log2(frac) = (frac - 1) * R(frac)  -- R is a degree-5 fit for
//       log2(frac)/(frac-1) over [0.836, 1.0], exact at frac=1.
//     This narrow domain gives much better accuracy than a single [0.5,1)
//     poly for the inner power (frac^m2 where m2=78.84 amplifies log2 error).
//
//   pq_exp2(x): exp2 for x in [-21, 0] (both computed arguments land here).
//     Floor decomposition: x = n + f, n in {-21..0}, f in [0, 1).
//     exp2(x) = 2^n * exp2(f), 2^n via biased exponent, exp2(f) = 1+f*G(f).
//     G is a degree-5 fit for (exp2(f)-1)/f, exact at f=0.
//
// Accuracy (double-precision reference sweep over 10000 points in [0,1]):
//   max |approx - ref|  < 2e-5  (~1.3 16-bit codes)
//   SDR-white anchor (y = 203/10000 = 0.0203): error < 3e-6 (~0.2 codes)
//
// The approximation is monotone over [0,1] and exact at y=0 (from the clamp)
// and y=1 (from the exact polynomial endpoint constraints).

static float pq_log2_wide(float y) {
    // frexpf-style decomposition: y = m * 2^e, m in [0.5, 1.0).
    // The IEEE 754 bias for e is 127, so biased_exp = e + 127.  To get m in
    // [0.5, 1.0), replace the exponent field with 126 (= 2^-1 * significand
    // gives [0.5, 1.0)).  Then e = biased_exp - 126 (not -127: the frexpf
    // exponent counts the power of 2 applied to the [0.5,1) mantissa).
    uint32_t bits;
    memcpy(&bits, &y, sizeof bits);
    int32_t const e = (int32_t)((bits >> 23) & 0xFFu) - 126;
    bits = (bits & 0x007FFFFFu) | (126u << 23);
    float m;
    memcpy(&m, &bits, sizeof m);
    // Minimax polynomial Q for (log2(m)+1)/(m-0.5) over [0.5, 1.0], degree 5.
    // The (m-0.5) factor forces log2(0.5) = -1 exactly, which makes log2_wide
    // return 0 exactly when y=1.0 (e=1, m=0.5 -> 1 + (-1) = 0).
    // Coefficients fitted by least squares on 1000 Chebyshev nodes.
    float const q = (((( -2.2140944730f  * m
                        +10.2211130607f) * m
                        -19.7612251545f) * m
                        +20.8321998273f) * m
                        -13.3117024570f) * m
                        + 6.2336918059f;
    return (float)e + (m - 0.5f) * q - 1.0f;
}

static float pq_log2_frac(float frac) {
    // frac = (c1 + c2*t) / (1 + c3*t) is always in [0.836, 1.0] for t in (0,1].
    // Its biased exponent is always 126 (since frac in [0.5, 1.0)), so no
    // exponent extraction is needed -- this is just a polynomial in frac.
    // Minimax polynomial R for log2(frac)/(frac-1) over [0.836, 1.0], degree 5.
    // The (frac-1) factor forces log2(1) = 0 exactly, making the boundary PQ=1
    // at y=1 exact.  Narrow domain [0.836, 1] allows better accuracy than the
    // full-range [0.5, 1) poly for the m2=78.84 amplification.
    // Coefficients fitted by least squares on 1000 Chebyshev nodes.
    float const r = ((((  -0.2517282200f  * frac
                         + 1.5710494790f) * frac
                         - 4.1226418001f) * frac
                         + 5.9401468216f) * frac
                         - 5.2592650306f) * frac
                         + 3.5651338172f;
    return (frac - 1.0f) * r;
}

static float pq_exp2(float x) {
    // Split x into integer and fractional parts.  x is always <= 0 in the PQ
    // context, so (int32_t)x truncates toward zero instead of flooring; the
    // conditional subtract converts to floor without calling floorf (which
    // triggers -Wbad-function-cast when cast to int under -Weverything).
    int32_t const n = (int32_t)x - (x < (float)(int32_t)x ? 1 : 0);
    float const f = x - (float)n;
    // Construct 2^n via biased exponent.  n is in [-21, 0] so biased = n+127
    // is in [106, 127] -- all normal floats, never denormal or inf.
    uint32_t const pow2_bits = (uint32_t)(n + 127) << 23;
    float pow2;
    memcpy(&pow2, &pow2_bits, sizeof pow2);
    // Minimax polynomial G for (exp2(f)-1)/f over [0, 1), degree 5.
    // The 1+f*... form forces exp2(0) = 1 exactly.
    // Coefficients fitted by least squares on 1000 Chebyshev nodes.
    float const g = (((( 0.0002082919f  * f
                       + 0.0012689354f) * f
                       + 0.0096524405f) * f
                       + 0.0554959362f) * f
                       + 0.2402272150f) * f
                       + 0.6931471706f;
    return (1.0f + f * g) * pow2;
}

float canvas2d_pq_oetf(float y) {
    float const m1 = 0.1593017578125f, m2 = 78.84375f;
    float const c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    if (y <= 0.0f) { return 0.0f; }
    if (y >= 1.0f) { return 1.0f; }
    // t = y^m1 via log2/exp2 (no libm transcendentals).
    float const t = pq_exp2(m1 * pq_log2_wide(y));
    // Inner fraction in (c1/(1+c3), 1] for t in (0, 1]; at t=1 exactly 1.
    float const frac = (c1 + c2 * t) / (1.0f + c3 * t);
    // frac^m2 via the narrow-domain log2 (exact at frac=1) and exp2.
    return pq_exp2(m2 * pq_log2_frac(frac));
}

// 8-wide forms of the three helpers and the OETF, each a direct transliteration
// of its scalar twin: identical coefficients in identical Horner order (so the
// compiler applies the same FMA contraction per lane) and the IEEE bit twiddling
// as vector reinterpret (memcpy f32x8<->i32x8) plus elementwise int ops.  The
// shifts are masked (&0xFF) or operate on values the domain keeps non-negative
// and small (n+127 in [106,147], so (n+127)<<23 never reaches bit 31), so signed
// lanes behave like the scalar's uint32.  Result: bit-identical to the scalar
// lane for lane -- test_colorspace's pq_oetf8_matches_scalar pins it exactly.
static f32x8 pq_log2_wide8(f32x8 y) {
    i32x8 bits;
    memcpy(&bits, &y, sizeof bits);
    i32x8 const e = ((bits >> 23) & 0xFF) - 126;
    bits = (bits & 0x007FFFFF) | (126 << 23);
    f32x8 m;
    memcpy(&m, &bits, sizeof m);
    f32x8 const q = (((( -2.2140944730f  * m
                        +10.2211130607f) * m
                        -19.7612251545f) * m
                        +20.8321998273f) * m
                        -13.3117024570f) * m
                        + 6.2336918059f;
    return __builtin_convertvector(e, f32x8) + (m - 0.5f) * q - 1.0f;
}

static f32x8 pq_log2_frac8(f32x8 frac) {
    f32x8 const r = ((((  -0.2517282200f  * frac
                         + 1.5710494790f) * frac
                         - 4.1226418001f) * frac
                         + 5.9401468216f) * frac
                         - 5.2592650306f) * frac
                         + 3.5651338172f;
    return (frac - 1.0f) * r;
}

static f32x8 pq_exp28(f32x8 x) {
    i32x8 const xi = __builtin_convertvector(x, i32x8);  // (int32_t)x: trunc toward 0
    // floor: the f32 compare yields a 0/-1 mask, so adding it subtracts 1 where
    // x < trunc(x) -- the scalar's `xi - (x < (float)xi ? 1 : 0)`.
    i32x8 const n = xi + (x < __builtin_convertvector(xi, f32x8));
    f32x8 const f = x - __builtin_convertvector(n, f32x8);
    i32x8 const pow2_bits = (n + 127) << 23;
    f32x8 pow2;
    memcpy(&pow2, &pow2_bits, sizeof pow2);
    f32x8 const g = (((( 0.0002082919f  * f
                       + 0.0012689354f) * f
                       + 0.0096524405f) * f
                       + 0.0554959362f) * f
                       + 0.2402272150f) * f
                       + 0.6931471706f;
    return (1.0f + f * g) * pow2;
}

f32x8 canvas2d_pq_oetf8(f32x8 y) {
    float const m1 = 0.1593017578125f, m2 = 78.84375f;
    float const c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    f32x8 const t = pq_exp28(m1 * pq_log2_wide8(y));
    f32x8 const frac = (c1 + c2 * t) / (1.0f + c3 * t);
    f32x8 e = pq_exp28(m2 * pq_log2_frac8(frac));
    // The scalar returns early outside [0,1]; the vector computes every lane then
    // blends.  Mandatory, not cosmetic: a negative y (a wide-gamut Rec.2020
    // component) computes to a large finite value, not 0, without this select.
    // ext_vector has no `?:` in C, so reinterpret to int lanes and mask-select:
    // y>=1 -> 1.0f bits, y<=0 -> 0 bits (= +0.0f, what the scalar returns).
    i32x8 const ge1 = (y >= 1.0f);  // 0 / -1 per lane
    i32x8 const le0 = (y <= 0.0f);
    f32x8 const onef = (f32x8)1.0f;
    i32x8 eb, ones;
    memcpy(&eb, &e, sizeof eb);
    memcpy(&ones, &onef, sizeof ones);
    eb = (ge1 & ones) | (~ge1 & eb);  // y>=1 -> 1.0f
    eb = ~le0 & eb;                    // y<=0 -> +0.0f
    memcpy(&e, &eb, sizeof e);
    return e;
}
