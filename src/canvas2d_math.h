#pragma once

#include <limits.h>
#include <stdint.h>

// Generic SIMD lane vocabulary, {u,i,f}{bits}x{lanes}: element kind (unsigned /
// signed / float), element bit width, then the lane count.  So i32x8 is 8 lanes
// of int32_t, u8x16 is 16 lanes of uint8_t, f16x4 is 4 lanes of _Float16.
// i16x8 is the f16-compare mask (a _Float16 comparison yields 16-bit lanes);
// f32 comparisons yield i32x4/i32x8.  The unsigned byte/word types serve the
// codec kernels (PNG filter + serialize, zlib adler/crc).  Domain-specific
// vector types build on these (canvas2d_planar.h).
typedef _Float16 f16x4   __attribute__((ext_vector_type(4)));
typedef _Float16 f16x8   __attribute__((ext_vector_type(8)));
typedef _Float16 f16x16  __attribute__((ext_vector_type(16)));
typedef float    f32x4   __attribute__((ext_vector_type(4)));
typedef float    f32x8   __attribute__((ext_vector_type(8)));
typedef float    f32x16  __attribute__((ext_vector_type(16)));
typedef int32_t  i32x4   __attribute__((ext_vector_type(4)));
typedef int32_t  i32x8   __attribute__((ext_vector_type(8)));
typedef uint32_t u32x16  __attribute__((ext_vector_type(16)));
typedef short    i16x8   __attribute__((ext_vector_type(8)));
typedef uint16_t u16x8   __attribute__((ext_vector_type(8)));
typedef uint8_t  u8x4    __attribute__((ext_vector_type(4)));
typedef uint8_t  u8x8    __attribute__((ext_vector_type(8)));
typedef uint8_t  u8x16   __attribute__((ext_vector_type(16)));

// clamp01 -- THE clamp (D1, docs/vocabulary.md): the output is guaranteed in
// [0,1] no matter what comes in, NaN included.  !(v > 0) catches <= 0 and NaN
// in one test, so NaN launders to 0 rather than flowing through a min/max
// chain that would pass it along.  One definition, one home: every clamp01 in
// the tree is this one.
static inline float canvas2d_clamp01(float v) {
    if (!(v > 0.0f)) {   // <= 0, or NaN
        return 0.0f;
    }
    return v > 1.0f ? 1.0f : v;
}

// The 8-lane twin, same guarantee: __builtin_elementwise_max/min return the
// other operand when one is NaN (maxnum semantics), so max(0, NaN) is 0 and
// every lane lands in [0,1] -- lane for lane the scalar above.
static inline f32x8 f32x8_clamp01(f32x8 v) {
    v = __builtin_elementwise_max((f32x8)0.0f, v);
    return __builtin_elementwise_min((f32x8)1.0f, v);
}

// Saturating float->integer conversions for the rasterizer.  Plain float->int is
// undefined behaviour when the value is non-finite or out of range; these clamp
// instead (NaN -> 0), keeping the device-space casts total on adversarial input.
static inline int canvas2d_f2i(float v) {
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

static inline uint8_t canvas2d_f2u8(float v) {
    if (!(v > 0.0f)) {             // <= 0, or NaN
        return 0;
    }
    if (v >= 255.0f) {
        return 255;
    }
    return (uint8_t)v;
}
