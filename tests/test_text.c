#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>

// Count pixels darker than `thr` on the red channel (ink over a white ground).
static long ink_count(uint8_t const *__counted_by(len) px, int len, int n, int thr) {
    (void)len;
    long ink = 0;
    for (int i = 0; i < n; i++) {
        if (px[i * 4] < thr) {
            ink++;
        }
    }
    return ink;
}

int main(void) {
    int const w = 240;
    int const h = 80;
    int const len = w * h * 4;
    int const n = w * h;
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

    canvas_set_font_size(cv, 48.0f);

    // measureText: non-empty has positive advance; empty string is zero.
    float adv = canvas_measure_text(cv, "Hi");
    float adv_empty = canvas_measure_text(cv, "");
    CHECK(adv > 1.0f);
    CHECK(adv_empty <= 0.0f);

    // fill_text marks ink; a longer string advances wider.
    CHECK(canvas_measure_text(cv, "Hello") > adv);

    // White ground, black fill_text -> a healthy patch of ink.
    canvas_set_fill_rgba(cv, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_rgba(cv, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_text(cv, "Hello", 10.0f, 55.0f);
    canvas_read_rgba(cv, px, len);
    long ink = ink_count(px, len, n, 128);
    CHECK(ink > 200);

    // Empty string draws nothing.
    canvas_set_fill_rgba(cv, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_rgba(cv, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_text(cv, "", 10.0f, 55.0f);
    canvas_read_rgba(cv, px, len);
    CHECK(ink_count(px, len, n, 128) == 0);

    // Clip excludes the text region -> nothing drawn.
    canvas_set_fill_rgba(cv, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_save(cv);
    canvas_begin_path(cv);
    canvas_rect(cv, 0.0f, 0.0f, 4.0f, 4.0f);  // tiny corner, far from the text
    canvas_clip(cv);
    canvas_set_fill_rgba(cv, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_text(cv, "Hello", 40.0f, 55.0f);
    canvas_restore(cv);
    canvas_read_rgba(cv, px, len);
    CHECK(ink_count(px, len, n, 128) == 0);

    // global alpha applies: black text at 0.5 over white never reaches full black.
    canvas_set_fill_rgba(cv, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_rgba(cv, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas_set_global_alpha(cv, 0.5f);
    canvas_fill_text(cv, "Hello", 10.0f, 55.0f);
    canvas_read_rgba(cv, px, len);
    CHECK(ink_count(px, len, n, 64) == 0);   // no near-black pixels
    CHECK(ink_count(px, len, n, 200) > 200);  // but plenty of mid-grey ink
    canvas_set_global_alpha(cv, 1.0f);

    // stroke_text also produces ink.
    canvas_set_fill_rgba(cv, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_stroke_rgba(cv, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas_set_line_width(cv, 2.0f);
    canvas_stroke_text(cv, "Hi", 10.0f, 55.0f);
    canvas_read_rgba(cv, px, len);
    CHECK(ink_count(px, len, n, 128) > 50);

    canvas_destroy(cv);
    free(px);
    return TEST_REPORT();
}
