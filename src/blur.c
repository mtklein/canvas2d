#include "blur.h"

#include <string.h>

// Rows ahead to prefetch in the strided vertical pass (one cache line is several
// rows apart, so we reach well ahead of the running window).
#define BLUR_PF_ROWS 16

static int clampi(int v, int lo, int hi) {
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

typedef int32_t blr32x8 __attribute__((ext_vector_type(8)));
typedef float blrf8 __attribute__((ext_vector_type(8)));
typedef uint8_t blru8x8 __attribute__((ext_vector_type(8)));

// Exclusive 8-lane prefix sum (lane i = sum of lanes 0..i-1), in-register:
// shift in a zero, then the three Hillis-Steele shift-add steps (the coverage
// resolve's prefix_sum8, exclusive).
static inline blr32x8 excl_prefix8(blr32x8 v) {
    blr32x8 z = (blr32x8){ 0, 0, 0, 0, 0, 0, 0, 0 };
    v = __builtin_shufflevector(v, z, 8, 0, 1, 2, 3, 4, 5, 6);   // shift in the zero
    v += __builtin_shufflevector(v, z, 8, 0, 1, 2, 3, 4, 5, 6);  // += lane-1
    v += __builtin_shufflevector(v, z, 8, 8, 0, 1, 2, 3, 4, 5);  // += lane-2
    v += __builtin_shufflevector(v, z, 8, 8, 8, 8, 0, 1, 2, 3);  // += lane-4
    return v;
}

static inline blr32x8 load8_widen(uint8_t const *__counted_by(8) p) {
    blru8x8 b;
    memcpy(&b, p, sizeof b);  // one bounds check for all 8 samples
    return __builtin_convertvector(b, blr32x8);
}

// Quantize 8 window sums to (sum + win/2) / win exactly, matching the scalar
// integer division: a float reciprocal multiply lands within +-1 of the true
// quotient (n < 2^24 is exact in float, and the relative error is ~2^-23), and
// one remainder comparison snaps it.  No NEON integer divide needed.
static inline blru8x8 quant8(blr32x8 wsum, int win, int half, float recip) {
    blr32x8 n = wsum + half;
    blrf8 f = __builtin_convertvector(n, blrf8);
    blr32x8 q = __builtin_convertvector(f * recip, blr32x8);  // truncates
    blr32x8 rem = n - q * win;
    q -= (rem >= win);  // comparison lanes are -1/0: snap a low guess up
    q += (rem < 0);     //                           ...and a high guess down
    return __builtin_convertvector(q, blru8x8);
}

void blur_box_h(uint8_t *__counted_by(w * h) dst,
                uint8_t const *__counted_by(w * h) src, int w, int h, int r) {
    if (w <= 0 || h <= 0 || r < 0) {
        return;
    }
    int win = 2 * r + 1;
    int half = win / 2;
    float recip = 1.0f / (float)win;
    // quant8's exactness argument needs n = 255*win + win/2 < 2^24; a window
    // that wide is degenerate (every output is the row average), so it keeps
    // the scalar loop.
    bool wide = r < 32768;
    for (int y = 0; y < h; y++) {
        int base = y * w;
        int sum = 0;
        for (int k = -r; k <= r; k++) {
            sum += src[base + clampi(k, 0, w - 1)];  // window centred at x = 0
        }
        int x = 0;
        // Left edge: the leaving index x-r still clamps to 0.
        for (; x < w && x < r; x++) {
            dst[base + x] = (uint8_t)((sum + half) / win);
            int in = base + clampi(x + r + 1, 0, w - 1);   // entering on the right
            int out = base + clampi(x - r, 0, w - 1);      // leaving on the left
            sum += (int)src[in] - (int)src[out];
        }
        // Interior: both window ends in-range, so no clamping -- 8 outputs per
        // step.  The entering samples e = src[x+r+1 ..] and leaving samples
        // l = src[x-r ..] load contiguously (one bounds check each), and the
        // exclusive prefix sum of e-l turns the serial running sum into eight
        // window sums at once; the carry to the next block is just lane math.
        if (wide) {
            for (; x + r + 9 <= w; x += 8) {
                blr32x8 e = load8_widen(src + base + x + r + 1);
                blr32x8 l = load8_widen(src + base + x - r);
                blr32x8 d = e - l;
                blr32x8 ws = sum + excl_prefix8(d);
                blru8x8 q = quant8(ws, win, half, recip);
                memcpy(dst + base + x, &q, sizeof q);  // bounds-checked vector store
                sum = ws[7] + d[7];
            }
        }
        // Right edge (and the tail): the entering index clamps to w-1.
        for (; x < w; x++) {
            dst[base + x] = (uint8_t)((sum + half) / win);
            int in = base + clampi(x + r + 1, 0, w - 1);
            int out = base + clampi(x - r, 0, w - 1);
            sum += (int)src[in] - (int)src[out];
        }
    }
}

void blur_box_v(uint8_t *__counted_by(w * h) dst,
                uint8_t const *__counted_by(w * h) src, int w, int h, int r) {
    if (w <= 0 || h <= 0 || r < 0) {
        return;
    }
    int win = 2 * r + 1;
    int half = win / 2;
    float recip = 1.0f / (float)win;
    int x = 0;
    // Eight adjacent columns at a time: columns are independent, so each lane
    // carries its own running sum -- no prefix sum needed, unlike the
    // horizontal pass.  The entering and leaving samples for all eight load
    // contiguously from one row (one bounds check each), and the row-index
    // clamps are shared by every lane, so even the edge rows run vectorized;
    // the only scalar work left is the w%8 tail columns.  r >= 32768 keeps
    // the scalar loop, as in the horizontal pass (quant8 needs n < 2^24).
    if (r < 32768) {
        for (; x + 8 <= w; x += 8) {
            blr32x8 sum = (blr32x8){ 0, 0, 0, 0, 0, 0, 0, 0 };
            for (int k = -r; k <= r; k++) {
                sum += load8_widen(src + clampi(k, 0, h - 1) * w + x);  // window centred at y = 0
            }
            for (int y = 0; y < h; y++) {
                blru8x8 q = quant8(sum, win, half, recip);
                memcpy(dst + y * w + x, &q, sizeof q);  // bounds-checked vector store
                int in = clampi(y + r + 1, 0, h - 1) * w + x;   // entering below
                int out = clampi(y - r, 0, h - 1) * w + x;      // leaving above
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
            dst[y * w + x] = (uint8_t)((sum + half) / win);
            int in = clampi(y + r + 1, 0, h - 1) * w + x;
            int out = clampi(y - r, 0, h - 1) * w + x;
            sum += (int)src[in] - (int)src[out];
        }
    }
}

void blur_box_v_pf(uint8_t *__counted_by(w * h) dst,
                   uint8_t const *__counted_by(w * h) src, int w, int h, int r) {
    if (w <= 0 || h <= 0 || r < 0) {
        return;
    }
    int win = 2 * r + 1;
    int half = win / 2;
    float recip = 1.0f / (float)win;
    int x = 0;
    if (r < 32768) {
        for (; x + 8 <= w; x += 8) {
            blr32x8 sum = (blr32x8){ 0, 0, 0, 0, 0, 0, 0, 0 };
            for (int k = -r; k <= r; k++) {
                sum += load8_widen(src + clampi(k, 0, h - 1) * w + x);
            }
            for (int y = 0; y < h; y++) {
                blru8x8 q = quant8(sum, win, half, recip);
                memcpy(dst + y * w + x, &q, sizeof q);
                // Prefetch the source row BLUR_PF_ROWS iterations ahead of the
                // running window -- the address is clamped in-bounds, so the only
                // question is whether -fbounds-safety treats the prefetch specially.
                int pf = clampi(y + r + 1 + BLUR_PF_ROWS, 0, h - 1) * w + x;
                __builtin_prefetch(&src[pf], 0, 0);
                int in = clampi(y + r + 1, 0, h - 1) * w + x;
                int out = clampi(y - r, 0, h - 1) * w + x;
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
            dst[y * w + x] = (uint8_t)((sum + half) / win);
            int pf = clampi(y + r + 1 + BLUR_PF_ROWS, 0, h - 1) * w + x;
            __builtin_prefetch(&src[pf], 0, 0);
            int in = clampi(y + r + 1, 0, h - 1) * w + x;
            int out = clampi(y - r, 0, h - 1) * w + x;
            sum += (int)src[in] - (int)src[out];
        }
    }
}
