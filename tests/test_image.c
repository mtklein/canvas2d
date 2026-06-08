#include "canvas.h"
#include "cnvs_image.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>
#include <string.h>

int main(void) {
    // --- cnvs_blit_rgba: plain copy of a 2x2 sub-rect (CPU only) ---
    int const sw = 4;
    int const sh = 4;
    int const slen = sw * sh * 4;
    uint8_t *__counted_by(slen) src = malloc((size_t)slen);
    int const dlen = 4 * 4 * 4;
    uint8_t *__counted_by(dlen) dst = malloc((size_t)dlen);
    CHECK(src != NULL && dst != NULL);
    if (src && dst) {
        for (int i = 0; i < slen; i++) {
            src[i] = (uint8_t)i;
        }
        memset(dst, 0, (size_t)dlen);
        cnvs_blit_rgba(dst, 4, 4, 0, 0, src, sw, sh, 1, 1, 2, 2);
        bool copied = true;
        for (int j = 0; j < 2; j++) {
            for (int i = 0; i < 2; i++) {
                for (int c = 0; c < 4; c++) {
                    int d = (j * 4 + i) * 4 + c;
                    int s = ((1 + j) * sw + (1 + i)) * 4 + c;
                    if (dst[d] != src[s]) {
                        copied = false;
                    }
                }
            }
        }
        CHECK(copied);
        CHECK(dst[(3 * 4 + 3) * 4] == 0);  // untouched corner

        // Out-of-bounds offsets clip rather than trap: dst(0,0) <- src(1,1).
        memset(dst, 0, (size_t)dlen);
        cnvs_blit_rgba(dst, 4, 4, -1, -1, src, sw, sh, 0, 0, 4, 4);
        CHECK(dst[0] == src[(1 * sw + 1) * 4]);
        CHECK(dst[(3 * 4 + 3) * 4] == 0);  // bottom-right had no source
    }
    free(src);
    free(dst);

    // --- GPU get/put round-trip ---
    int const W = 8;
    int const H = 8;
    int const len = W * H * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    canvas *__single cv = canvas_create(W, H);
    CHECK(px != NULL && cv != NULL);
    if (px && cv) {
        canvas_set_fill_rgba(cv, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_fill_rect(cv, 2.0f, 2.0f, 4.0f, 4.0f);
        canvas_get_image_data(cv, 0, 0, W, H, px, len);
        CHECK(px_near(pixel_at(px, len, W, 3, 3), 255, 0, 0, 255, 1));  // red square
        CHECK(px_near(pixel_at(px, len, W, 0, 0), 0, 0, 0, 0, 1));      // transparent

        // A sub-rect that runs off the canvas reads transparent past the edge.
        int const s2 = 4 * 4 * 4;
        uint8_t *__counted_by(s2) sub = malloc((size_t)s2);
        if (sub) {
            canvas_get_image_data(cv, 6, 6, 4, 4, sub, s2);
            CHECK(px_near(pixel_at(sub, s2, 4, 3, 3), 0, 0, 0, 0, 1));  // off-canvas
            free(sub);
        }

        // putImageData overwrites a region.
        int const g4 = 4 * 4 * 4;
        uint8_t *__counted_by(g4) green = malloc((size_t)g4);
        if (green) {
            uint8_t const gpx[4] = { 0, 255, 0, 255 };
            for (int i = 0; i < g4; i += 4) {
                for (int c = 0; c < 4; c++) {
                    green[i + c] = gpx[c];
                }
            }
            canvas_put_image_data(cv, green, g4, 4, 4, 2, 2);
            canvas_get_image_data(cv, 0, 0, W, H, px, len);
            CHECK(px_near(pixel_at(px, len, W, 3, 3), 0, 255, 0, 255, 1));  // now green
            CHECK(px_near(pixel_at(px, len, W, 0, 0), 0, 0, 0, 0, 1));      // untouched

            // Clipped put (negative origin): only the in-canvas corner lands.
            canvas_put_image_data(cv, green, g4, 4, 4, -2, -2);
            canvas_get_image_data(cv, 0, 0, W, H, px, len);
            CHECK(px_near(pixel_at(px, len, W, 0, 0), 0, 255, 0, 255, 1));
            free(green);
        }
        canvas_destroy(cv);
    }
    free(px);
    return TEST_REPORT();
}
