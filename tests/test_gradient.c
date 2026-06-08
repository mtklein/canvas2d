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

    // Horizontal linear gradient red -> blue across the canvas.
    canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, (float)w, 0.0f);
    canvas_add_fill_color_stop(cv, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_add_fill_color_stop(cv, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);  // gradients ignore fill_rect

    canvas_begin_path(cv);
    canvas_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_fill(cv);
    canvas_read_rgba(cv, px, len);
    struct px4 lft = pixel_at(px, len, w, 6, 32);
    struct px4 mid = pixel_at(px, len, w, 32, 32);
    struct px4 rgt = pixel_at(px, len, w, 58, 32);
    CHECK(lft.r > 200 && lft.b < 60 && lft.g < 20);                  // near red
    CHECK(rgt.b > 200 && rgt.r < 60 && rgt.g < 20);                  // near blue
    CHECK(mid.r > 96 && mid.r < 160 && mid.b > 96 && mid.b < 160);   // purple

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
    CHECK(px_near(pixel_at(px, len, w, 6, 32), 51, 51, 51, 255, 2));  // restored solid

    canvas_destroy(cv);
    free(px);
    return TEST_REPORT();
}
