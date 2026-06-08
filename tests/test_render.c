#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>

int main(void) {
    const int w = 8;
    const int h = 8;
    const int len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }

    // Whole canvas opaque red.
    canvas *__single cv = canvas_create(w, h);
    CHECK(cv != NULL);
    if (cv) {
        canvas_set_fill_rgba(cv, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_read_rgba(cv, px, len);
        CHECK(px_near(pixel_at(px, len, w, 4, 4), 255, 0, 0, 255, 1));
        CHECK(px_near(pixel_at(px, len, w, 0, 0), 255, 0, 0, 255, 1));

        // clearRect erases a sub-region; outside stays red.
        canvas_clear_rect(cv, 2.0f, 2.0f, 4.0f, 4.0f);
        canvas_read_rgba(cv, px, len);
        CHECK(px_near(pixel_at(px, len, w, 4, 4), 0, 0, 0, 0, 1));
        CHECK(px_near(pixel_at(px, len, w, 0, 0), 255, 0, 0, 255, 1));
        canvas_destroy(cv);
    }

    // 50% blue over red -> ~(128, 0, 128, 255).
    canvas *__single cb = canvas_create(w, h);
    CHECK(cb != NULL);
    if (cb) {
        canvas_set_fill_rgba(cb, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_fill_rect(cb, 0.0f, 0.0f, (float)w, (float)h);
        canvas_set_fill_rgba(cb, 0.0f, 0.0f, 1.0f, 1.0f);
        canvas_set_global_alpha(cb, 0.5f);
        canvas_fill_rect(cb, 0.0f, 0.0f, (float)w, (float)h);
        canvas_read_rgba(cb, px, len);
        CHECK(px_near(pixel_at(px, len, w, 4, 4), 128, 0, 128, 255, 3));
        canvas_destroy(cb);
    }
    free(px);

    // Demo image for eyeballing: background, a square, a rotated square, and a
    // translucent rectangle.
    canvas *__single demo = canvas_create(200, 200);
    if (demo) {
        canvas_set_fill_rgba(demo, 0.12f, 0.12f, 0.16f, 1.0f);
        canvas_fill_rect(demo, 0.0f, 0.0f, 200.0f, 200.0f);

        canvas_set_fill_rgba(demo, 0.90f, 0.25f, 0.25f, 1.0f);
        canvas_fill_rect(demo, 20.0f, 20.0f, 60.0f, 60.0f);

        canvas_save(demo);
        canvas_translate(demo, 140.0f, 60.0f);
        canvas_rotate(demo, 0.5f);
        canvas_set_fill_rgba(demo, 0.25f, 0.80f, 0.35f, 1.0f);
        canvas_fill_rect(demo, -30.0f, -30.0f, 60.0f, 60.0f);
        canvas_restore(demo);

        canvas_set_global_alpha(demo, 0.5f);
        canvas_set_fill_rgba(demo, 0.25f, 0.45f, 0.95f, 1.0f);
        canvas_fill_rect(demo, 60.0f, 110.0f, 100.0f, 70.0f);
        canvas_set_global_alpha(demo, 1.0f);

        // Filled triangle path.
        canvas_set_fill_rgba(demo, 0.95f, 0.80f, 0.20f, 1.0f);
        canvas_begin_path(demo);
        canvas_move_to(demo, 30.0f, 150.0f);
        canvas_line_to(demo, 70.0f, 120.0f);
        canvas_line_to(demo, 50.0f, 185.0f);
        canvas_close_path(demo);
        canvas_fill(demo);

        // Filled circle via arc.
        canvas_set_fill_rgba(demo, 0.85f, 0.30f, 0.75f, 1.0f);
        canvas_begin_path(demo);
        canvas_arc(demo, 150.0f, 150.0f, 28.0f, 0.0f, 2.0f * (float)M_PI, false);
        canvas_fill(demo);

        // Filled cubic-Bezier "leaf".
        canvas_set_fill_rgba(demo, 0.30f, 0.85f, 0.85f, 1.0f);
        canvas_begin_path(demo);
        canvas_move_to(demo, 100.0f, 30.0f);
        canvas_bezier_curve_to(demo, 150.0f, 10.0f, 150.0f, 90.0f, 100.0f, 70.0f);
        canvas_bezier_curve_to(demo, 70.0f, 60.0f, 70.0f, 40.0f, 100.0f, 30.0f);
        canvas_close_path(demo);
        canvas_fill(demo);

        // Stroked zig-zag (bevel joins, butt caps).
        canvas_set_stroke_rgba(demo, 0.95f, 0.95f, 0.98f, 1.0f);
        canvas_set_line_width(demo, 5.0f);
        canvas_begin_path(demo);
        canvas_move_to(demo, 20.0f, 100.0f);
        canvas_line_to(demo, 45.0f, 75.0f);
        canvas_line_to(demo, 70.0f, 100.0f);
        canvas_line_to(demo, 95.0f, 75.0f);
        canvas_stroke(demo);

        // Stroked outline around the magenta disc.
        canvas_set_stroke_rgba(demo, 1.0f, 0.60f, 0.10f, 1.0f);
        canvas_set_line_width(demo, 4.0f);
        canvas_begin_path(demo);
        canvas_arc(demo, 150.0f, 150.0f, 28.0f, 0.0f, 2.0f * (float)M_PI, false);
        canvas_close_path(demo);
        canvas_stroke(demo);

        CHECK(canvas_write_png(demo, "build/m1_demo.png"));
        canvas_destroy(demo);
    }

    return TEST_REPORT();
}
