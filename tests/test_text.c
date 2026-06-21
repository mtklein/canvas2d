#include "canvas2d.h"
#include "test_pixels.h"
#include "test_util.h"

#include <math.h>
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

    struct canvas2d_context *__single cv = canvas2d(w, h, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    canvas2d_set_font_size(cv, 48.0f);

    // measureText: non-empty has positive advance; empty string is zero.
    float const adv = canvas2d_measure_text(cv, "Hi");
    float const adv_empty = canvas2d_measure_text(cv, "");
    CHECK(adv > 1.0f);
    CHECK(adv_empty <= 0.0f);

    // fill_text marks ink; a longer string advances wider.
    CHECK(canvas2d_measure_text(cv, "Hello") > adv);

    // White ground, black fill_text -> a healthy patch of ink.
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_fill_text(cv, "Hello", 10.0f, 55.0f);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    long ink = ink_count(px, len, n, 128);
    CHECK(ink > 200);

    // Non-finite coordinates draw nothing (the spec's "infinite or NaN ->
    // return").  Regression for a fuzz_replay OOM finding: an inf pen used to
    // poison every glyph point and blow the curve flattener up to its
    // 2^depth-cap segments per curve.
    float const inf = HUGE_VALF;
    canvas2d_fill_text(cv, "Hello", inf, 55.0f);
    canvas2d_fill_text(cv, "Hello", 10.0f, -inf);
    canvas2d_stroke_text(cv, "Hello", NAN, 55.0f);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(ink_count(px, len, n, 128) == ink);  // exactly the ink already there

    // UTF-8: a Chinese string (3-byte code points) maps to glyphs, measures wider
    // than empty, and renders ink -- Libian TC carries both scripts.
    CHECK(canvas2d_measure_text(cv, "\xe4\xbd\xa0\xe5\xa5\xbd") > 1.0f);  // 你好
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_fill_text(cv, "\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c", 10.0f, 55.0f);  // 你好世界
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(ink_count(px, len, n, 128) > 200);

    // Empty string draws nothing.
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_fill_text(cv, "", 10.0f, 55.0f);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(ink_count(px, len, n, 128) == 0);

    // Clip excludes the text region -> nothing drawn.
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_save(cv);
    canvas2d_begin_path(cv);
    canvas2d_rect(cv, 0.0f, 0.0f, 4.0f, 4.0f);  // tiny corner, far from the text
    canvas2d_clip(cv, CANVAS2D_NONZERO);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_fill_text(cv, "Hello", 40.0f, 55.0f);
    canvas2d_restore(cv);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(ink_count(px, len, n, 128) == 0);

    // global alpha applies: black text at 0.5 over white never reaches full black.
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_set_global_alpha(cv, 0.5f);
    canvas2d_fill_text(cv, "Hello", 10.0f, 55.0f);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(ink_count(px, len, n, 64) == 0);   // no near-black pixels
    CHECK(ink_count(px, len, n, 200) > 200);  // but plenty of mid-grey ink
    canvas2d_set_global_alpha(cv, 1.0f);

    // stroke_text also produces ink.
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_set_stroke_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_set_line_width(cv, 2.0f);
    canvas2d_stroke_text(cv, "Hi", 10.0f, 55.0f);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(ink_count(px, len, n, 128) > 50);

    canvas2d_free(cv);
    free(px);
    return TEST_REPORT();
}
