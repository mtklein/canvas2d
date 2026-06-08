// Renders the showcase PNGs in gallery/.  Built like any consumer (public API,
// -std=c23 -fbounds-safety -Weverything), then run by `ninja images`.

#include "canvas.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static float const TAU = 6.2831853f;

static void save(canvas *__single cv, char const *__null_terminated path) {
    if (!canvas_write_png(cv, path)) {
        (void)fprintf(stderr, "gallery: write failed: %s\n", path);
    }
    canvas_destroy(cv);
}

static void star(canvas *__single cv, float cx, float cy, float r) {
    canvas_begin_path(cv);
    for (int i = 0; i < 5; i++) {
        float a = -TAU * 0.25f + (float)i * (TAU * 0.4f);
        if (i == 0) {
            canvas_move_to(cv, cx + r * cosf(a), cy + r * sinf(a));
        } else {
            canvas_line_to(cv, cx + r * cosf(a), cy + r * sinf(a));
        }
    }
    canvas_close_path(cv);
}

// Transforms, alpha, filled curves/arcs, and strokes.
static void shapes(void) {
    canvas *__single c = canvas_create(240, 180);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.12f, 0.12f, 0.16f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 240.0f, 180.0f);

    canvas_set_fill_rgba(c, 0.90f, 0.25f, 0.25f, 1.0f);
    canvas_fill_rect(c, 20.0f, 20.0f, 55.0f, 55.0f);

    canvas_save(c);
    canvas_translate(c, 150.0f, 52.0f);
    canvas_rotate(c, 0.5f);
    canvas_set_fill_rgba(c, 0.25f, 0.80f, 0.35f, 1.0f);
    canvas_fill_rect(c, -28.0f, -28.0f, 56.0f, 56.0f);
    canvas_restore(c);

    canvas_set_global_alpha(c, 0.5f);
    canvas_set_fill_rgba(c, 0.25f, 0.45f, 0.95f, 1.0f);
    canvas_fill_rect(c, 55.0f, 95.0f, 110.0f, 65.0f);
    canvas_set_global_alpha(c, 1.0f);

    canvas_set_fill_rgba(c, 0.30f, 0.85f, 0.85f, 1.0f);
    canvas_begin_path(c);
    canvas_move_to(c, 100.0f, 28.0f);
    canvas_bezier_curve_to(c, 150.0f, 8.0f, 150.0f, 88.0f, 100.0f, 68.0f);
    canvas_bezier_curve_to(c, 70.0f, 58.0f, 70.0f, 38.0f, 100.0f, 28.0f);
    canvas_close_path(c);
    canvas_fill(c);

    canvas_set_fill_rgba(c, 0.85f, 0.30f, 0.75f, 1.0f);
    canvas_begin_path(c);
    canvas_arc(c, 198.0f, 135.0f, 28.0f, 0.0f, TAU, false);
    canvas_fill(c);
    canvas_set_stroke_rgba(c, 1.0f, 0.60f, 0.10f, 1.0f);
    canvas_set_line_width(c, 4.0f);
    canvas_begin_path(c);
    canvas_arc(c, 198.0f, 135.0f, 28.0f, 0.0f, TAU, false);
    canvas_close_path(c);
    canvas_stroke(c);

    canvas_set_stroke_rgba(c, 0.95f, 0.95f, 0.98f, 1.0f);
    canvas_set_line_width(c, 5.0f);
    canvas_begin_path(c);
    canvas_move_to(c, 20.0f, 125.0f);
    canvas_line_to(c, 45.0f, 100.0f);
    canvas_line_to(c, 70.0f, 125.0f);
    canvas_line_to(c, 95.0f, 100.0f);
    canvas_stroke(c);

    save(c, "gallery/shapes.png");
}

// Winding rules: a donut (nonzero hole) and a pentagram filled both ways.
static void winding(void) {
    canvas *__single c = canvas_create(300, 120);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 300.0f, 120.0f);

    canvas_set_fill_rule(c, CANVAS_NONZERO);
    canvas_set_fill_rgba(c, 0.95f, 0.80f, 0.25f, 1.0f);
    canvas_begin_path(c);
    canvas_rect(c, 20.0f, 20.0f, 80.0f, 80.0f);
    canvas_move_to(c, 85.0f, 35.0f);
    canvas_line_to(c, 35.0f, 35.0f);
    canvas_line_to(c, 35.0f, 85.0f);
    canvas_line_to(c, 85.0f, 85.0f);
    canvas_close_path(c);
    canvas_fill(c);

    star(c, 160.0f, 60.0f, 40.0f);
    canvas_set_fill_rule(c, CANVAS_NONZERO);
    canvas_set_fill_rgba(c, 0.90f, 0.30f, 0.40f, 1.0f);
    canvas_fill(c);

    star(c, 250.0f, 60.0f, 40.0f);
    canvas_set_fill_rule(c, CANVAS_EVENODD);
    canvas_set_fill_rgba(c, 0.30f, 0.75f, 0.95f, 1.0f);
    canvas_fill(c);

    save(c, "gallery/winding.png");
}

