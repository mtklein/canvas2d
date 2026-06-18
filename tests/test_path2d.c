#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>

#define W 40

static bool red(uint8_t const *__counted_by(len) px, int len, int x, int y) {
    return px_near(pixel_at(px, len, W, x, y), 255, 0, 0, 255, 2);
}
static bool clear(uint8_t const *__counted_by(len) px, int len, int x, int y) {
    return px_near(pixel_at(px, len, W, x, y), 0, 0, 0, 0, 2);
}

int main(void) {
    int const len = W * W * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }
    struct canvas *__single cv = canvas(W, W);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    // fill_path fills the Path2D and leaves the canvas's current path untouched.
    canvas_begin_path(cv);
    canvas_rect(cv, 0.0f, 0.0f, 4.0f, 4.0f);  // current path (not yet filled)
    struct canvas_path2d *__single rp = canvas_path2d();
    CHECK(rp != NULL);
    canvas_path2d_rect(rp, 10.0f, 10.0f, 12.0f, 12.0f);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_path(cv, rp, CANVAS_NONZERO);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(red(px, len, 16, 16));   // inside the Path2D rect
    CHECK(clear(px, len, 2, 2));   // current path was NOT filled by fill_path
    // Filling the current path now proves it survived intact.
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_fill(cv, CANVAS_NONZERO);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, W, 1, 1), 0, 0, 255, 255, 2));
    canvas_path2d_free(rp);

    // The Path2D honours the current transform: under translate(8,8) the rect
    // lands at device (8,8)-(20,20).
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    struct canvas_path2d *__single tp = canvas_path2d();
    canvas_path2d_rect(tp, 0.0f, 0.0f, 12.0f, 12.0f);
    canvas_save(cv);
    canvas_translate(cv, 8.0f, 8.0f);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_path(cv, tp, CANVAS_NONZERO);
    canvas_restore(cv);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(red(px, len, 14, 14));   // inside the translated rect
    CHECK(clear(px, len, 2, 2));   // origin untouched
    canvas_path2d_free(tp);

    // Explicit fill rule: nested same-wound rects fill the centre under nonzero
    // but leave it hollow under even-odd.
    struct canvas_path2d *__single np = canvas_path2d();
    canvas_path2d_rect(np, 4.0f, 4.0f, 32.0f, 32.0f);
    canvas_path2d_rect(np, 14.0f, 14.0f, 12.0f, 12.0f);
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_path(cv, np, CANVAS_NONZERO);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(red(px, len, 20, 20));   // centre filled
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_fill_path(cv, np, CANVAS_EVENODD);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(clear(px, len, 20, 20));  // centre hollow
    CHECK(red(px, len, 8, 20));     // ring still filled
    canvas_path2d_free(np);

    // A curved Path2D (a full-circle arc) fills a disc.
    struct canvas_path2d *__single ap = canvas_path2d();
    canvas_path2d_arc(ap, 20.0f, 20.0f, 12.0f, 0.0f, 2.0f * (float)M_PI, false);
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_path(cv, ap, CANVAS_NONZERO);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(red(px, len, 20, 20));   // centre of the disc
    CHECK(clear(px, len, 4, 4));   // corner outside the disc
    canvas_path2d_free(ap);

    // stroke_path strokes a Path2D with the current line styles.
    struct canvas_path2d *__single lp = canvas_path2d();
    canvas_path2d_move_to(lp, 5.0f, 20.0f);
    canvas_path2d_line_to(lp, 35.0f, 20.0f);
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_set_stroke_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_set_line_width(cv, 6.0f);
    canvas_stroke_path(cv, lp);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(red(px, len, 20, 20));   // on the stroked line
    CHECK(clear(px, len, 20, 32)); // off the line

    // Hit testing a Path2D (fill and stroke).
    struct canvas_path2d *__single hp = canvas_path2d();
    canvas_path2d_rect(hp, 10.0f, 10.0f, 12.0f, 12.0f);
    CHECK(canvas_is_point_in_path2d(cv, hp, 16.0f, 16.0f, CANVAS_NONZERO));
    CHECK(!canvas_is_point_in_path2d(cv, hp, 2.0f, 2.0f, CANVAS_NONZERO));
    canvas_path2d_free(hp);
    CHECK(canvas_is_point_in_stroke_path(cv, lp, 20.0f, 20.0f));
    CHECK(!canvas_is_point_in_stroke_path(cv, lp, 20.0f, 32.0f));
    canvas_path2d_free(lp);

    // add_path appends one path's commands to another.
    struct canvas_path2d *__single a = canvas_path2d();
    struct canvas_path2d *__single bp = canvas_path2d();
    canvas_path2d_rect(a, 4.0f, 4.0f, 8.0f, 8.0f);
    canvas_path2d_rect(bp, 24.0f, 24.0f, 8.0f, 8.0f);
    canvas_path2d_add_path(a, bp);
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_path(cv, a, CANVAS_NONZERO);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(red(px, len, 7, 7));     // first rect
    CHECK(red(px, len, 27, 27));   // appended rect
    canvas_path2d_free(a);
    canvas_path2d_free(bp);

    canvas_free(cv);
    free(px);
    return TEST_REPORT();
}
