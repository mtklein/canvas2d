#pragma once

// RGBA8 <-> planar _Float16 channel vectors, shared by the pixel-VM backends so
// they (de)interleave identically -- the A/B/C comparison must differ only in
// dispatch, not in pixel I/O.  static inline because this is the per-chunk hot path
// in every backend: it has to inline, and it stays whole-vector -- lane-wise writes
// into a register file alias its other slots and make -Os spill the file (the
// artifact chased down in docs/pixel-pipelines.md).
//
// Channels are _Float16, but u8 <-> channel quantization runs in float: the divide
// and the *255 + 0.5 are exact there, and fp16's [0,1] resolution is fine enough
// (< 0.5/255 error) that the u8 round-trip is lossless.  Matches how the rest of
// the project converts colour (fp16 storage, float at the 8-bit boundary).

#include "pixvm.h"

typedef uint8_t u8x8  __attribute__((ext_vector_type(PIXVM_N)));
typedef uint8_t u8x16 __attribute__((ext_vector_type(PIXVM_N * 2)));
typedef uint8_t u8x32 __attribute__((ext_vector_type(PIXVM_N * 4)));
typedef float   f32xN __attribute__((ext_vector_type(PIXVM_N)));
static_assert(PIXVM_N == 8, "the RGBA8 (de)interleave shuffles assume 8 lanes");

static inline pixv pixio_unit(u8x8 v) {
    return __builtin_convertvector(__builtin_convertvector(v, f32xN) / 255.0f, pixv);
}

static inline u8x8 pixio_quant(pixv c) {
    f32xN f = __builtin_convertvector(c, f32xN);
    f32xN lo = 0.0f, hi = 1.0f;
    f = __builtin_elementwise_min(__builtin_elementwise_max(f, lo), hi);
    return __builtin_convertvector(f * 255.0f + 0.5f, u8x8);
}

// 8 RGBA8 pixels (r0g0b0a0 r1g1b1a1 ...) -> four unit-range channel vectors.
static inline void pixio_unpack(u8x32 raw, pixv *r, pixv *g, pixv *b, pixv *a) {
    *r = pixio_unit(__builtin_shufflevector(raw, raw, 0, 4,  8, 12, 16, 20, 24, 28));
    *g = pixio_unit(__builtin_shufflevector(raw, raw, 1, 5,  9, 13, 17, 21, 25, 29));
    *b = pixio_unit(__builtin_shufflevector(raw, raw, 2, 6, 10, 14, 18, 22, 26, 30));
    *a = pixio_unit(__builtin_shufflevector(raw, raw, 3, 7, 11, 15, 19, 23, 27, 31));
}

// Four channel vectors -> 8 interleaved RGBA8 pixels, clamped and quantized.
static inline u8x32 pixio_pack(pixv r, pixv g, pixv b, pixv a) {
    u8x8 r8 = pixio_quant(r);
    u8x8 g8 = pixio_quant(g);
    u8x8 b8 = pixio_quant(b);
    u8x8 a8 = pixio_quant(a);
    u8x16 rg = __builtin_shufflevector(r8, g8, 0, 8, 1, 9, 2, 10, 3, 11,
                                               4, 12, 5, 13, 6, 14, 7, 15);
    u8x16 ba = __builtin_shufflevector(b8, a8, 0, 8, 1, 9, 2, 10, 3, 11,
                                               4, 12, 5, 13, 6, 14, 7, 15);
    return __builtin_shufflevector(rg, ba, 0, 1, 16, 17, 2, 3, 18, 19,
                                           4, 5, 20, 21, 6, 7, 22, 23,
                                           8, 9, 24, 25, 10, 11, 26, 27,
                                           12, 13, 28, 29, 14, 15, 30, 31);
}

// Scalar u8 <- channel for the short final chunk.  v is widened from _Float16.
static inline uint8_t pixio_to_u8(float v) {
    if (v < 0.0f) { v = 0.0f; }
    if (v > 1.0f) { v = 1.0f; }
    return (uint8_t)(v * 255.0f + 0.5f);
}

// Scalar channel <- u8 for the short final chunk.
static inline _Float16 pixio_from_u8(uint8_t v) {
    return (_Float16)((float)v / 255.0f);
}
