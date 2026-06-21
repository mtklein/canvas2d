#include "canvas2d.h"
#include "canvas2d_path2d.h"
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
    struct canvas2d_context *__single cv = canvas2d(W, W, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    // fill_path fills the Path2D and leaves the canvas's current path untouched.
    canvas2d_begin_path(cv);
    canvas2d_rect(cv, 0.0f, 0.0f, 4.0f, 4.0f);  // current path (not yet filled)
    struct canvas2d_path2d *__single rp = canvas2d_path2d();
    CHECK(rp != NULL);
    canvas2d_path2d_rect(rp, 10.0f, 10.0f, 12.0f, 12.0f);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_fill_path(cv, rp, CANVAS2D_NONZERO);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(red(px, len, 16, 16));   // inside the Path2D rect
    CHECK(clear(px, len, 2, 2));   // current path was NOT filled by fill_path
    // Filling the current path now proves it survived intact.
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas2d_fill(cv, CANVAS2D_NONZERO);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, W, 1, 1), 0, 0, 255, 255, 2));
    canvas2d_path2d_free(rp);

    // The Path2D honours the current transform: under translate(8,8) the rect
    // lands at device (8,8)-(20,20).
    canvas2d_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    struct canvas2d_path2d *__single tp = canvas2d_path2d();
    canvas2d_path2d_rect(tp, 0.0f, 0.0f, 12.0f, 12.0f);
    canvas2d_save(cv);
    canvas2d_translate(cv, 8.0f, 8.0f);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_fill_path(cv, tp, CANVAS2D_NONZERO);
    canvas2d_restore(cv);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(red(px, len, 14, 14));   // inside the translated rect
    CHECK(clear(px, len, 2, 2));   // origin untouched
    canvas2d_path2d_free(tp);

    // Explicit fill rule: nested same-wound rects fill the centre under nonzero
    // but leave it hollow under even-odd.
    struct canvas2d_path2d *__single np = canvas2d_path2d();
    canvas2d_path2d_rect(np, 4.0f, 4.0f, 32.0f, 32.0f);
    canvas2d_path2d_rect(np, 14.0f, 14.0f, 12.0f, 12.0f);
    canvas2d_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_fill_path(cv, np, CANVAS2D_NONZERO);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(red(px, len, 20, 20));   // centre filled
    canvas2d_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas2d_fill_path(cv, np, CANVAS2D_EVENODD);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(clear(px, len, 20, 20));  // centre hollow
    CHECK(red(px, len, 8, 20));     // ring still filled
    canvas2d_path2d_free(np);

    // A curved Path2D (a full-circle arc) fills a disc.
    struct canvas2d_path2d *__single ap = canvas2d_path2d();
    canvas2d_path2d_arc(ap, 20.0f, 20.0f, 12.0f, 0.0f, 2.0f * (float)M_PI, false);
    canvas2d_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_fill_path(cv, ap, CANVAS2D_NONZERO);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(red(px, len, 20, 20));   // centre of the disc
    CHECK(clear(px, len, 4, 4));   // corner outside the disc
    canvas2d_path2d_free(ap);

    // stroke_path strokes a Path2D with the current line styles.
    struct canvas2d_path2d *__single lp = canvas2d_path2d();
    canvas2d_path2d_move_to(lp, 5.0f, 20.0f);
    canvas2d_path2d_line_to(lp, 35.0f, 20.0f);
    canvas2d_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas2d_set_stroke_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_set_line_width(cv, 6.0f);
    canvas2d_stroke_path(cv, lp);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(red(px, len, 20, 20));   // on the stroked line
    CHECK(clear(px, len, 20, 32)); // off the line

    // Hit testing a Path2D (fill and stroke).
    struct canvas2d_path2d *__single hp = canvas2d_path2d();
    canvas2d_path2d_rect(hp, 10.0f, 10.0f, 12.0f, 12.0f);
    CHECK(canvas2d_is_point_in_path2d(cv, hp, 16.0f, 16.0f, CANVAS2D_NONZERO));
    CHECK(!canvas2d_is_point_in_path2d(cv, hp, 2.0f, 2.0f, CANVAS2D_NONZERO));
    canvas2d_path2d_free(hp);
    CHECK(canvas2d_is_point_in_stroke_path(cv, lp, 20.0f, 20.0f));
    CHECK(!canvas2d_is_point_in_stroke_path(cv, lp, 20.0f, 32.0f));
    canvas2d_path2d_free(lp);

    // add_path appends one path's commands to another.
    struct canvas2d_path2d *__single a = canvas2d_path2d();
    struct canvas2d_path2d *__single bp = canvas2d_path2d();
    canvas2d_path2d_rect(a, 4.0f, 4.0f, 8.0f, 8.0f);
    canvas2d_path2d_rect(bp, 24.0f, 24.0f, 8.0f, 8.0f);
    canvas2d_path2d_add_path(a, bp);
    canvas2d_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_fill_path(cv, a, CANVAS2D_NONZERO);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(red(px, len, 7, 7));     // first rect
    CHECK(red(px, len, 27, 27));   // appended rect
    canvas2d_path2d_free(a);
    canvas2d_path2d_free(bp);

    canvas2d_free(cv);
    free(px);
    return TEST_REPORT();
}
