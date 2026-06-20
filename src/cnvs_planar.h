#pragma once

// Planar (SoA) colour vocabulary: a SLAB -- eight premultiplied pixels held
// as four 8-lane _Float16 channel planes, one full 128-bit NEON register of
// native fp16 arithmetic per channel (docs/decisions/color-axis.md).  Slab is
// the house word for this 8-pixel planar unit (block stays the format's and
// DEFLATE's word).  Planar is the pipeline's house layout: per-channel math needs no alpha-splat shuffles
// (sa IS a vector) and per-pixel branches become lane selects.  A cnvs_px8
// is a four-member homogeneous vector aggregate, so the arm64 ABI passes and
// returns it in q0-q3 -- stages can call each other with whole pixel slabs
// in registers.
//
// The AoS<->planar seams use explicit arm_neon.h ld4/st4 intrinsics (there
// is no portable spelling for a deinterleaving load).  arm_neon.h is
// unannotated, so the intrinsics' own pointer parameters carry no bounds --
// each wrapper below takes __counted_by(8) (or (32) for RGBA8), making the
// implicit conversion at every call site the bounds check: exactly one per
// 8-pixel slab.

#include "cnvs_math.h"  // cnvs_premul

#include <arm_neon.h>
#include <ptrcheck.h>
#include <stdint.h>
#include <string.h>

// One slab: eight pixels, channel-planar.  A half8 is one channel plane
// (cnvs_math.h's generic lane vocabulary; a half4 is one AoS pixel, a uchar8
// one byte plane).
typedef struct {
    half8 r, g, b, a;
} cnvs_px8;

// Bit-exact lane select: a where the mask lane is set (-1, from a vector
// comparison), else b.  Bitwise, not arithmetic (b + (a-b)*m), because the
// unselected lane may hold the inf/NaN of a guarded divide and must be
// discarded exactly -- this is how scalar `p ? q : r` branches translate to
// lanes without changing any selected value.
static inline half8 half8_if_then_else(short8 m, half8 a, half8 b) {
    return (half8)(((short8)a & m) | ((short8)b & ~m));
}

// --- the f16 tile seam (cnvs_premul is four contiguous _Float16) ------------

static inline cnvs_px8 cnvs_px8_load(cnvs_premul const *__counted_by(8) p) {
    float16x8x4_t v = vld4q_f16((float16_t const *)p);
    return (cnvs_px8){ (half8)v.val[0], (half8)v.val[1],
                       (half8)v.val[2], (half8)v.val[3] };
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
    cnvs_px8 o = { .r = (half8)(_Float16)0.0f, .g = (half8)(_Float16)0.0f,
                   .b = (half8)(_Float16)0.0f, .a = (half8)(_Float16)0.0f };
    for (int i = 0; i < k && i < 8; i++) {
        half4 px;
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
        half4 q = { px.r[i], px.g[i], px.b[i], px.a[i] };
        memcpy(&p[i], &q, sizeof q);
    }
}

// --- the RGBA8 seam (interleaved u8, one byte per channel) ------------------

// Eight RGBA8 pixels widened to planes of 0..255 (every u8 value is exact in
// _Float16); scaling to [0,1] is the caller's job, so the divide's rounding
// stays visible at the call site.
static inline cnvs_px8 cnvs_px8_load_rgba8(uint8_t const *__counted_by(32) p) {
    uint8x8x4_t v = vld4_u8(p);
    return (cnvs_px8){ __builtin_convertvector((uchar8)v.val[0], half8),
                       __builtin_convertvector((uchar8)v.val[1], half8),
                       __builtin_convertvector((uchar8)v.val[2], half8),
                       __builtin_convertvector((uchar8)v.val[3], half8) };
}

