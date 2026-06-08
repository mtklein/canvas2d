#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <math.h>
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

    // Filled triangle (8,8)-(56,8)-(32,56).
    canvas_set_fill_rgba(cv, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_begin_path(cv);
    canvas_move_to(cv, 8.0f, 8.0f);
    canvas_line_to(cv, 56.0f, 8.0f);
    canvas_line_to(cv, 32.0f, 56.0f);
    canvas_close_path(cv);
    canvas_fill(cv);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 32, 24), 255, 0, 0, 255, 1));  // interior
    CHECK(px_near(pixel_at(px, len, w, 4, 52), 0, 0, 0, 0, 1));       // outside
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);

    // Filled circle, centre (32,32) radius 20.
    canvas_set_fill_rgba(cv, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas_begin_path(cv);
    canvas_arc(cv, 32.0f, 32.0f, 20.0f, 0.0f, 2.0f * (float)M_PI, false);
    canvas_fill(cv);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 0, 255, 0, 255, 1));  // centre
    CHECK(px_near(pixel_at(px, len, w, 32, 16), 0, 255, 0, 255, 1));  // inside (r=16)
    CHECK(px_near(pixel_at(px, len, w, 60, 60), 0, 0, 0, 0, 1));      // far corner
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);

    // Filled rectangle via rect() path.
    canvas_set_fill_rgba(cv, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_begin_path(cv);
    canvas_rect(cv, 10.0f, 10.0f, 20.0f, 20.0f);
    canvas_fill(cv);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 20, 20), 0, 0, 255, 255, 1));  // interior
    CHECK(px_near(pixel_at(px, len, w, 50, 50), 0, 0, 0, 0, 1));      // outside
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);

    // Donut: outer rect plus a reversed inner rect -> nonzero cancels in the hole.
    canvas_set_fill_rule(cv, CANVAS_NONZERO);
    canvas_set_fill_rgba(cv, 1.0f, 1.0f, 0.0f, 1.0f);
    canvas_begin_path(cv);
    canvas_rect(cv, 8.0f, 8.0f, 48.0f, 48.0f);
    canvas_move_to(cv, 40.0f, 24.0f);
    canvas_line_to(cv, 24.0f, 24.0f);
    canvas_line_to(cv, 24.0f, 40.0f);
    canvas_line_to(cv, 40.0f, 40.0f);
    canvas_close_path(cv);
    canvas_fill(cv);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 0, 0, 0, 0, 1));        // hole
    CHECK(px_near(pixel_at(px, len, w, 12, 32), 255, 255, 0, 255, 1));  // ring
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);

    // Self-intersecting pentagram: nonzero fills the centre, even-odd empties it.
    canvas_set_fill_rgba(cv, 1.0f, 0.0f, 1.0f, 1.0f);
    canvas_begin_path(cv);
    for (int i = 0; i < 5; i++) {
        float a = -(float)M_PI * 0.5f + (float)i * (4.0f * (float)M_PI / 5.0f);
        float sx = 32.0f + 28.0f * cosf(a);
        float sy = 32.0f + 28.0f * sinf(a);
        if (i == 0) {
            canvas_move_to(cv, sx, sy);
        } else {
            canvas_line_to(cv, sx, sy);
        }
    }
    canvas_close_path(cv);

    canvas_set_fill_rule(cv, CANVAS_NONZERO);
    canvas_fill(cv);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 255, 0, 255, 255, 1));  // nonzero: centre
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);

    canvas_set_fill_rule(cv, CANVAS_EVENODD);
    canvas_fill(cv);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 0, 0, 0, 0, 1));        // even-odd: hole
    CHECK(px_near(pixel_at(px, len, w, 32, 10), 255, 0, 255, 255, 1));  // arm still filled

    // Rounded rect: corners are clipped off by the radius.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_rule(cv, CANVAS_NONZERO);
    canvas_set_fill_rgba(cv, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas_begin_path(cv);
    canvas_round_rect(cv, 8.0f, 8.0f, 48.0f, 48.0f, 12.0f);
    canvas_fill(cv);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 0, 255, 0, 255, 1));  // interior
    CHECK(px_near(pixel_at(px, len, w, 10, 10), 0, 0, 0, 0, 1));      // rounded corner

    // Wide ellipse: filled along the long axis, empty past the short one.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_rgba(cv, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_begin_path(cv);
    canvas_ellipse(cv, 32.0f, 32.0f, 24.0f, 12.0f, 0.0f, 0.0f, 2.0f * (float)M_PI, false);
    canvas_fill(cv);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 52, 32), 0, 0, 255, 255, 1));  // inside long axis
    CHECK(px_near(pixel_at(px, len, w, 32, 46), 0, 0, 0, 0, 1));      // past short axis

    canvas_destroy(cv);
    free(px);
    return TEST_REPORT();
}
