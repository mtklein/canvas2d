#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>

#define W 8

static bool is(uint8_t const *__counted_by(len) px, int len, int x, int y,
               int r, int g, int b, int a) {
    return px_near(pixel_at(px, len, W, x, y), r, g, b, a, 1);
}

int main(void) {
    int const len = W * W * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }
    // 2x2 source: red, green / blue, white (row-major, top row first).
    uint8_t src[16] = {
        255, 0, 0, 255,   0, 255, 0, 255,
        0, 0, 255, 255,   255, 255, 255, 255,
    };
    struct canvas *__single cv = canvas_create(W, W);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }
    // Nearest sampling for crisp, pixel-exact tiling assertions.
    canvas_set_image_smoothing_enabled(cv, false);

    // REPEAT: the 2x2 image tiles across the whole canvas.
    canvas_set_fill_pattern(cv, src, 2, 2, CANVAS_REPEAT);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_read_rgba(cv, px, len);
    CHECK(is(px, len, 0, 0, 255, 0, 0, 255));     // red
    CHECK(is(px, len, 1, 0, 0, 255, 0, 255));     // green
    CHECK(is(px, len, 0, 1, 0, 0, 255, 255));     // blue
    CHECK(is(px, len, 1, 1, 255, 255, 255, 255)); // white
    CHECK(is(px, len, 2, 0, 255, 0, 0, 255));     // tiled: red again
    CHECK(is(px, len, 3, 1, 255, 255, 255, 255)); // tiled: white again

    // NO_REPEAT: only the 2x2 footprint at the origin is painted.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_set_fill_pattern(cv, src, 2, 2, CANVAS_NO_REPEAT);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_read_rgba(cv, px, len);
    CHECK(is(px, len, 0, 0, 255, 0, 0, 255));     // inside
    CHECK(is(px, len, 1, 1, 255, 255, 255, 255)); // inside
    CHECK(is(px, len, 5, 5, 0, 0, 0, 0));         // outside -> transparent
    CHECK(is(px, len, 0, 5, 0, 0, 0, 0));         // below the tile -> transparent

    // REPEAT_X: tiles along x, transparent outside [0,h) in y.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_set_fill_pattern(cv, src, 2, 2, CANVAS_REPEAT_X);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_read_rgba(cv, px, len);
    CHECK(is(px, len, 5, 0, 0, 255, 0, 255));     // x wraps: column 5 -> green
    CHECK(is(px, len, 0, 5, 0, 0, 0, 0));         // y outside -> transparent

    // The pattern is pinned in device space at the transform it was set under:
    // translate(1,0) shifts the tiling one pixel, so device (0,0) now reads the
    // pattern's column 1 (green) instead of column 0 (red).
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_translate(cv, 1.0f, 0.0f);
    canvas_set_fill_pattern(cv, src, 2, 2, CANVAS_REPEAT);
    canvas_reset_transform(cv);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_read_rgba(cv, px, len);
    CHECK(is(px, len, 0, 0, 0, 255, 0, 255));     // shifted: green at the origin

    // A pattern paints strokes too.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_reset_transform(cv);
    canvas_set_stroke_pattern(cv, src, 2, 2, CANVAS_REPEAT);
    canvas_set_line_width(cv, 4.0f);
    canvas_begin_path(cv);
    canvas_move_to(cv, 0.0f, 4.0f);
    canvas_line_to(cv, (float)W, 4.0f);
    canvas_stroke(cv);
    canvas_read_rgba(cv, px, len);
    struct px4 s = pixel_at(px, len, W, 4, 4);
    CHECK(s.a > 0);  // the stroke laid down pattern ink

    // Invalid dimensions are ignored, leaving the (solid) fill paint in place.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_set_fill_rgba(cv, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas_set_fill_pattern(cv, src, 0, 2, CANVAS_REPEAT);  // w = 0: rejected
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_read_rgba(cv, px, len);
    CHECK(is(px, len, 4, 4, 0, 255, 0, 255));  // still solid green

    canvas_destroy(cv);
    free(px);
    return TEST_REPORT();
}
