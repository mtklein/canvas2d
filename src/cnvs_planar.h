#pragma once

// Planar (SoA) colour vocabulary: eight premultiplied pixels held as four
// 8-lane _Float16 channel planes -- one full 128-bit NEON register of native
// fp16 arithmetic per channel (docs/decisions/color-axis.md).  Planar is the
// pipeline's house layout: per-channel math needs no alpha-splat shuffles
// (sa IS a vector) and per-pixel branches become lane selects.  A cnvs_px8
// is a four-member homogeneous vector aggregate, so the arm64 ABI passes and
// returns it in q0-q3 -- stages can call each other with whole pixel blocks
// in registers.
//
// The AoS<->planar seams use explicit arm_neon.h ld4/st4 intrinsics (there
// is no portable spelling for a deinterleaving load).  arm_neon.h is
// unannotated, so the intrinsics' own pointer parameters carry no bounds --
// each wrapper below takes __counted_by(8) (or (32) for RGBA8), making the
// implicit conversion at every call site the bounds check: exactly one per
// 8-pixel block.

#include "cnvs_math.h"  // cnvs_premul

#include <arm_neon.h>
#include <ptrcheck.h>
#include <stdint.h>
#include <string.h>

typedef _Float16 cnvs_h8 __attribute__((ext_vector_type(8)));   // one channel plane
typedef _Float16 cnvs_h4 __attribute__((ext_vector_type(4)));   // one AoS pixel
typedef short    cnvs_m8 __attribute__((ext_vector_type(8)));   // f16 compare mask
typedef uint8_t  cnvs_b8 __attribute__((ext_vector_type(8)));   // one byte plane

// Eight pixels, channel-planar.
typedef struct {
    cnvs_h8 r, g, b, a;
} cnvs_px8;

// Bit-exact lane select: a where the mask lane is set (-1, from a vector
// comparison), else b.  Bitwise, not arithmetic (b + (a-b)*m), because the
// unselected lane may hold the inf/NaN of a guarded divide and must be
// discarded exactly -- this is how scalar `p ? q : r` branches translate to
// lanes without changing any selected value.
static inline cnvs_h8 cnvs_h8_sel(cnvs_m8 m, cnvs_h8 a, cnvs_h8 b) {
    return (cnvs_h8)(((cnvs_m8)a & m) | ((cnvs_m8)b & ~m));
}

// --- the f16 tile seam (cnvs_premul is four contiguous _Float16) ------------

static inline cnvs_px8 cnvs_px8_load(cnvs_premul const *__counted_by(8) p) {
    float16x8x4_t v = vld4q_f16((float16_t const *)p);
    return (cnvs_px8){ (cnvs_h8)v.val[0], (cnvs_h8)v.val[1],
                       (cnvs_h8)v.val[2], (cnvs_h8)v.val[3] };
}

static inline void cnvs_px8_store(cnvs_premul *__counted_by(8) p, cnvs_px8 px) {
    float16x8x4_t v = { { (float16x8_t)px.r, (float16x8_t)px.g,
                          (float16x8_t)px.b, (float16x8_t)px.a } };
    vst4q_f16((float16_t *)p, v);
}

// The k < 8 tail: gather k pixels into planes, zero-filling the rest.
// Transparent-black lanes are inert through every kernel (no divide here is
// unguarded), and the partial store writes only the first k pixels back.
static inline cnvs_px8 cnvs_px8_load_k(cnvs_premul const *__counted_by(k) p, int k) {
    cnvs_px8 o = { .r = (cnvs_h8)(_Float16)0.0f, .g = (cnvs_h8)(_Float16)0.0f,
                   .b = (cnvs_h8)(_Float16)0.0f, .a = (cnvs_h8)(_Float16)0.0f };
    for (int i = 0; i < k && i < 8; i++) {
        cnvs_h4 px;
        memcpy(&px, &p[i], sizeof px);  // one bounds check, four channels
        o.r[i] = px[0];
        o.g[i] = px[1];
        o.b[i] = px[2];
        o.a[i] = px[3];
    }
    return o;
}

