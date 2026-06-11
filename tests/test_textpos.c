#include "canvas.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>

#define W 240
#define H 160

// Render "Hi" with the given align/baseline at (x0,y0) on a white ground in black
// and report the ink bounding box (pixels with a dark red channel).  Returns the
// ink pixel count.
static long ink_bbox(struct canvas *__single cv, uint8_t *__counted_by(len) px, int len,
                     enum canvas_text_align align, enum canvas_text_baseline base,
                     float x0, float y0,
                     int *__single ixmin, int *__single iymin) {
    canvas_set_fill_rgba(cv, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_set_fill_rgba(cv, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas_set_text_align(cv, align);
    canvas_set_text_baseline(cv, base);
    canvas_fill_text(cv, "Hi", x0, y0);
    canvas_read_rgba(cv, px, len);
    *ixmin = W;
    *iymin = H;
    long n = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (px[(y * W + x) * 4] < 128) {
                n++;
                if (x < *ixmin) { *ixmin = x; }
                if (y < *iymin) { *iymin = y; }
            }
        }
    }
    return n;
}

int main(void) {
    int const len = W * H * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }
    struct canvas *__single cv = canvas_create(W, H);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }
    canvas_set_font_size(cv, 40.0f);
    float adv = canvas_measure_text(cv, "Hi");
    CHECK(adv > 0.0f);

    // textAlign: the ink's left edge shifts left by frac*advance as we go
    // left -> center -> right (advance-based, so the shift is exact).
    int lx, cx, rx, ly, cy, ry;
    CHECK(ink_bbox(cv, px, len, CANVAS_ALIGN_LEFT, CANVAS_BASELINE_ALPHABETIC,
                   120.0f, 90.0f, &lx, &ly) > 0);
    CHECK(ink_bbox(cv, px, len, CANVAS_ALIGN_CENTER, CANVAS_BASELINE_ALPHABETIC,
                   120.0f, 90.0f, &cx, &cy) > 0);
    CHECK(ink_bbox(cv, px, len, CANVAS_ALIGN_RIGHT, CANVAS_BASELINE_ALPHABETIC,
                   120.0f, 90.0f, &rx, &ry) > 0);
    CHECK(fabsf((float)(lx - cx) - adv * 0.5f) <= 3.0f);
    CHECK(fabsf((float)(cx - rx) - adv * 0.5f) <= 3.0f);
    // start == left under LTR: identical placement.
    int sx, sy;
    CHECK(ink_bbox(cv, px, len, CANVAS_ALIGN_START, CANVAS_BASELINE_ALPHABETIC,
                   120.0f, 90.0f, &sx, &sy) > 0);
    CHECK(sx == lx);

    // textBaseline: the ink's top edge moves down as the baseline goes
    // bottom -> alphabetic -> top (the baseline is pushed down by ascent/descent).
    int ax, ay, tx, ty, bx, by, mx, my, ix, iy;
    CHECK(ink_bbox(cv, px, len, CANVAS_ALIGN_LEFT, CANVAS_BASELINE_ALPHABETIC,
                   20.0f, 80.0f, &ax, &ay) > 0);
    CHECK(ink_bbox(cv, px, len, CANVAS_ALIGN_LEFT, CANVAS_BASELINE_TOP,
                   20.0f, 80.0f, &tx, &ty) > 0);
    CHECK(ink_bbox(cv, px, len, CANVAS_ALIGN_LEFT, CANVAS_BASELINE_BOTTOM,
                   20.0f, 80.0f, &bx, &by) > 0);
    CHECK(ink_bbox(cv, px, len, CANVAS_ALIGN_LEFT, CANVAS_BASELINE_MIDDLE,
                   20.0f, 80.0f, &mx, &my) > 0);
    CHECK(ink_bbox(cv, px, len, CANVAS_ALIGN_LEFT, CANVAS_BASELINE_IDEOGRAPHIC,
                   20.0f, 80.0f, &ix, &iy) > 0);
    CHECK(by < ay);   // bottom baseline -> text sits higher
    CHECK(ay < ty);   // top baseline -> text sits lower
    CHECK(by < my && my < ty);     // middle strictly between
    CHECK(abs(iy - by) <= 1);      // ideographic ~ bottom (no BASE table)

    canvas_destroy(cv);
    free(px);
    return TEST_REPORT();
}
