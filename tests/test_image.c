#include "canvas.h"
#include "cnvs_image.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>
#include <string.h>

// The colour pipeline's load-bearing exactness claim (docs/decisions/
// color-axis.md, experiment 1), pinned end to end: EVERY 8-bit (colour, alpha)
// pair with alpha > 0 -- all 256*255 = 65,280 of them -- survives putImageData
// (premultiply, store) followed by getImageData (un-premultiply, quantize)
// byte-identical.  This is the property that picked _Float16 over u8 storage,
// and it must keep holding now that the premultiply, divide, and 255-quantize
// all run in _Float16 arithmetic too.  Alpha 0 is excluded by definition: the
// colour is unrecoverable (premultiplies to zero), per spec.
static void roundtrip_exhaustive(void) {
    enum { RW = 256, RH = 255 };  // x: colour 0..255; y: alpha y+1 (1..255)
    int const rlen = RW * RH * 4;
    uint8_t *__counted_by(rlen) in = malloc((size_t)rlen);
    uint8_t *__counted_by(rlen) out = malloc((size_t)rlen);
    canvas *__single cv = canvas_create(RW, RH);
    CHECK(in != NULL && out != NULL && cv != NULL);
    if (in && out && cv) {
        for (int y = 0; y < RH; y++) {
            for (int x = 0; x < RW; x++) {
                int i = (y * RW + x) * 4;
                in[i] = in[i + 1] = in[i + 2] = (uint8_t)x;
                in[i + 3] = (uint8_t)(y + 1);
            }
        }
        canvas_put_image_data(cv, in, rlen, RW, RH, 0, 0);
        canvas_get_image_data(cv, 0, 0, RW, RH, out, rlen);
        int mismatched = 0;
        for (int i = 0; i < rlen; i++) {
            if (in[i] != out[i]) {
                mismatched += 1;
            }
        }
        CHECK(mismatched == 0);
    }
    if (cv) {
        canvas_destroy(cv);
    }
    free(in);
    free(out);
}

int main(void) {
    roundtrip_exhaustive();
    // cnvs_blit_rgba: plain copy of a 2x2 sub-rect.
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

    // get/put round-trip.
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
