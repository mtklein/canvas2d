#include "blur.h"

// Rows ahead to prefetch in the strided vertical pass (one cache line is several
// rows apart, so we reach well ahead of the running window).
#define BLUR_PF_ROWS 16

static int clampi(int v, int lo, int hi) {
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

void blur_box_h(uint8_t *__counted_by(w * h) dst,
                uint8_t const *__counted_by(w * h) src, int w, int h, int r) {
    if (w <= 0 || h <= 0 || r < 0) {
        return;
    }
    int win = 2 * r + 1;
    for (int y = 0; y < h; y++) {
        int base = y * w;
        int sum = 0;
        for (int k = -r; k <= r; k++) {
            sum += src[base + clampi(k, 0, w - 1)];  // window centred at x = 0
        }
        for (int x = 0; x < w; x++) {
            dst[base + x] = (uint8_t)((sum + win / 2) / win);
            int in = base + clampi(x + r + 1, 0, w - 1);   // entering on the right
            int out = base + clampi(x - r, 0, w - 1);      // leaving on the left
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
    for (int x = 0; x < w; x++) {
        int sum = 0;
        for (int k = -r; k <= r; k++) {
            sum += src[clampi(k, 0, h - 1) * w + x];  // window centred at y = 0
        }
        for (int y = 0; y < h; y++) {
            dst[y * w + x] = (uint8_t)((sum + win / 2) / win);
            int in = clampi(y + r + 1, 0, h - 1) * w + x;   // entering below
            int out = clampi(y - r, 0, h - 1) * w + x;      // leaving above
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
    for (int x = 0; x < w; x++) {
        int sum = 0;
        for (int k = -r; k <= r; k++) {
            sum += src[clampi(k, 0, h - 1) * w + x];
        }
        for (int y = 0; y < h; y++) {
            dst[y * w + x] = (uint8_t)((sum + win / 2) / win);
            // Prefetch the source sample BLUR_PF_ROWS iterations ahead of the
            // running window -- the address is clamped in-bounds, so the only
            // question is whether -fbounds-safety treats the prefetch specially.
            int pf = clampi(y + r + 1 + BLUR_PF_ROWS, 0, h - 1) * w + x;
            __builtin_prefetch(&src[pf], 0, 0);
            int in = clampi(y + r + 1, 0, h - 1) * w + x;
            int out = clampi(y - r, 0, h - 1) * w + x;
            sum += (int)src[in] - (int)src[out];
        }
    }
}
