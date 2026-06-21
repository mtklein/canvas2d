#pragma once

#include <limits.h>
#include <stdint.h>

// Generic SIMD lane vocabulary, <element><lanes> (OpenCL-style: the suffix is the
// lane count, the element is the C base type, so int8 is 8 int32 lanes, not an
// int8_t).  short8 is the f16-compare mask (a _Float16 comparison yields 16-bit
// lanes); f32 comparisons yield int4/int8.  The unsigned byte/word types serve
// the codec kernels (PNG filter + serialize, zlib adler/crc).  Domain-specific
// vector types build on these (cnvs_planar.h).
typedef _Float16 half4    __attribute__((ext_vector_type(4)));
typedef _Float16 half8    __attribute__((ext_vector_type(8)));
typedef _Float16 half16   __attribute__((ext_vector_type(16)));
typedef float    float4   __attribute__((ext_vector_type(4)));
typedef float    float8   __attribute__((ext_vector_type(8)));
typedef float    float16  __attribute__((ext_vector_type(16)));
typedef int32_t  int4     __attribute__((ext_vector_type(4)));
typedef int32_t  int8     __attribute__((ext_vector_type(8)));
typedef uint32_t uint16   __attribute__((ext_vector_type(16)));
typedef short    short8   __attribute__((ext_vector_type(8)));
typedef uint16_t ushort8  __attribute__((ext_vector_type(8)));
typedef uint8_t  uchar4   __attribute__((ext_vector_type(4)));
typedef uint8_t  uchar8   __attribute__((ext_vector_type(8)));
typedef uint8_t  uchar16  __attribute__((ext_vector_type(16)));

// 2D projective (homography) transforms, a deliberate extension beyond the HTML
// Canvas 2D affine spec (docs/decisions/perspective.md).  A matrix maps (x, y) to
//     x' = (a*x + c*y + e) / w,  y' = (b*x + d*y + f) / w,  w = g*x + h*y + i.
// Affine is the (g, h, i) = (0, 0, 1) subset -- then w == 1, no divide, and the
// (a, b, c, d, e, f) are the CanvasRenderingContext2D.setTransform() six.  The
// affine subset stays on a divide-free fast path so existing scenes compute bit
// for bit as before.

typedef struct {
    float x, y;
} cnvs_vec2;

// Colour channels are _Float16 -- the narrowest storage type for which the
// spec's 8-bit edges round-trip exactly: every (u8 colour, u8 alpha) pair
// survives premultiply -> f16 store -> unpremultiply -> 8-bit quantize
// unchanged (all 65,280; a u8 premultiplied store corrupts half of them), at
// half float32's footprint -- see docs/decisions/float16-color-type.md.
// Per docs/decisions/color-axis.md, _Float16 is the COMPUTE type too: the
// blend, filter, gradient-lerp, premultiply, and readback kernels do their
// arithmetic in f16, with no widen/narrow converts at the load/store
// boundaries.  The bulk kernels are PLANAR over that type (cnvs_planar.h);
// this header's per-pixel converters stay one pixel's four lanes.
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
    float a, b, c, d, e, f, g, h, i;
} cnvs_mat;

cnvs_mat cnvs_mat_identity(void);

// Whether the bottom row is (0, 0, 1) -- the affine subset, on which apply/mul/
// invert take a divide-free path that reproduces the old 2x3 arithmetic exactly.
bool cnvs_mat_is_affine(cnvs_mat m);

// mat_apply(mat_mul(m, n), p) == apply(m, apply(n, p)): n is applied first, as
// when Canvas chains translate() then scale().
cnvs_mat cnvs_mat_mul(cnvs_mat m, cnvs_mat n);

// translate/scale/rotate build affine matrices (g = h = 0, i = 1); their (a..f)
// values are unchanged from the 2x3 era.
cnvs_mat cnvs_mat_translate(float tx, float ty);
cnvs_mat cnvs_mat_scale(float sx, float sy);
cnvs_mat cnvs_mat_rotate(float radians);

// Apply to (x, y): affine maps with no divide (bit-identical to the 2x3 era);
// projective divides by w.
cnvs_vec2 cnvs_mat_apply(cnvs_mat m, cnvs_vec2 p);

// Inverse; identity if (near-)singular.  Affine inputs yield the affine inverse
// bit-identically.
cnvs_mat cnvs_mat_invert(cnvs_mat m);

// clamp01 -- THE clamp (D1, docs/vocabulary.md): the output is guaranteed in
// [0,1] no matter what comes in, NaN included.  !(v > 0) catches <= 0 and NaN
// in one test, so NaN launders to 0 rather than flowing through a min/max
// chain that would pass it along.  One definition, one home: every clamp01 in
// the tree is this one.
static inline float cnvs_clamp01(float v) {
    if (!(v > 0.0f)) {   // <= 0, or NaN
        return 0.0f;
    }
    return v > 1.0f ? 1.0f : v;
}

// The 8-lane twin, same guarantee: __builtin_elementwise_max/min return the
// other operand when one is NaN (maxnum semantics), so max(0, NaN) is 0 and
// every lane lands in [0,1] -- lane for lane the scalar above.
static inline float8 float8_clamp01(float8 v) {
    v = __builtin_elementwise_max((float8)0.0f, v);
    return __builtin_elementwise_min((float8)1.0f, v);
}

// Saturating float->integer conversions for the rasterizer.  Plain float->int is
// undefined behaviour when the value is non-finite or out of range; these clamp
// instead (NaN -> 0), keeping the device-space casts total on adversarial input.
static inline int cnvs_f2i(float v) {
    if (v != v) {                  // NaN
        return 0;
    }
    if (v >= (float)INT_MAX) {     // (float)INT_MAX rounds to 2^31, just above it
        return INT_MAX;
    }
    if (v <= (float)INT_MIN) {
        return INT_MIN;
    }
    return (int)v;
}

static inline uint8_t cnvs_f2u8(float v) {
    if (!(v > 0.0f)) {             // <= 0, or NaN
        return 0;
    }
    if (v >= 255.0f) {
        return 255;
    }
    return (uint8_t)v;
}