// Line dashing: a few patterns, an offset, and a dashed circle.
static void dashes(void) {
    canvas *__single c = canvas_create(260, 120);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 260.0f, 120.0f);
    canvas_set_line_width(c, 4.0f);

    float const d1[2] = { 14.0f, 9.0f };
    canvas_set_line_dash(c, d1, 2);
    canvas_set_stroke_rgba(c, 0.90f, 0.45f, 0.45f, 1.0f);
    canvas_begin_path(c);
    canvas_move_to(c, 15.0f, 26.0f);
    canvas_line_to(c, 245.0f, 26.0f);
    canvas_stroke(c);

    float const d2[4] = { 2.0f, 7.0f, 14.0f, 7.0f };
    canvas_set_line_dash(c, d2, 4);
    canvas_set_stroke_rgba(c, 0.45f, 0.85f, 0.55f, 1.0f);
    canvas_begin_path(c);
    canvas_move_to(c, 15.0f, 50.0f);
    canvas_line_to(c, 245.0f, 50.0f);
    canvas_stroke(c);

    float const d3[2] = { 2.0f, 6.0f };
    canvas_set_line_dash(c, d3, 2);
    canvas_set_stroke_rgba(c, 0.55f, 0.65f, 0.95f, 1.0f);
    canvas_begin_path(c);
    canvas_move_to(c, 15.0f, 72.0f);
    canvas_line_to(c, 245.0f, 72.0f);
    canvas_stroke(c);

    float const d4[2] = { 11.0f, 8.0f };
    canvas_set_line_dash(c, d4, 2);
    canvas_set_line_width(c, 3.0f);
    canvas_set_stroke_rgba(c, 0.95f, 0.85f, 0.30f, 1.0f);
    canvas_begin_path(c);
    canvas_arc(c, 130.0f, 98.0f, 16.0f, 0.0f, TAU, false);
    canvas_close_path(c);
    canvas_stroke(c);

    save(c, "gallery/dashes.png");
}

// getImageData / putImageData: capture a motif and stamp copies of it.
static void imagedata(void) {
    canvas *__single c = canvas_create(240, 90);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.12f, 0.12f, 0.16f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 240.0f, 90.0f);

    canvas_set_fill_rgba(c, 0.95f, 0.80f, 0.25f, 1.0f);
    canvas_begin_path(c);
    canvas_arc(c, 30.0f, 45.0f, 17.0f, 0.0f, TAU, false);
    canvas_fill(c);
    canvas_set_fill_rgba(c, 0.85f, 0.30f, 0.75f, 1.0f);
    canvas_fill_rect(c, 12.0f, 27.0f, 14.0f, 14.0f);

    int const blen = 44 * 44 * 4;
    uint8_t *__counted_by(blen) block = malloc((size_t)blen);
    if (block) {
        canvas_get_image_data(c, 8, 23, 44, 44, block, blen);
        for (int k = 1; k < 5; k++) {
            canvas_put_image_data(c, block, blen, 44, 44, 8 + k * 46, 23);
        }
        free(block);
    }

    save(c, "gallery/imagedata.png");
}

// Line joins (miter / round / bevel) on sharp Vs, and caps (butt / round /
// square) on short segments.
static void joinscaps(void) {
    canvas *__single c = canvas_create(280, 160);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 280.0f, 160.0f);

    canvas_line_join const js[3] = { CANVAS_JOIN_MITER, CANVAS_JOIN_ROUND,
                                     CANVAS_JOIN_BEVEL };
    canvas_set_line_width(c, 16.0f);
    for (int k = 0; k < 3; k++) {
        canvas_set_line_join(c, js[k]);
        canvas_set_stroke_rgba(c, 0.95f, 0.55f, 0.35f, 1.0f);
        float cx = 55.0f + (float)k * 85.0f;
        canvas_begin_path(c);
        canvas_move_to(c, cx - 28.0f, 80.0f);
        canvas_line_to(c, cx, 26.0f);
        canvas_line_to(c, cx + 28.0f, 80.0f);
        canvas_stroke(c);
    }

    canvas_line_cap const cs[3] = { CANVAS_CAP_BUTT, CANVAS_CAP_ROUND,
                                    CANVAS_CAP_SQUARE };
    canvas_set_line_join(c, CANVAS_JOIN_MITER);
    for (int k = 0; k < 3; k++) {
        canvas_set_line_cap(c, cs[k]);
        canvas_set_stroke_rgba(c, 0.40f, 0.80f, 0.95f, 1.0f);
        float x0 = 45.0f + (float)k * 80.0f;
        canvas_begin_path(c);
        canvas_move_to(c, x0, 130.0f);
        canvas_line_to(c, x0 + 45.0f, 130.0f);
        canvas_stroke(c);
    }

    save(c, "gallery/joins.png");
}

int main(void) {
    shapes();
    winding();
    dashes();
    imagedata();
    joinscaps();
    return 0;
}
