#include "canvas.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>

#define W 240
#define H 80

// Ink bounding-box width and height (black text on a white ground).
static void ink_extent(struct canvas *__single cv, uint8_t *__counted_by(len) px, int len,
                       int *__single iw, int *__single ih) {
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    int xmin = W, xmax = -1, ymin = H, ymax = -1;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (px[(y * W + x) * 4] < 128) {
                if (x < xmin) { xmin = x; }
                if (x > xmax) { xmax = x; }
                if (y < ymin) { ymin = y; }
                if (y > ymax) { ymax = y; }
            }
        }
    }
    *iw = xmax - xmin;
    *ih = ymax - ymin;
}

static void clear_white(struct canvas *__single cv) {
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
}

int main(void) {
    int const len = W * H * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }
    struct canvas *__single cv = canvas(W, H);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }
    canvas_set_font_size(cv, 40.0f);
    char const *__null_terminated s = "Hello";
    float const adv = canvas_measure_text(cv, s);
    CHECK(adv > 0.0f);

    // Baseline: unconstrained render.
    clear_white(cv);
    canvas_fill_text(cv, s, 10.0f, 55.0f);
    int wn, hn;
    ink_extent(cv, px, len, &wn, &hn);
    CHECK(wn > 0 && hn > 0);

    // maxWidth = half the advance: the ink is condensed to ~half width, same
    // height (only x scales).
    clear_white(cv);
    canvas_fill_text_max(cv, s, 10.0f, 55.0f, adv * 0.5f);
    int wh, hh;
    ink_extent(cv, px, len, &wh, &hh);
    CHECK(fabsf((float)wh - 0.5f * (float)wn) <= 4.0f);
    CHECK(abs(hh - hn) <= 2);

    // maxWidth wider than the text: no condensing -- identical to the baseline.
    clear_white(cv);
    canvas_fill_text_max(cv, s, 10.0f, 55.0f, adv * 2.0f);
    int ww, hw;
    ink_extent(cv, px, len, &ww, &hw);
    CHECK(abs(ww - wn) <= 2);

    // maxWidth <= 0 imposes no limit, also identical to the baseline.
    clear_white(cv);
    canvas_fill_text_max(cv, s, 10.0f, 55.0f, 0.0f);
    int wz, hz;
    ink_extent(cv, px, len, &wz, &hz);
    CHECK(abs(wz - wn) <= 2);

    // stroke_text_max condenses too (and still lays down ink).
    clear_white(cv);
    canvas_set_line_width(cv, 2.0f);
    canvas_stroke_text_max(cv, s, 10.0f, 55.0f, adv * 0.5f);
    int ws, hs;
    ink_extent(cv, px, len, &ws, &hs);
    CHECK(ws > 0);
    CHECK(ws < wn);  // narrower than the unconstrained fill

    canvas_free(cv);
    free(px);
    return TEST_REPORT();
}
