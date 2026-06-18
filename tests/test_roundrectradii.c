#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>

// Fill a fresh red round-rect-radii path and read the canvas back.
static void draw(struct canvas *__single cv, uint8_t *__counted_by(len) px, int len,
                 float x, float y, float w, float h,
                 float tl_x, float tl_y, float tr_x, float tr_y,
                 float br_x, float br_y, float bl_x, float bl_y) {
    canvas_clear_rect(cv, 0.0f, 0.0f, 40.0f, 40.0f);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_begin_path(cv);
    canvas_round_rect_radii(cv, x, y, w, h, tl_x, tl_y, tr_x, tr_y,
                            br_x, br_y, bl_x, bl_y);
    canvas_fill(cv, CANVAS_NONZERO);
    canvas_read_rgba(cv, px, len);
}

static bool red(uint8_t const *__counted_by(len) px, int len, int x, int y) {
    return px_near(pixel_at(px, len, 40, x, y), 255, 0, 0, 255, 2);
}
static bool clear(uint8_t const *__counted_by(len) px, int len, int x, int y) {
    return px_near(pixel_at(px, len, 40, x, y), 0, 0, 0, 0, 2);
}

int main(void) {
    int const w = 40, h = 40, len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }
    struct canvas *__single cv = canvas(w, h);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    // Uniform circular radius 8 on a [4,36]x[4,36] rect: corners rounded away,
    // centre and straight edge midpoints filled.
    draw(cv, px, len, 4, 4, 32, 32, 8, 8, 8, 8, 8, 8, 8, 8);
    CHECK(red(px, len, 20, 20));    // centre
    CHECK(red(px, len, 4, 20));     // left edge midpoint (straight)
    CHECK(red(px, len, 20, 4));     // top edge midpoint (straight)
    CHECK(clear(px, len, 5, 5));    // top-left corner rounded off
    CHECK(clear(px, len, 34, 34));  // bottom-right corner rounded off

    // Per-corner: a sharp top-left (radius 0) keeps its corner; the rounded
    // top-right loses its corner.
    draw(cv, px, len, 4, 4, 32, 32, 0, 0, 8, 8, 8, 8, 8, 8);
    CHECK(red(px, len, 5, 5));      // sharp TL corner is filled
    CHECK(clear(px, len, 34, 5));   // rounded TR corner is cut away
    CHECK(red(px, len, 20, 20));    // centre still filled

    // Elliptical top-left (rx=4, ry=16): the rounding runs far down the left
    // edge (to y=20), so a point a circular r=4 corner would keep is now cut.
    draw(cv, px, len, 4, 4, 32, 32, 4, 16, 0, 0, 0, 0, 0, 0);
    CHECK(clear(px, len, 4, 8));    // high on the left edge: eaten by the tall ry
    CHECK(red(px, len, 4, 24));     // below the corner: straight left edge
    CHECK(red(px, len, 20, 20));    // centre
    CHECK(red(px, len, 34, 5));     // sharp TR corner unaffected

    // Oversized radii (1000 on every corner) scale down to a 16px inscribed
    // circle in the 32x32 rect: corners gone, centre and edge midpoints kept.
    draw(cv, px, len, 4, 4, 32, 32, 1000, 1000, 1000, 1000,
         1000, 1000, 1000, 1000);
    CHECK(red(px, len, 20, 20));    // centre
    CHECK(red(px, len, 20, 5));     // top of the inscribed circle
    CHECK(clear(px, len, 6, 6));    // corner well outside the circle

    canvas_free(cv);
    free(px);
    return TEST_REPORT();
}
