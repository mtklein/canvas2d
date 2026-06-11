#pragma once

#include <limits.h>
#include <stdint.h>

// Generic SIMD lane vocabulary, <element><lanes>.  mask8 is the f16-compare
// mask (a _Float16 comparison yields 16-bit lanes); f32 comparisons yield
// int4/int8.  Domain-specific vector types build on these (cnvs_planar.h).
typedef _Float16 half4   __attribute__((ext_vector_type(4)));
typedef _Float16 half8   __attribute__((ext_vector_type(8)));
typedef _Float16 half16  __attribute__((ext_vector_type(16)));
typedef float    float4  __attribute__((ext_vector_type(4)));
typedef float    float8  __attribute__((ext_vector_type(8)));
typedef float    float16 __attribute__((ext_vector_type(16)));
typedef int32_t  int4    __attribute__((ext_vector_type(4)));
typedef int32_t  int8    __attribute__((ext_vector_type(8)));
typedef short    mask8   __attribute__((ext_vector_type(8)));
typedef uint8_t  byte4   __attribute__((ext_vector_type(4)));
typedef uint8_t  byte8   __attribute__((ext_vector_type(8)));

// 2D affine transforms (HTML Canvas 2D convention).  A matrix maps (x, y) to
//     (a*x + c*y + e,  b*x + d*y + f),
// the (a, b, c, d, e, f) of CanvasRenderingContext2D.setTransform().

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
    float a, b, c, d, e, f;
} cnvs_mat;

cnvs_mat cnvs_mat_identity(void);

// mat_apply(mat_mul(m, n), p) == apply(m, apply(n, p)): n is applied first, as
// when Canvas chains translate() then scale().
cnvs_mat cnvs_mat_mul(cnvs_mat m, cnvs_mat n);

cnvs_mat cnvs_mat_translate(float tx, float ty);
cnvs_mat cnvs_mat_scale(float sx, float sy);
cnvs_mat cnvs_mat_rotate(float radians);

cnvs_vec2 cnvs_mat_apply(cnvs_mat m, cnvs_vec2 p);

// Inverse; identity if (near-)singular.
cnvs_mat cnvs_mat_invert(cnvs_mat m);

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
