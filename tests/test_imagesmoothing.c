#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>

int main(void) {
    int const W = 40, H = 40, clen = W * H * 4;
    uint8_t *__counted_by(clen) px = malloc((size_t)clen);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }
    // A 2x1 source: left pixel red, right pixel blue.
    uint8_t src[8] = { 255, 0, 0, 255,  0, 0, 255, 255 };

    struct canvas *__single cv = canvas(W, H);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    // Smoothing OFF: nearest-neighbour upscaling gives a hard red|blue edge at the
    // midline -- no blended pixels.  Source x maps as fsx = dest_x / 20, so the
    // split is at dest x = 20.
    canvas_set_image_smoothing_enabled(cv, false);
    canvas_draw_image_scaled(cv, src, 2, 1, 0.0f, 0.0f, (float)W, (float)H);
    canvas_read_rgba(cv, px, clen);
    CHECK(px_near(pixel_at(px, clen, W, 19, 20), 255, 0, 0, 255, 1));  // left of split
    CHECK(px_near(pixel_at(px, clen, W, 20, 20), 0, 0, 255, 255, 1));  // right of split
    CHECK(px_near(pixel_at(px, clen, W, 5, 20), 255, 0, 0, 255, 1));
    CHECK(px_near(pixel_at(px, clen, W, 35, 20), 0, 0, 255, 255, 1));

    // Smoothing ON (default): bilinear blends across the midline -- the columns
    // straddling the split carry both red and blue (a purple), not a pure colour.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_set_image_smoothing_enabled(cv, true);
    canvas_draw_image_scaled(cv, src, 2, 1, 0.0f, 0.0f, (float)W, (float)H);
    canvas_read_rgba(cv, px, clen);
    struct px4 a = pixel_at(px, clen, W, 19, 20);
    struct px4 b = pixel_at(px, clen, W, 20, 20);
    CHECK(a.r > 30 && a.b > 30);   // blended, not pure red
    CHECK(b.r > 30 && b.b > 30);   // blended, not pure blue
    // Far from the seam it is still essentially the source colour.
    CHECK(px_near(pixel_at(px, clen, W, 3, 20), 255, 0, 0, 255, 2));
    CHECK(px_near(pixel_at(px, clen, W, 37, 20), 0, 0, 255, 255, 2));

    canvas_free(cv);
    free(px);
    return TEST_REPORT();
}
