#include "canvas2d_blur.h"

#include <string.h>

static int clampi(int v, int lo, int hi) {
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

// Exclusive 8-lane prefix sum (lane i = sum of lanes 0..i-1), in-register:
// shift in a zero, then the three Hillis-Steele shift-add steps (the coverage
// resolve's prefix_sum8, exclusive).
static inline int8 excl_prefix8(int8 v) {
    int8 const z = (int8){ 0, 0, 0, 0, 0, 0, 0, 0 };
    v  = __builtin_shufflevector(v, z, 8, 0, 1, 2, 3, 4, 5, 6);  // shift in the zero
    v += __builtin_shufflevector(v, z, 8, 0, 1, 2, 3, 4, 5, 6);  // += lane-1
    v += __builtin_shufflevector(v, z, 8, 8, 0, 1, 2, 3, 4, 5);  // += lane-2
    v += __builtin_shufflevector(v, z, 8, 8, 8, 8, 0, 1, 2, 3);  // += lane-4
    return v;
}

static inline int8 load8_widen(uint8_t const *__counted_by(8) p) {
    uchar8 b;
    memcpy(&b, p, sizeof b);  // one bounds check for all 8 samples
    return __builtin_convertvector(b, int8);
}

// Divide 8 window sums by the window EXACTLY, rounded: (sum + win/2) / win,
// matching the scalar integer division.  A float reciprocal multiply lands
// within +-1 of the true quotient (n < 2^24 is exact in float, and the
// relative error is ~2^-23), and one remainder comparison snaps it.  No NEON
// integer divide needed.
static inline uchar8 div_round8(int8 wsum, int win, int bias, float recip) {
    int8 const n = wsum + bias;
    float8 const f = __builtin_convertvector(n, float8);
    int8 q = __builtin_convertvector(f * recip, int8);  // truncates
    int8 const rem = n - q * win;
    q -= (rem >= win);  // comparison lanes are -1/0: snap a low guess up
    q += (rem < 0);     //                           ...and a high guess down
    return __builtin_convertvector(q, uchar8);
}

void canvas2d_blur_box_h(uint8_t *__counted_by(w * h) dst,
                uint8_t const *__counted_by(w * h) src, int w, int h, int r) {
    if (w <= 0 || h <= 0 || r < 0) {
        return;
    }
    int const win = 2 * r + 1;
    int const bias = win / 2;
    float const recip = 1.0f / (float)win;
    // div_round8's exactness needs n = 255*win + win/2 < 2^24; a window that wide
    // is degenerate (every output is the row average), so it keeps the
    // scalar loop.
    static_assert(255 * (2 * 32767 + 1) + 32767 < (1 << 24),
                  "div_round8 stays exact up to the vector-path radius cutoff");
    bool const wide = r < 32768;
    for (int y = 0; y < h; y++) {
        int const base = y * w;
        int sum = 0;
        for (int k = -r; k <= r; k++) {
            sum += src[base + clampi(k, 0, w - 1)];  // window centred at x = 0
        }
        int x = 0;
        // Left edge: the leaving index x-r still clamps to 0.
        for (; x < w && x < r; x++) {
            dst[base + x] = (uint8_t)((sum + bias) / win);
            int in  = base + clampi(x + r + 1, 0, w - 1);  // entering on the right
            int out = base + clampi(x - r,     0, w - 1);  // leaving on the left
            sum += (int)src[in] - (int)src[out];
        }
        // Interior: both window ends in-range, so no clamping -- 8 outputs per
        // step.  The entering samples e = src[x+r+1 ..] and leaving samples
        // l = src[x-r ..] load contiguously (one bounds check each), and the
        // exclusive prefix sum of e-l turns the serial running sum into eight
        // window sums at once; the carry to the next block is just lane math.
        if (wide) {
            for (; x + r + 9 <= w; x += 8) {
                int8 const e = load8_widen(src + base + x + r + 1);
                int8 const l = load8_widen(src + base + x - r);
                int8 const d = e - l;
                int8 const ws = sum + excl_prefix8(d);
                uchar8 q = div_round8(ws, win, bias, recip);
                memcpy(dst + base + x, &q, sizeof q);  // bounds-checked vector store
                sum = ws[7] + d[7];
            }
        }
        // Right edge (and the tail): the entering index clamps to w-1.
        for (; x < w; x++) {
            dst[base + x] = (uint8_t)((sum + bias) / win);
            int in  = base + clampi(x + r + 1, 0, w - 1);
            int const out = base + clampi(x - r,     0, w - 1);
            sum += (int)src[in] - (int)src[out];
        }
    }
}

void canvas2d_blur_box_v(uint8_t *__counted_by(w * h) dst,
                uint8_t const *__counted_by(w * h) src, int w, int h, int r) {
    if (w <= 0 || h <= 0 || r < 0) {
        return;
    }
    int const win = 2 * r + 1;
    int const bias = win / 2;
    float const recip = 1.0f / (float)win;
    int x = 0;
    // Eight adjacent columns at a time: columns are independent, so each lane
    // carries its own running sum -- no prefix sum needed, unlike the
    // horizontal pass.  The entering and leaving samples for all eight load
    // contiguously from one row (one bounds check each), and the row-index
    // clamps are shared by every lane, so even the edge rows run vectorized;
    // the only scalar work left is the w%8 tail columns.  r >= 32768 keeps
    // the scalar loop, as in the horizontal pass (div_round8 needs n < 2^24).
    if (r < 32768) {
        for (; x + 8 <= w; x += 8) {
            int8 sum = (int8){ 0, 0, 0, 0, 0, 0, 0, 0 };
            for (int k = -r; k <= r; k++) {
                sum += load8_widen(src + clampi(k, 0, h - 1) * w + x);  // window centred at y = 0
            }
            for (int y = 0; y < h; y++) {
                uchar8 q = div_round8(sum, win, bias, recip);
                memcpy(dst + y * w + x, &q, sizeof q);  // bounds-checked vector store
                int in  = clampi(y + r + 1, 0, h - 1) * w + x;  // entering below
                int out = clampi(y - r,     0, h - 1) * w + x;  // leaving above
                sum += load8_widen(src + in) - load8_widen(src + out);
            }
        }
    }
    for (; x < w; x++) {
        int sum = 0;
        for (int k = -r; k <= r; k++) {
            sum += src[clampi(k, 0, h - 1) * w + x];
        }
        for (int y = 0; y < h; y++) {
            dst[y * w + x] = (uint8_t)((sum + bias) / win);
            int in  = clampi(y + r + 1, 0, h - 1) * w + x;
            int const out = clampi(y - r,     0, h - 1) * w + x;
            sum += (int)src[in] - (int)src[out];
        }
    }
}

// --- subpixel shift passes ----------------------------------------------------
//
// A 2-tap lerp between each sample and its left (up) neighbour: max products
// 255*256 need 32-bit lanes, so these ride the box passes' load8_widen ->
// int8 idiom, eight pixels per step.  The first column (row) has no
// neighbour inside the mask and lerps against transparent zero.

void canvas2d_blur_shift_h(uint8_t *__counted_by(w * h) dst,
                  uint8_t const *__counted_by(w * h) src, int w, int h, int k) {
    if (w <= 0 || h <= 0 || k < 0 || k > 255) {
        return;
    }
    int const j = 256 - k;
    for (int y = 0; y < h; y++) {
        int const base = y * w;
        dst[base] = (uint8_t)((src[base] * j + 128) >> 8);  // in[-1] is transparent
        int x = 1;
        for (; x + 8 <= w; x += 8) {
            int8 const cur  = load8_widen(src + base + x);
            int8 const left = load8_widen(src + base + x - 1);
            uchar8 const q =
                __builtin_convertvector((cur * j + left * k + 128) >> 8, uchar8);
            memcpy(dst + base + x, &q, sizeof q);
        }
        for (; x < w; x++) {
            dst[base + x] = (uint8_t)(
                (src[base + x] * j + src[base + x - 1] * k + 128) >> 8);
        }
    }
}

void canvas2d_blur_shift_v(uint8_t *__counted_by(w * h) dst,
                  uint8_t const *__counted_by(w * h) src, int w, int h, int k) {
    if (w <= 0 || h <= 0 || k < 0 || k > 255) {
        return;
    }
    int const j = 256 - k;
    for (int y = 0; y < h; y++) {
        int const base = y * w, up = base - w;  // y = 0 lerps against transparent
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            int8 const cur   = load8_widen(src + base + x);
            int8 const above = y > 0 ? load8_widen(src + up + x)
                                     : (int8){ 0, 0, 0, 0, 0, 0, 0, 0 };
            uchar8 const q =
                __builtin_convertvector((cur * j + above * k + 128) >> 8, uchar8);
            memcpy(dst + base + x, &q, sizeof q);
        }
        for (; x < w; x++) {
            int const above = y > 0 ? src[up + x] : 0;
            dst[base + x] = (uint8_t)((src[base + x] * j + above * k + 128) >> 8);
        }
    }
}

