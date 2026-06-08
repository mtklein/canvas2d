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

    // Horizontal linear gradient red -> blue, painted with fillRect (which must
    // honour a gradient fill, like fill()).
    canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, (float)w, 0.0f);
    canvas_add_fill_color_stop(cv, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_add_fill_color_stop(cv, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_read_rgba(cv, px, len);
    // Interpolated in linear light, then sRGB-encoded for 8-bit readout: the
    // midpoint is ~(186, 0, 189), lighter than the 128 an 8-bit-space lerp gives.
    struct px4 lft = pixel_at(px, len, w, 6, 32);
    struct px4 mid = pixel_at(px, len, w, 32, 32);
    struct px4 rgt = pixel_at(px, len, w, 58, 32);
    CHECK(lft.r > 200 && lft.b < 130 && lft.g < 20);                 // near red
    CHECK(rgt.b > 200 && rgt.r < 130 && rgt.g < 20);                 // near blue
    CHECK(mid.r > 150 && mid.r < 220 && mid.b > 150 && mid.b < 220 && mid.g < 20);

    // Concentric radial gradient yellow centre -> red rim (radius 28).
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_radial_gradient(cv, 32.0f, 32.0f, 0.0f, 32.0f, 32.0f, 28.0f);
    canvas_add_fill_color_stop(cv, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f);
    canvas_add_fill_color_stop(cv, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_begin_path(cv);
    canvas_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_fill(cv);
    canvas_read_rgba(cv, px, len);
    struct px4 ctr = pixel_at(px, len, w, 32, 32);
    struct px4 ring = pixel_at(px, len, w, 46, 32);  // ~half radius
    struct px4 rim = pixel_at(px, len, w, 60, 32);   // at/over the rim
    CHECK(ctr.r > 240 && ctr.g > 220 && ctr.b < 20);              // yellow
    CHECK(ring.r > 240 && ring.g > 80 && ring.g < 200 && ring.b < 20);  // orange
    CHECK(rim.r > 240 && rim.g < 50 && rim.b < 20);              // red

    // A three-stop gradient passes through the middle colour.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, (float)w, 0.0f);
    canvas_add_fill_color_stop(cv, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_add_fill_color_stop(cv, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas_add_fill_color_stop(cv, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_begin_path(cv);
    canvas_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_fill(cv);
    canvas_read_rgba(cv, px, len);
    struct px4 g_mid = pixel_at(px, len, w, 32, 32);
    CHECK(g_mid.g > 200 && g_mid.r < 40 && g_mid.b < 40);  // green at the centre stop

    // Setting a solid fill colour reverts the paint away from the gradient.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_rgba(cv, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas_begin_path(cv);
    canvas_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_fill(cv);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 6, 32), 0, 255, 0, 255, 1));  // solid green again

    // save/restore brackets the gradient: restore brings back the solid fill.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_rgba(cv, 0.2f, 0.2f, 0.2f, 1.0f);
    canvas_save(cv);
    canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, (float)w, 0.0f);
    canvas_add_fill_color_stop(cv, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_add_fill_color_stop(cv, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_restore(cv);
    canvas_begin_path(cv);
    canvas_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_fill(cv);
    canvas_read_rgba(cv, px, len);
    // Linear 0.2 sRGB-encodes to ~124, not 51.
    CHECK(px_near(pixel_at(px, len, w, 6, 32), 124, 124, 124, 255, 2));  // restored solid

    // Gradient stroke: a thick horizontal line, red -> blue left to right.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_stroke_linear_gradient(cv, 0.0f, 0.0f, (float)w, 0.0f);
    canvas_add_stroke_color_stop(cv, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_add_stroke_color_stop(cv, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_set_line_width(cv, 12.0f);
    canvas_begin_path(cv);
    canvas_move_to(cv, 6.0f, 32.0f);
    canvas_line_to(cv, 58.0f, 32.0f);
    canvas_stroke(cv);
    canvas_read_rgba(cv, px, len);
    struct px4 s_lft = pixel_at(px, len, w, 10, 32);
    struct px4 s_rgt = pixel_at(px, len, w, 54, 32);
    CHECK(s_lft.r > 180 && s_lft.b < 150 && s_lft.r > s_lft.b);  // red end
    CHECK(s_rgt.b > 180 && s_rgt.r < 150 && s_rgt.b > s_rgt.r);  // blue end

    // A solid stroke colour reverts the stroke paint away from the gradient.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_stroke_rgba(cv, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas_set_line_width(cv, 12.0f);
    canvas_begin_path(cv);
    canvas_move_to(cv, 6.0f, 32.0f);
    canvas_line_to(cv, 58.0f, 32.0f);
    canvas_stroke(cv);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 0, 255, 0, 255, 2));  // solid green again

    canvas_destroy(cv);
    free(px);
    return TEST_REPORT();
}
