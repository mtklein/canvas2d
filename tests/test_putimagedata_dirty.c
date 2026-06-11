#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>

// Build an 8x8 RGBA8 source: blue everywhere, with a red block in [2,6)x[2,6).
static void fill_source(uint8_t *__counted_by(len) src, int len) {
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            int o = (row * 8 + col) * 4;
            bool red = col >= 2 && col < 6 && row >= 2 && row < 6;
            src[o + 0] = red ? 255 : 0;
            src[o + 1] = 0;
            src[o + 2] = red ? 0 : 255;
            src[o + 3] = 255;
        }
    }
    (void)len;
}

static bool is_red(uint8_t const *__counted_by(len) px, int len, int x, int y) {
    return px_near(pixel_at(px, len, 16, x, y), 255, 0, 0, 255, 1);
}
static bool is_blue(uint8_t const *__counted_by(len) px, int len, int x, int y) {
    return px_near(pixel_at(px, len, 16, x, y), 0, 0, 255, 255, 1);
}
static bool is_clear(uint8_t const *__counted_by(len) px, int len, int x, int y) {
    return px_near(pixel_at(px, len, 16, x, y), 0, 0, 0, 0, 1);
}

int main(void) {
    int const slen = 8 * 8 * 4;
    uint8_t *__counted_by(slen) src = malloc((size_t)slen);
    int const clen = 16 * 16 * 4;
    uint8_t *__counted_by(clen) px = malloc((size_t)clen);
    CHECK(src != NULL);
    CHECK(px != NULL);
    if (!src || !px) {
        free(src);
        free(px);
        return TEST_REPORT();
    }
    fill_source(src, slen);
    struct canvas *__single cv = canvas(16, 16);
    CHECK(cv != NULL);
    if (!cv) {
        free(src);
        free(px);
        return TEST_REPORT();
    }

    // Dirty rect = the red block (2,2,4,4), image origin at (4,4): only the red
    // block is written, at canvas [6,10)x[6,10).  The blue surround is NOT copied.
    canvas_clear_rect(cv, 0.0f, 0.0f, 16.0f, 16.0f);
    canvas_put_image_data_dirty(cv, src, slen, 8, 8, 4, 4, 2, 2, 4, 4);
    canvas_read_rgba(cv, px, clen);
    CHECK(is_red(px, clen, 6, 6));     // top-left of the copied block
    CHECK(is_red(px, clen, 9, 9));     // bottom-right of the copied block
    CHECK(is_clear(px, clen, 5, 5));   // just outside the dirty dest rect
    CHECK(is_clear(px, clen, 1, 1));   // far away, untouched

    // Regression: the plain (no dirty rect) overload still copies everything.
    canvas_clear_rect(cv, 0.0f, 0.0f, 16.0f, 16.0f);
    canvas_put_image_data(cv, src, slen, 8, 8, 0, 0);
    canvas_read_rgba(cv, px, clen);
    CHECK(is_blue(px, clen, 0, 0));    // blue surround
    CHECK(is_red(px, clen, 4, 4));     // red block
    CHECK(is_blue(px, clen, 7, 7));

    // A dirty rect larger than the source clamps to the source bounds.
    canvas_clear_rect(cv, 0.0f, 0.0f, 16.0f, 16.0f);
    canvas_put_image_data_dirty(cv, src, slen, 8, 8, 0, 0, 2, 2, 100, 100);
    canvas_read_rgba(cv, px, clen);
    CHECK(is_red(px, clen, 3, 3));     // source col/row 3: red
    CHECK(is_blue(px, clen, 7, 7));    // source col/row 7: blue (still inside)
    CHECK(is_clear(px, clen, 1, 1));   // col/row 1: left of dirtyX=2, not copied

    // Negative dirty width normalises to the same red block (anchored at x=6,
    // width -4 -> x=2, width 4).
    canvas_clear_rect(cv, 0.0f, 0.0f, 16.0f, 16.0f);
    canvas_put_image_data_dirty(cv, src, slen, 8, 8, 0, 0, 6, 2, -4, 4);
    canvas_read_rgba(cv, px, clen);
    CHECK(is_red(px, clen, 3, 3));
    CHECK(is_clear(px, clen, 1, 1));

    // An empty dirty rect is a no-op: a pre-painted pixel survives untouched.
    canvas_clear_rect(cv, 0.0f, 0.0f, 16.0f, 16.0f);
    canvas_set_fill_rgba(cv, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, 16.0f, 16.0f);
    canvas_put_image_data_dirty(cv, src, slen, 8, 8, 0, 0, 0, 0, 0, 4);
    canvas_read_rgba(cv, px, clen);
    CHECK(px_near(pixel_at(px, clen, 16, 4, 4), 0, 255, 0, 255, 1));  // still green

    canvas_free(cv);
    free(src);
    free(px);
    return TEST_REPORT();
}