// --- RGBA16F (premultiplied tile) passes -------------------------------------
//
// One pixel's four channels already form a vector lane group, so the natural
// widths differ from the u8 passes: the scalar unit is a 4-lane f32 vector (one
// pixel), and the wide unit is a 16-lane f32 vector (four pixels) -- the u8
// idiom with a pixel, not a byte, as the lane.  Out-of-tile samples are
// transparent black, which is *simpler* than the u8 clamp: the running sum
// adds/subtracts nothing outside, and every output still divides by the full
// window (the missing samples really are zeros, not replicated edge pixels).

// Load/store one premultiplied pixel (four contiguous _Float16) widened to f32:
// the memcpy is one bounds check for all four channels (the canvas2d_filter idiom).
static inline float4 load4f(canvas2d_premul const *__counted_by(1) p) {
    half4 v;
    memcpy(&v, p, sizeof v);
    return __builtin_convertvector(v, float4);
}

static inline void store4f(canvas2d_premul *__counted_by(1) p, float4 v) {
    half4 q = __builtin_convertvector(v, half4);
    memcpy(p, &q, sizeof q);
}

// Four adjacent pixels (16 lanes) at once; one bounds check covers all four.
static inline float16 load16f(canvas2d_premul const *__counted_by(4) p) {
    half16 v;
    memcpy(&v, p, sizeof v);
    return __builtin_convertvector(v, float16);
}