static inline void cnvs_px8_store_k(cnvs_premul *__counted_by(k) p, int k,
                                    cnvs_px8 px) {
    for (int i = 0; i < k && i < 8; i++) {
        cnvs_h4 q = { px.r[i], px.g[i], px.b[i], px.a[i] };
        memcpy(&p[i], &q, sizeof q);
    }
}

// --- the RGBA8 seam (interleaved u8, one byte per channel) ------------------

// Eight RGBA8 pixels widened to planes of 0..255 (every u8 value is exact in
// _Float16); scaling to [0,1] is the caller's job, so the divide's rounding
// stays visible at the call site.
static inline cnvs_px8 cnvs_px8_load_rgba8(uint8_t const *__counted_by(32) p) {
    uint8x8x4_t v = vld4_u8(p);
    return (cnvs_px8){ __builtin_convertvector((cnvs_b8)v.val[0], cnvs_h8),
                       __builtin_convertvector((cnvs_b8)v.val[1], cnvs_h8),
                       __builtin_convertvector((cnvs_b8)v.val[2], cnvs_h8),
                       __builtin_convertvector((cnvs_b8)v.val[3], cnvs_h8) };
}

// Planes of finished byte values in [0, 255.5) narrowed (truncating, the
// convert's rounding) and re-interleaved to eight RGBA8 pixels.
static inline void cnvs_px8_store_rgba8(uint8_t *__counted_by(32) p, cnvs_px8 px) {
    uint8x8x4_t v = { { (uint8x8_t)__builtin_convertvector(px.r, cnvs_b8),
                        (uint8x8_t)__builtin_convertvector(px.g, cnvs_b8),
                        (uint8x8_t)__builtin_convertvector(px.b, cnvs_b8),
                        (uint8x8_t)__builtin_convertvector(px.a, cnvs_b8) } };
    vst4_u8(p, v);
}

static inline cnvs_px8 cnvs_px8_load_rgba8_k(uint8_t const *__counted_by(4 * k) p,
                                             int k) {
    cnvs_px8 o = { .r = (cnvs_h8)(_Float16)0.0f, .g = (cnvs_h8)(_Float16)0.0f,
                   .b = (cnvs_h8)(_Float16)0.0f, .a = (cnvs_h8)(_Float16)0.0f };
    for (int i = 0; i < k && i < 8; i++) {
        uint8_t px[4];
        memcpy(px, &p[i * 4], sizeof px);
        o.r[i] = (_Float16)px[0];
        o.g[i] = (_Float16)px[1];
        o.b[i] = (_Float16)px[2];
        o.a[i] = (_Float16)px[3];
    }
    return o;
}

static inline void cnvs_px8_store_rgba8_k(uint8_t *__counted_by(4 * k) p, int k,
                                          cnvs_px8 px) {
    typedef uint8_t cnvs_b4 __attribute__((ext_vector_type(4)));
    for (int i = 0; i < k && i < 8; i++) {
        cnvs_h4 v = { px.r[i], px.g[i], px.b[i], px.a[i] };
        cnvs_b4 q = __builtin_convertvector(v, cnvs_b4);
        memcpy(&p[i * 4], &q, sizeof q);
    }
}

// --- the unpremultiplied f16 seam (cnvs_unpremul, four contiguous _Float16) -

// Eight unpremultiplied colours deinterleaved to planes.  Like the RGBA8
// loaders above, the planes hold unpremultiplied values only on their way
// into cnvs_px8_premultiply -- the shade stage's fold is the one consumer.
static inline cnvs_px8 cnvs_px8_load_unpremul(cnvs_unpremul const *__counted_by(8) p) {
    float16x8x4_t v = vld4q_f16((float16_t const *)p);
    return (cnvs_px8){ (cnvs_h8)v.val[0], (cnvs_h8)v.val[1],
                       (cnvs_h8)v.val[2], (cnvs_h8)v.val[3] };
}

