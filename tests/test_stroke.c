#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>

int main(void) {
    int const w = 64;
    int const h = 64;
    int const len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }
    canvas *__single cv = canvas_create(w, h);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    // Horizontal red line of width 6 at y=32, from x=8 to x=56.
    canvas_set_stroke_rgba(cv, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_set_line_width(cv, 6.0f);
    canvas_begin_path(cv);
    canvas_move_to(cv, 8.0f, 32.0f);
    canvas_line_to(cv, 56.0f, 32.0f);
    canvas_stroke(cv);
    canvas_read_rgba(cv, px, len);

    CHECK(px_near(pixel_at(px, len, w, 32, 32), 255, 0, 0, 255, 1));  // centre
    CHECK(px_near(pixel_at(px, len, w, 32, 30), 255, 0, 0, 255, 1));  // within half-width
    CHECK(px_near(pixel_at(px, len, w, 32, 24), 0, 0, 0, 0, 1));      // above the line
    CHECK(px_near(pixel_at(px, len, w, 4, 32), 0, 0, 0, 0, 1));       // before butt cap

    canvas_destroy(cv);
    free(px);
    return TEST_REPORT();
}