static inline void store16f(canvas2d_premul *__counted_by(4) p, float16 v) {
    half16 q = __builtin_convertvector(v, half16);
    memcpy(p, &q, sizeof q);
}

// Exclusive prefix sum over the four pixel groups of a 16-lane block (group g =
// sum of groups 0..g-1): excl_prefix8 with a pixel's four lanes as the unit --
// shift in a zero group, then the two Hillis-Steele shift-add steps.
static inline float16 excl_prefix4px(float16 v) {
    float16 const z = (float16)0.0f;
    v  = __builtin_shufflevector(v, z, 16, 17, 18, 19, 0, 1, 2, 3,
                                 4, 5, 6, 7, 8, 9, 10, 11);  // shift in the zero group
    v += __builtin_shufflevector(v, z, 16, 17, 18, 19, 0, 1, 2, 3,
                                 4, 5, 6, 7, 8, 9, 10, 11);  // += group-1
    v += __builtin_shufflevector(v, z, 16, 17, 18, 19, 16, 17, 18, 19,
                                 0, 1, 2, 3, 4, 5, 6, 7);    // += group-2
    return v;
}

// One pixel repeated across the four groups, and the last group extracted.
static inline float16 splat4px(float4 p) {
    return __builtin_shufflevector(p, p, 0, 1, 2, 3, 0, 1, 2, 3,
                                   0, 1, 2, 3, 0, 1, 2, 3);
}

