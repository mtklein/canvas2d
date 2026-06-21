#include "canvas2d.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>

int main(void) {
    struct canvas2d_context *__single cv = canvas2d(8, 8, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return TEST_REPORT();
    }

    // A 4x3 blank image is 48 bytes, all zero (transparent black).
    int len = -1;
    uint8_t *img = canvas2d_create_image_data(4, 3, &len);
    CHECK(img != NULL);
    CHECK(len == 4 * 3 * 4);
    if (img) {
        bool all_zero = true;
        for (int i = 0; i < len; i++) {
            if (img[i] != 0) {
                all_zero = false;
            }
        }
        CHECK(all_zero);

        // The buffer is usable as a put_image_data source: paint it opaque red
        // and stamp it into the canvas.
        for (int i = 0; i < 4 * 3; i++) {
            img[i * 4 + 0] = 255;
            img[i * 4 + 3] = 255;
        }
        canvas2d_put_image_data(cv, CANVAS2D_CS_SRGB, img, len, 4, 3, 0, 0);
        int const clen = 8 * 8 * 4;
        uint8_t *__counted_by(clen) px = malloc((size_t)clen);
        CHECK(px != NULL);
        if (px) {
            canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, clen);
            CHECK(px_near(pixel_at(px, clen, 8, 0, 0), 255, 0, 0, 255, 1));
            CHECK(px_near(pixel_at(px, clen, 8, 3, 2), 255, 0, 0, 255, 1));
            CHECK(px_near(pixel_at(px, clen, 8, 4, 0), 0, 0, 0, 0, 1));  // untouched
            free(px);
        }
        free(img);
    }

    // Invalid dimensions return NULL and a zero length.
    len = -1;
    CHECK(canvas2d_create_image_data(0, 5, &len) == NULL);
    CHECK(len == 0);
    len = -1;
    CHECK(canvas2d_create_image_data(-2, 3, &len) == NULL);
    CHECK(len == 0);
    // A size that would overflow int is rejected (no huge allocation attempted).
    len = -1;
    CHECK(canvas2d_create_image_data(100000, 100000, &len) == NULL);
    CHECK(len == 0);

    canvas2d_free(cv);
    return TEST_REPORT();
}