// Planes of finished byte values in [0, 255.5) narrowed (truncating, the
// convert's rounding) and re-interleaved to eight RGBA8 pixels.
static inline void cnvs_px8_store_rgba8(uint8_t *__counted_by(32) p, cnvs_px8 px) {
    uint8x8x4_t v = { { (uint8x8_t)__builtin_convertvector(px.r, uchar8),
                        (uint8x8_t)__builtin_convertvector(px.g, uchar8),
                        (uint8x8_t)__builtin_convertvector(px.b, uchar8),
                        (uint8x8_t)__builtin_convertvector(px.a, uchar8) } };
    vst4_u8(p, v);
}

static inline cnvs_px8 cnvs_px8_load_rgba8_k(uint8_t const *__counted_by(4 * k) p,
                                             int k) {
    cnvs_px8 o = { .r = (half8)(_Float16)0.0f, .g = (half8)(_Float16)0.0f,
                   .b = (half8)(_Float16)0.0f, .a = (half8)(_Float16)0.0f };
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
    for (int i = 0; i < k && i < 8; i++) {
        half4 const v = { px.r[i], px.g[i], px.b[i], px.a[i] };
        uchar4 q = __builtin_convertvector(v, uchar4);
        memcpy(&p[i * 4], &q, sizeof q);
    }
}

// --- the unpremultiplied f16 seam (cnvs_unpremul, four contiguous _Float16) -

// Eight unpremultiplied colours deinterleaved to planes.  Like the RGBA8
// loaders above, the planes hold unpremultiplied values only on their way
// into cnvs_px8_premultiply -- the shade stage's fold is the one consumer.
static inline cnvs_px8 cnvs_px8_load_unpremul(cnvs_unpremul const *__counted_by(8) p) {
    float16x8x4_t v = vld4q_f16((float16_t const *)p);
    return (cnvs_px8){ (half8)v.val[0], (half8)v.val[1],
                       (half8)v.val[2], (half8)v.val[3] };
}

static inline cnvs_px8 cnvs_px8_load_unpremul_k(cnvs_unpremul const *__counted_by(k) p,
                                                int k) {
    cnvs_px8 o = { .r = (half8)(_Float16)0.0f, .g = (half8)(_Float16)0.0f,
                   .b = (half8)(_Float16)0.0f, .a = (half8)(_Float16)0.0f };
    for (int i = 0; i < k && i < 8; i++) {
        half4 px;
        memcpy(&px, &p[i], sizeof px);  // one bounds check, four channels
        o.r[i] = px[0];
        o.g[i] = px[1];
        o.b[i] = px[2];
        o.a[i] = px[3];
    }
    return o;
}

// The store twin of cnvs_px8_load_unpremul: re-interleave eight straight RGBA
// colours back to contiguous cnvs_unpremul, st4 at the seam.  Structurally
// cnvs_px8_store to a cnvs_unpremul rather than a cnvs_premul; values pass
// through unbounded (the f16 readback emits extended colour).
static inline void cnvs_px8_store_unpremul(cnvs_unpremul *__counted_by(8) p,
                                           cnvs_px8 px) {
    float16x8x4_t v = { { (float16x8_t)px.r, (float16x8_t)px.g,
                          (float16x8_t)px.b, (float16x8_t)px.a } };
    vst4q_f16((float16_t *)p, v);
}

static inline void cnvs_px8_store_unpremul_k(cnvs_unpremul *__counted_by(k) p, int k,
                                             cnvs_px8 px) {
    for (int i = 0; i < k && i < 8; i++) {
        half4 q = { px.r[i], px.g[i], px.b[i], px.a[i] };
        memcpy(&p[i], &q, sizeof q);
    }
}

// --- the coverage seam (one contiguous byte per pixel; no deinterleave) -----

static inline half8 half8_from_u8(uint8_t const *__counted_by(8) p) {
    uchar8 b;
    memcpy(&b, p, sizeof b);  // one bounds check, eight samples
    return __builtin_convertvector(b, half8);
}

static inline half8 half8_from_u8_k(uint8_t const *__counted_by(k) p, int k) {
    half8 o = (half8)(_Float16)0.0f;
    for (int i = 0; i < k && i < 8; i++) {
        o[i] = (_Float16)p[i];
    }
    return o;
}

