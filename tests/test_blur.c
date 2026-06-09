#include "blur.h"
#include "test_util.h"

#include <stdlib.h>

int main(void) {
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
