#include "canvas2d.h"
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
    struct canvas2d_context *__single cv = canvas2d(W, W, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }
    // Nearest sampling for crisp, pixel-exact tiling assertions.
    canvas2d_set_image_smoothing_enabled(cv, false);

    // REPEAT: the 2x2 image tiles across the whole canvas.
    canvas2d_set_fill_pattern(cv, CANVAS2D_CS_SRGB, src, 2, 2, CANVAS2D_REPEAT);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(is(px, len, 0, 0, 255, 0, 0, 255));     // red
    CHECK(is(px, len, 1, 0, 0, 255, 0, 255));     // green
    CHECK(is(px, len, 0, 1, 0, 0, 255, 255));     // blue
    CHECK(is(px, len, 1, 1, 255, 255, 255, 255)); // white
    CHECK(is(px, len, 2, 0, 255, 0, 0, 255));     // tiled: red again
    CHECK(is(px, len, 3, 1, 255, 255, 255, 255)); // tiled: white again

    // NO_REPEAT: only the 2x2 footprint at the origin is painted.
    canvas2d_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas2d_set_fill_pattern(cv, CANVAS2D_CS_SRGB, src, 2, 2, CANVAS2D_NO_REPEAT);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(is(px, len, 0, 0, 255, 0, 0, 255));     // inside
    CHECK(is(px, len, 1, 1, 255, 255, 255, 255)); // inside
    CHECK(is(px, len, 5, 5, 0, 0, 0, 0));         // outside -> transparent
    CHECK(is(px, len, 0, 5, 0, 0, 0, 0));         // below the tile -> transparent

    // REPEAT_X: tiles along x, transparent outside [0,h) in y.
    canvas2d_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas2d_set_fill_pattern(cv, CANVAS2D_CS_SRGB, src, 2, 2, CANVAS2D_REPEAT_X);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(is(px, len, 5, 0, 0, 255, 0, 255));     // x wraps: column 5 -> green
    CHECK(is(px, len, 0, 5, 0, 0, 0, 0));         // y outside -> transparent

    // The pattern is pinned in device space at the transform it was set under:
    // translate(1,0) shifts the tiling one pixel, so device (0,0) now reads the
    // pattern's column 1 (green) instead of column 0 (red).
    canvas2d_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas2d_translate(cv, 1.0f, 0.0f);
    canvas2d_set_fill_pattern(cv, CANVAS2D_CS_SRGB, src, 2, 2, CANVAS2D_REPEAT);
    canvas2d_reset_transform(cv);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(is(px, len, 0, 0, 0, 255, 0, 255));     // shifted: green at the origin

    // A pattern paints strokes too.
    canvas2d_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas2d_reset_transform(cv);
    canvas2d_set_stroke_pattern(cv, CANVAS2D_CS_SRGB, src, 2, 2, CANVAS2D_REPEAT);
    canvas2d_set_line_width(cv, 4.0f);
    canvas2d_begin_path(cv);
    canvas2d_move_to(cv, 0.0f, 4.0f);
    canvas2d_line_to(cv, (float)W, 4.0f);
    canvas2d_stroke(cv);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    struct rgba s = pixel_at(px, len, W, 4, 4);
    CHECK(s.a > 0);  // the stroke laid down pattern ink

    // Invalid dimensions are ignored, leaving the (solid) fill paint in place.
    canvas2d_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas2d_set_fill_pattern(cv, CANVAS2D_CS_SRGB, src, 0, 2, CANVAS2D_REPEAT);  // w = 0: rejected
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(is(px, len, 4, 4, 0, 255, 0, 255));  // still solid green

    canvas2d_free(cv);
    free(px);
    return TEST_REPORT();
}