// --- shared alpha math -------------------------------------------------------

// Attenuate all four planes (premultiplied scaling: alpha scales too).
static inline cnvs_px8 cnvs_px8_scale(cnvs_px8 p, half8 k) {
    return (cnvs_px8){ p.r * k, p.g * k, p.b * k, p.a * k };
}

// Planar premultiply: scale the rgb planes by the alpha plane and clamp every
// plane -- alpha included -- to [0,1].  Lane-wise exactly cnvs_premultiply
// (cnvs_math.c), planes instead of one pixel's lanes.
static inline cnvs_px8 cnvs_px8_premultiply(cnvs_px8 p) {
    half8 const zero = (half8)(_Float16)0.0f, one = (half8)(_Float16)1.0f;
    cnvs_px8 o = { p.r * p.a, p.g * p.a, p.b * p.a, p.a };
    o.r = __builtin_elementwise_min(one, __builtin_elementwise_max(zero, o.r));
    o.g = __builtin_elementwise_min(one, __builtin_elementwise_max(zero, o.g));
    o.b = __builtin_elementwise_min(one, __builtin_elementwise_max(zero, o.b));
    o.a = __builtin_elementwise_min(one, __builtin_elementwise_max(zero, o.a));
    return o;
}

// Planar premultiply that keeps extended colour: scale the rgb planes by the
// alpha plane and clamp ONLY alpha into [0,1] (the surface-alpha invariant),
// leaving the colour planes unbounded in both directions.  The f16 putImageData
// deposit's premultiply, where HDR (>1) and wide-gamut (negative) colour must
// survive into the COPY blend.  A peer of cnvs_px8_premultiply (which clamps
// every plane to [0,1]); kept separate so the u8 path's clamp stays untouched.
static inline cnvs_px8 cnvs_px8_premultiply_unclamped(cnvs_px8 p) {
    half8 const zero = (half8)(_Float16)0.0f, one = (half8)(_Float16)1.0f;
    half8 const a = __builtin_elementwise_min(one, __builtin_elementwise_max(zero, p.a));
    return (cnvs_px8){ p.r * a, p.g * a, p.b * a, a };  // colour planes unbounded
}

// The blend stage's output clamp: ao = min(a, 1) ('lighter' can exceed 1), and
// every channel -- alpha included -- clamps into [0, ao], preserving the
// premultiplied invariant rgb <= a.
static inline cnvs_px8 cnvs_px8_clamp_premul(cnvs_px8 co) {
    half8 const zero = (half8)(_Float16)0.0f;
    half8 const ao = __builtin_elementwise_min(co.a, (half8)(_Float16)1.0f);
    co.r = __builtin_elementwise_max(zero, __builtin_elementwise_min(ao, co.r));
    co.g = __builtin_elementwise_max(zero, __builtin_elementwise_min(ao, co.g));
    co.b = __builtin_elementwise_max(zero, __builtin_elementwise_min(ao, co.b));
    co.a = __builtin_elementwise_max(zero, __builtin_elementwise_min(ao, co.a));
    return co;
}

// The LINEAR working space's blend output clamp: alpha clamps into [0,1] (so
// 'lighter' still saturates alpha and the readback's a>0 test stays meaningful),
// but the colour planes keep NO bound in either direction.  Extended linear sRGB
// carries colour both above the [0,1] gamut (HDR, brighter than white) and below
// it (wide gamut -- a Rec.2020 primary has negative sRGB-primary components); the
// only place either collapses is the output encode+quantize.  A NEW helper
// rather than a branch inside cnvs_px8_clamp_premul, so the sRGB path's exact
// [0,ao] rounding is provably untouched (reached only on a linear canvas).
static inline cnvs_px8 cnvs_px8_clamp_premul_lin(cnvs_px8 co) {
    half8 const zero = (half8)(_Float16)0.0f;
    co.a = __builtin_elementwise_max(zero,
               __builtin_elementwise_min(co.a, (half8)(_Float16)1.0f));
    return co;  // colour planes unbounded
}
