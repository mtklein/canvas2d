#include "blur.h"
#include "test_util.h"

#include <stdlib.h>

static int clampi(int v, int lo, int hi) {
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

// O(w*r) brute-force reference for one row of the horizontal pass: the literal
// window sum, edge-clamped, with the same (sum + win/2) / win quantize.
static void ref_blur_h_row(uint8_t *__counted_by(w) dst,
                           uint8_t const *__counted_by(w) src, int w, int r) {
    int win = 2 * r + 1;
    for (int x = 0; x < w; x++) {
        int sum = 0;
        for (int k = -r; k <= r; k++) {
            sum += src[clampi(x + k, 0, w - 1)];
        }
        dst[x] = (uint8_t)((sum + win / 2) / win);
    }
}

// O(h*r) brute-force reference for one column of the vertical pass: the literal
// window sum down column x, edge-clamped, with the same (sum + win/2) / win
// quantize.
static void ref_blur_v_col(uint8_t *__counted_by(h) dst,
                           uint8_t const *__counted_by(w * h) src,
                           int w, int h, int x, int r) {
    int win = 2 * r + 1;
    for (int y = 0; y < h; y++) {
        int sum = 0;
        for (int k = -r; k <= r; k++) {
            sum += src[clampi(y + k, 0, h - 1) * w + x];
        }
        dst[y] = (uint8_t)((sum + win / 2) / win);
    }
}

// blur_box_v must match the brute-force reference bit-for-bit across awkward
// shapes: widths off the 8-column lane grid (tail columns), heights of 1 and 2,
// radii of 0, >= h, and crossing the height.
static void check_v_vs_ref(void) {
    static int const ws[] = { 1, 2, 7, 8, 9, 31, 33, 64 };
    static int const hs[] = { 1, 2, 7, 24 };
    static int const rs[] = { 0, 1, 2, 5, 17, 300 };
    enum { MAXW = 64, MAXH = 24 };
    uint8_t src[MAXW * MAXH], dst[MAXW * MAXH], ref[MAXH];
    for (size_t wi = 0; wi < sizeof ws / sizeof *ws; wi++) {
        for (size_t hi = 0; hi < sizeof hs / sizeof *hs; hi++) {
            for (size_t ri = 0; ri < sizeof rs / sizeof *rs; ri++) {
                int const w = ws[wi], h = hs[hi], r = rs[ri], n = w * h;
                for (int i = 0; i < n; i++) {
                    src[i] = (uint8_t)((i * 73 + (int)hi * 57 + (int)wi * 31 + (int)ri * 11) & 0xFF);
                }
                blur_box_v(dst, src, w, h, r);
                bool same = true;
                for (int x = 0; x < w; x++) {
                    ref_blur_v_col(ref, src, w, h, x, r);
                    for (int y = 0; y < h; y++) {
                        if (dst[y * w + x] != ref[y]) {
                            same = false;
                        }
                    }
                }
                CHECK(same);
            }
        }
    }
}

// blur_box_h must match the brute-force reference bit-for-bit across awkward
// shapes: widths off the vector-block grid, radii of 0, >= w, and crossing the
// width, single-pixel rows.
static void check_h_vs_ref(void) {
    static int const ws[] = { 1, 2, 7, 8, 9, 31, 33, 64, 200 };
    static int const rs[] = { 0, 1, 2, 5, 17, 300 };
    enum { MAXW = 200, H = 3 };
    uint8_t src[MAXW * H], dst[MAXW * H], ref[MAXW];
    for (size_t wi = 0; wi < sizeof ws / sizeof *ws; wi++) {
        for (size_t ri = 0; ri < sizeof rs / sizeof *rs; ri++) {
            int const w = ws[wi], r = rs[ri], n = w * H;
            for (int i = 0; i < n; i++) {
                // Noise covering 0..255, varied per (w, r) config; small-int
                // arithmetic so the debug build's integer sanitizer stays quiet.
                src[i] = (uint8_t)((i * 73 + (int)wi * 31 + (int)ri * 11) & 0xFF);
            }
            blur_box_h(dst, src, w, H, r);
            bool same = true;
            for (int y = 0; y < H; y++) {
                ref_blur_h_row(ref, src + y * w, w, r);
                for (int x = 0; x < w; x++) {
                    if (dst[y * w + x] != ref[x]) {
                        same = false;
                    }
                }
            }
            CHECK(same);
        }
    }
}

int main(void) {
    check_h_vs_ref();
    check_v_vs_ref();

    int const w = 32, h = 24, n = w * h, r = 2, win = 2 * r + 1;
    uint8_t *__counted_by(n) src = malloc((size_t)n);
    uint8_t *__counted_by(n) dst = malloc((size_t)n);
    uint8_t *__counted_by(n) dst2 = malloc((size_t)n);
    CHECK(src != NULL && dst != NULL && dst2 != NULL);
    if (!src || !dst || !dst2) {
        free(src); free(dst); free(dst2);
        return TEST_REPORT();
    }

    // Uniform input stays uniform (edge clamping must not darken the borders).
    for (int i = 0; i < n; i++) {
        src[i] = 200;
    }
    blur_box_h(dst, src, w, h, r);
    blur_box_v(dst2, dst, w, h, r);
    bool flat = true;
    for (int i = 0; i < n; i++) {
        if (dst2[i] != 200) {
            flat = false;
        }
    }
    CHECK(flat);

    // Horizontal pass of a centred impulse spreads symmetrically over 2r+1 pixels
    // (255/win each) on its row only, and leaves other rows zero.
    for (int i = 0; i < n; i++) {
        src[i] = 0;
    }
    int cx = 16, cy = 12;
    src[cy * w + cx] = 255;
    blur_box_h(dst, src, w, h, r);
    uint8_t lvl = (uint8_t)((255 + win / 2) / win);
    CHECK(dst[cy * w + cx] == lvl);
    CHECK(dst[cy * w + cx - r] == lvl && dst[cy * w + cx + r] == lvl);  // window edge
    CHECK(dst[cy * w + cx - r - 1] == 0 && dst[cy * w + cx + r + 1] == 0);  // outside
    CHECK(dst[(cy - 1) * w + cx] == 0);  // a different row is untouched

    // Prefetch is a hint: blur_box_v_pf must match blur_box_v bit-for-bit.
    for (int i = 0; i < n; i++) {
        src[i] = (uint8_t)((i * 37 + 11) & 0xFF);
    }
    blur_box_v(dst, src, w, h, r);
    blur_box_v_pf(dst2, src, w, h, r);
    bool same = true;
    for (int i = 0; i < n; i++) {
        if (dst[i] != dst2[i]) {
            same = false;
        }
    }
    CHECK(same);

    free(src); free(dst); free(dst2);
    return TEST_REPORT();
}