static inline float4 group3(float16 v) {
    return __builtin_shufflevector(v, v, 12, 13, 14, 15);
}

void canvas2d_blur_box_h_f16(canvas2d_premul *__counted_by(w * h) dst,
                    canvas2d_premul const *__counted_by(w * h) src,
                    int w, int h, int r) {
    if (w <= 0 || h <= 0 || r < 0) {
        return;
    }
    float const recip = 1.0f / (float)(2 * r + 1);
    for (int y = 0; y < h; y++) {
        int const base = y * w;
        // Window centred at x = 0: only src[0..r] exist; outside is transparent.
        float4 sum = (float4)0.0f;
        int const top = r < w - 1 ? r : w - 1;
        for (int k = 0; k <= top; k++) {
            sum += load4f(src + base + k);
        }
        int x = 0;
        // Left edge: the leaving sample x-r is still outside the tile (nothing
        // to subtract); the entering one may be too (nothing to add).
        for (; x < w && x < r; x++) {
            store4f(dst + base + x, sum * recip);
            if (x + r + 1 < w) {
                sum += load4f(src + base + x + r + 1);
            }
        }
        // Interior: both window ends in range, four pixels per step.  The
        // entering and leaving pixels load contiguously (one bounds check for
        // four pixels each), and the exclusive prefix sum of e-l over pixel
        // groups turns the serial running sum into four window sums at once;
        // the carry to the next block is lane math.
        for (; x >= r && x + r + 4 < w; x += 4) {
            float16 const e = load16f(src + base + x + r + 1);
            float16 const l = load16f(src + base + x - r);
            float16 const d = e - l;
            float16 const ws = splat4px(sum) + excl_prefix4px(d);
            store16f(dst + base + x, ws * recip);
            sum = group3(ws) + group3(d);
        }
        // Right edge (and the tail): the entering sample leaves the tile.
        for (; x < w; x++) {
            store4f(dst + base + x, sum * recip);
            if (x + r + 1 < w) {
                sum += load4f(src + base + x + r + 1);
            }
            if (x - r >= 0) {
                sum -= load4f(src + base + x - r);
            }
        }
    }
}

void canvas2d_blur_box_v_f16(canvas2d_premul *__counted_by(w * h) dst,
                    canvas2d_premul const *__counted_by(w * h) src,
                    int w, int h, int r) {
    if (w <= 0 || h <= 0 || r < 0) {
        return;
    }
    float const recip = 1.0f / (float)(2 * r + 1);
    int const top = r < h - 1 ? r : h - 1;
    int x = 0;
    // Four adjacent pixels (16 lanes) per step: columns are independent, so
    // each lane carries its own running sum -- no prefix sum, as in the u8
    // pass.  The entering and leaving pixels for all four load contiguously
    // from one row, and the in-range tests are shared by every lane (they test
    // y, not x), so even the edge rows run vectorized; an out-of-tile row is
    // transparent and simply adds nothing.
    for (; x + 4 <= w; x += 4) {
        float16 sum = (float16)0.0f;
        for (int k = 0; k <= top; k++) {
            sum += load16f(src + k * w + x);
        }
        for (int y = 0; y < h; y++) {
            store16f(dst + y * w + x, sum * recip);
            if (y + r + 1 < h) {
                sum += load16f(src + (y + r + 1) * w + x);  // entering below
            }
            if (y - r >= 0) {
                sum -= load16f(src + (y - r)     * w + x);  // leaving above
            }
        }
    }
    // The w%4 tail columns, one pixel's four lanes at a time.
    for (; x < w; x++) {
        float4 sum = (float4)0.0f;
        for (int k = 0; k <= top; k++) {
            sum += load4f(src + k * w + x);
        }
        for (int y = 0; y < h; y++) {
            store4f(dst + y * w + x, sum * recip);
            if (y + r + 1 < h) {
                sum += load4f(src + (y + r + 1) * w + x);
            }
            if (y - r >= 0) {
                sum -= load4f(src + (y - r)     * w + x);
            }
        }
    }
}
