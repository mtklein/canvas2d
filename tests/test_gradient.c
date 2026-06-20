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
    struct canvas *__single cv = canvas(w, h, CANVAS_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    // Horizontal linear gradient red -> blue, painted with fillRect (which must
    // honour a gradient fill, like fill()).
    canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, (float)w, 0.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    struct rgba lft = pixel_at(px, len, w, 6, 32);
    struct rgba mid = pixel_at(px, len, w, 32, 32);
    struct rgba rgt = pixel_at(px, len, w, 58, 32);
    CHECK(lft.r > 200 && lft.b < 60 && lft.g < 20);                  // near red
    CHECK(rgt.b > 200 && rgt.r < 60 && rgt.g < 20);                  // near blue
    CHECK(mid.r > 96 && mid.r < 160 && mid.b > 96 && mid.b < 160);   // purple

    // Concentric radial gradient yellow centre -> red rim (radius 28).
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_radial_gradient(cv, 32.0f, 32.0f, 0.0f, 32.0f, 32.0f, 28.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_begin_path(cv);
    canvas_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_fill(cv, CANVAS_NONZERO);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    struct rgba ctr = pixel_at(px, len, w, 32, 32);
    struct rgba ring = pixel_at(px, len, w, 46, 32);  // ~half radius
    struct rgba rim = pixel_at(px, len, w, 60, 32);   // at/over the rim
    CHECK(ctr.r > 240 && ctr.g > 220 && ctr.b < 20);              // yellow
    CHECK(ring.r > 240 && ring.g > 80 && ring.g < 200 && ring.b < 20);  // orange
    CHECK(rim.r > 240 && rim.g < 50 && rim.b < 20);              // red

    // A three-stop gradient passes through the middle colour.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, (float)w, 0.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_begin_path(cv);
    canvas_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_fill(cv, CANVAS_NONZERO);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    struct rgba g_mid = pixel_at(px, len, w, 32, 32);
    CHECK(g_mid.g > 200 && g_mid.r < 40 && g_mid.b < 40);  // green at the centre stop

    // Setting a solid fill colour reverts the paint away from the gradient.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas_begin_path(cv);
    canvas_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_fill(cv, CANVAS_NONZERO);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, w, 6, 32), 0, 255, 0, 255, 1));  // solid green again

    // save/restore brackets the gradient: restore brings back the solid fill.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.2f, 0.2f, 0.2f, 1.0f);
    canvas_save(cv);
    canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, (float)w, 0.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_restore(cv);
    canvas_begin_path(cv);
    canvas_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_fill(cv, CANVAS_NONZERO);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, w, 6, 32), 51, 51, 51, 255, 2));  // restored solid

    // Gradient stroke: a thick horizontal line, red -> blue left to right.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_stroke_linear_gradient(cv, 0.0f, 0.0f, (float)w, 0.0f);
    canvas_add_stroke_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_add_stroke_color_stop(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_set_line_width(cv, 12.0f);
    canvas_begin_path(cv);
    canvas_move_to(cv, 6.0f, 32.0f);
    canvas_line_to(cv, 58.0f, 32.0f);
    canvas_stroke(cv);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    struct rgba s_lft = pixel_at(px, len, w, 10, 32);
    struct rgba s_rgt = pixel_at(px, len, w, 54, 32);
    CHECK(s_lft.r > 180 && s_lft.b < 80);  // red end
    CHECK(s_rgt.b > 180 && s_rgt.r < 80);  // blue end

    // A solid stroke colour reverts the stroke paint away from the gradient.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_stroke_rgba(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas_set_line_width(cv, 12.0f);
    canvas_begin_path(cv);
    canvas_move_to(cv, 6.0f, 32.0f);
    canvas_line_to(cv, 58.0f, 32.0f);
    canvas_stroke(cv);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 0, 255, 0, 255, 2));  // solid green again

    // Per spec, exact degenerates paint NOTHING -- the draw is a no-op:
    // a zero-length linear gradient, and a radial whose circles coincide.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);  // white ground

    canvas_set_fill_linear_gradient(cv, 32.0f, 32.0f, 32.0f, 32.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 255, 255, 255, 255, 0));

    canvas_set_fill_radial_gradient(cv, 32.0f, 32.0f, 8.0f, 32.0f, 32.0f, 8.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 255, 255, 255, 255, 0));

    // The boundary is exact equality: a tiny-but-nonzero linear gradient and a
    // point-centred conic both still paint.
    canvas_set_fill_linear_gradient(cv, 32.0f, 32.0f, 32.001f, 32.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 255, 0, 0, 255, 2));

    canvas_set_fill_conic_gradient(cv, 0.0f, 32.0f, 32.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 0, 0, 255, 255, 2));

    canvas_free(cv);
    free(px);
    return TEST_REPORT();
}
