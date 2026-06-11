#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>

// Paint an opaque backdrop, then a source under `op`, and read the centre pixel.
static struct px4 blend(struct canvas *__single cv, int w, int h,
                        uint8_t *__counted_by(len) px, int len,
                        enum canvas_composite_op op,
                        float br, float bg, float bb,
                        float sr, float sg, float sb, float sa) {
    canvas_set_global_composite_operation(cv, CANVAS_OP_SOURCE_OVER);
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_rgba(cv, br, bg, bb, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_global_composite_operation(cv, op);
    canvas_set_fill_rgba(cv, sr, sg, sb, sa);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_read_rgba(cv, px, len);
    return pixel_at(px, len, w, w / 2, h / 2);
}

int main(void) {
    int const w = 8, h = 8, len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }
    struct canvas *__single cv = canvas_create(w, h);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    // Backdrop opaque red, source opaque green, unless noted.  Tolerance covers
    // the 16F round-trip.
    int const t = 2;

    // multiply: red*green = black.
    CHECK(px_near(blend(cv, w, h, px, len, CANVAS_OP_MULTIPLY,
                        1, 0, 0, 0, 1, 0, 1), 0, 0, 0, 255, t));
    // screen: red(+)green = yellow.
    CHECK(px_near(blend(cv, w, h, px, len, CANVAS_OP_SCREEN,
                        1, 0, 0, 0, 1, 0, 1), 255, 255, 0, 255, t));
    // lighter: additive red+green = yellow (alpha clamps to 1).
    CHECK(px_near(blend(cv, w, h, px, len, CANVAS_OP_LIGHTER,
                        1, 0, 0, 0, 1, 0, 1), 255, 255, 0, 255, t));
    // darken: min(red,green) per channel = black.
    CHECK(px_near(blend(cv, w, h, px, len, CANVAS_OP_DARKEN,
                        1, 0, 0, 0, 1, 0, 1), 0, 0, 0, 255, t));
    // lighten: max(red,green) = yellow.
    CHECK(px_near(blend(cv, w, h, px, len, CANVAS_OP_LIGHTEN,
                        1, 0, 0, 0, 1, 0, 1), 255, 255, 0, 255, t));
    // difference: |red - white| = cyan.
    CHECK(px_near(blend(cv, w, h, px, len, CANVAS_OP_DIFFERENCE,
                        1, 0, 0, 1, 1, 1, 1), 0, 255, 255, 255, t));
    // color-dodge: 0.5 over 0.5 -> 0.5/(1-0.5) = 1 -> white (exercises the
    // un-premultiplied path).
    CHECK(px_near(blend(cv, w, h, px, len, CANVAS_OP_COLOR_DODGE,
                        0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1), 255, 255, 255, 255, t));
    // soft-light: src 0.25 (<=0.5) over backdrop 0.5 -> 0.5 - 0.5*0.5*0.5 = 0.375.
    CHECK(px_near(blend(cv, w, h, px, len, CANVAS_OP_SOFT_LIGHT,
                        0.5f, 0.5f, 0.5f, 0.25f, 0.25f, 0.25f, 1), 96, 96, 96, 255, 3));
    // Translucent: 50% green screened over opaque red -> (255,128,0), exercising
    // the premultiplied s*(1-da)+d*(1-sa)+T path, not just the opaque reduction.
    CHECK(px_near(blend(cv, w, h, px, len, CANVAS_OP_SCREEN,
                        1, 0, 0, 0, 1, 0, 0.5f), 255, 128, 0, 255, 3));
    // copy: source replaces backdrop entirely = green.
    CHECK(px_near(blend(cv, w, h, px, len, CANVAS_OP_COPY,
                        1, 0, 0, 0, 1, 0, 1), 0, 255, 0, 255, t));
    // source-atop over an opaque backdrop = the source colour, backdrop alpha.
    CHECK(px_near(blend(cv, w, h, px, len, CANVAS_OP_SOURCE_ATOP,
                        1, 0, 0, 0, 1, 0, 1), 0, 255, 0, 255, t));
    // destination-out: opaque source erases the backdrop -> transparent.
    CHECK(px_near(blend(cv, w, h, px, len, CANVAS_OP_DESTINATION_OUT,
                        1, 0, 0, 0, 1, 0, 1), 0, 0, 0, 0, t));
    // xor of two fully-overlapping opaque colours -> transparent.
    CHECK(px_near(blend(cv, w, h, px, len, CANVAS_OP_XOR,
                        1, 0, 0, 0, 1, 0, 1), 0, 0, 0, 0, t));

    // A non-source-over mode must not corrupt the default path afterwards:
    // source-over half-green over red is the usual 50% mix.
    struct px4 so = blend(cv, w, h, px, len, CANVAS_OP_SOURCE_OVER,
                          1, 0, 0, 0, 1, 0, 0.5f);
    CHECK(px_near(so, 128, 128, 0, 255, 3));

    canvas_destroy(cv);
    free(px);
    return TEST_REPORT();
}
