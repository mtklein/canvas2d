#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>

int main(void) {
    canvas *__single cv = canvas_create(8, 8);
    CHECK(cv != NULL);
    if (!cv) {
        return TEST_REPORT();
    }

    // A 4x3 blank image is 48 bytes, all zero (transparent black).
    int len = -1;
    uint8_t *img = canvas_create_image_data(cv, 4, 3, &len);
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
        canvas_put_image_data(cv, img, len, 4, 3, 0, 0);
        int const clen = 8 * 8 * 4;
        uint8_t *__counted_by(clen) px = malloc((size_t)clen);
        CHECK(px != NULL);
        if (px) {
            canvas_read_rgba(cv, px, clen);
            CHECK(px_near(pixel_at(px, clen, 8, 0, 0), 255, 0, 0, 255, 1));
            CHECK(px_near(pixel_at(px, clen, 8, 3, 2), 255, 0, 0, 255, 1));
            CHECK(px_near(pixel_at(px, clen, 8, 4, 0), 0, 0, 0, 0, 1));  // untouched
            free(px);
        }
        free(img);
    }

    // Invalid dimensions return NULL and a zero length.
    len = -1;
    CHECK(canvas_create_image_data(cv, 0, 5, &len) == NULL);
    CHECK(len == 0);
    len = -1;
    CHECK(canvas_create_image_data(cv, -2, 3, &len) == NULL);
    CHECK(len == 0);
    // A size that would overflow int is rejected (no huge allocation attempted).
    len = -1;
    CHECK(canvas_create_image_data(cv, 100000, 100000, &len) == NULL);
    CHECK(len == 0);

    canvas_destroy(cv);
    return TEST_REPORT();
}
