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
    struct canvas *__single cv = canvas(w, h);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    // Horizontal red line of width 6 at y=32, from x=8 to x=56.
    canvas_set_stroke_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_set_line_width(cv, 6.0f);
    canvas_begin_path(cv);
    canvas_move_to(cv, 8.0f, 32.0f);
    canvas_line_to(cv, 56.0f, 32.0f);
    canvas_stroke(cv);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);

    CHECK(px_near(pixel_at(px, len, w, 32, 32), 255, 0, 0, 255, 1));  // centre
    CHECK(px_near(pixel_at(px, len, w, 32, 30), 255, 0, 0, 255, 1));  // within half-width
    CHECK(px_near(pixel_at(px, len, w, 32, 24), 0, 0, 0, 0, 1));      // above the line
    CHECK(px_near(pixel_at(px, len, w, 4, 32), 0, 0, 0, 0, 1));       // before butt cap

    // Dashed line [10 on, 10 off] along y=32: on/off runs alternate every 10px.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    float const dash[2] = { 10.0f, 10.0f };
    canvas_set_line_dash(cv, dash, 2);
    canvas_set_stroke_rgba(cv, CANVAS_CS_SRGB, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_set_line_width(cv, 6.0f);
    canvas_begin_path(cv);
    canvas_move_to(cv, 0.0f, 32.0f);
    canvas_line_to(cv, 64.0f, 32.0f);
    canvas_stroke(cv);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, w, 5, 32), 0, 0, 255, 255, 1));   // on  [0,10)
    CHECK(px_near(pixel_at(px, len, w, 15, 32), 0, 0, 0, 0, 1));      // off [10,20)
    CHECK(px_near(pixel_at(px, len, w, 25, 32), 0, 0, 255, 255, 1));  // on  [20,30)
    CHECK(px_near(pixel_at(px, len, w, 35, 32), 0, 0, 0, 0, 1));      // off [30,40)

    // Line caps: butt stops at the endpoint; square/round extend by half-width.
    canvas_set_line_dash(cv, dash, 0);  // back to solid
    canvas_set_line_width(cv, 8.0f);
    canvas_set_stroke_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);

    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_line_cap(cv, CANVAS_CAP_BUTT);
    canvas_begin_path(cv);
    canvas_move_to(cv, 20.0f, 32.0f);
    canvas_line_to(cv, 40.0f, 32.0f);
    canvas_stroke(cv);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, w, 30, 32), 255, 0, 0, 255, 1));  // on the line
    CHECK(px_near(pixel_at(px, len, w, 43, 32), 0, 0, 0, 0, 1));      // past butt end

    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_line_cap(cv, CANVAS_CAP_SQUARE);
    canvas_begin_path(cv);
    canvas_move_to(cv, 20.0f, 32.0f);
    canvas_line_to(cv, 40.0f, 32.0f);
    canvas_stroke(cv);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, w, 43, 32), 255, 0, 0, 255, 1));  // square extends

    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_line_cap(cv, CANVAS_CAP_ROUND);
    canvas_begin_path(cv);
    canvas_move_to(cv, 20.0f, 32.0f);
    canvas_line_to(cv, 40.0f, 32.0f);
    canvas_stroke(cv);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, w, 42, 32), 255, 0, 0, 255, 1));  // round extends

    canvas_free(cv);
    free(px);
    return TEST_REPORT();
}