static inline cnvs_px8 cnvs_px8_load_unpremul_k(cnvs_unpremul const *__counted_by(k) p,
                                                int k) {
    cnvs_px8 o = { .r = (cnvs_h8)(_Float16)0.0f, .g = (cnvs_h8)(_Float16)0.0f,
                   .b = (cnvs_h8)(_Float16)0.0f, .a = (cnvs_h8)(_Float16)0.0f };
    for (int i = 0; i < k && i < 8; i++) {
        cnvs_h4 px;
        memcpy(&px, &p[i], sizeof px);  // one bounds check, four channels
        o.r[i] = px[0];
        o.g[i] = px[1];
        o.b[i] = px[2];
        o.a[i] = px[3];
    }
    return o;
}

// --- the coverage seam (one contiguous byte per pixel; no deinterleave) -----

static inline cnvs_h8 cnvs_h8_from_u8(uint8_t const *__counted_by(8) p) {
    cnvs_b8 b;
    memcpy(&b, p, sizeof b);  // one bounds check, eight samples
    return __builtin_convertvector(b, cnvs_h8);
}

static inline cnvs_h8 cnvs_h8_from_u8_k(uint8_t const *__counted_by(k) p, int k) {
    cnvs_h8 o = (cnvs_h8)(_Float16)0.0f;
    for (int i = 0; i < k && i < 8; i++) {
        o[i] = (_Float16)p[i];
    }
    return o;
}

// --- shared alpha math -------------------------------------------------------

// Attenuate all four planes (premultiplied scaling: alpha scales too).
static inline cnvs_px8 cnvs_px8_scale(cnvs_px8 p, cnvs_h8 k) {
    return (cnvs_px8){ p.r * k, p.g * k, p.b * k, p.a * k };
}

// Planar premultiply: scale the rgb planes by the alpha plane and clamp every
// plane -- alpha included -- to [0,1].  Lane-wise exactly cnvs_premultiply
// (cnvs_math.c), planes instead of one pixel's lanes.
static inline cnvs_px8 cnvs_px8_premultiply(cnvs_px8 p) {
    cnvs_h8 const zero = (cnvs_h8)(_Float16)0.0f, one = (cnvs_h8)(_Float16)1.0f;
    cnvs_px8 o = { p.r * p.a, p.g * p.a, p.b * p.a, p.a };
    o.r = __builtin_elementwise_min(one, __builtin_elementwise_max(zero, o.r));
    o.g = __builtin_elementwise_min(one, __builtin_elementwise_max(zero, o.g));
    o.b = __builtin_elementwise_min(one, __builtin_elementwise_max(zero, o.b));
    o.a = __builtin_elementwise_min(one, __builtin_elementwise_max(zero, o.a));
    return o;
}

// The compositor's output clamp: ao = min(a, 1) ('lighter' can exceed 1), and
// every channel -- alpha included -- pins into [0, ao], preserving the
// premultiplied invariant rgb <= a.
static inline cnvs_px8 cnvs_px8_clamp_premul(cnvs_px8 co) {
    cnvs_h8 const zero = (cnvs_h8)(_Float16)0.0f;
    cnvs_h8 ao = __builtin_elementwise_min(co.a, (cnvs_h8)(_Float16)1.0f);
    co.r = __builtin_elementwise_max(zero, __builtin_elementwise_min(ao, co.r));
    co.g = __builtin_elementwise_max(zero, __builtin_elementwise_min(ao, co.g));
    co.b = __builtin_elementwise_max(zero, __builtin_elementwise_min(ao, co.b));
    co.a = __builtin_elementwise_max(zero, __builtin_elementwise_min(ao, co.a));
    return co;
}
