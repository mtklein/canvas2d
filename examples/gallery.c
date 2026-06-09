// Renders the showcase PNGs in gallery/.  Built like any consumer (public API,
// -std=c23 -fbounds-safety -Weverything), then run by `ninja images`.

#include "canvas.h"

#include <math.h>
#include <stdint.h>
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

// Path primitives: a filled ellipse, a rounded rectangle, and an arcTo fillet.
static void paths(void) {
    canvas *__single c = canvas_create(280, 170);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 280.0f, 170.0f);

    canvas_set_fill_rgba(c, 0.40f, 0.65f, 0.95f, 1.0f);
    canvas_begin_path(c);
    canvas_ellipse(c, 72.0f, 52.0f, 52.0f, 28.0f, 0.0f, 0.0f, TAU, false);
    canvas_fill(c);

    canvas_set_fill_rgba(c, 0.55f, 0.85f, 0.45f, 1.0f);
    canvas_begin_path(c);
    canvas_round_rect(c, 158.0f, 16.0f, 100.0f, 72.0f, 22.0f);
    canvas_fill(c);
    canvas_set_stroke_rgba(c, 0.95f, 0.60f, 0.20f, 1.0f);
    canvas_set_line_width(c, 4.0f);
    canvas_begin_path(c);
    canvas_round_rect(c, 158.0f, 16.0f, 100.0f, 72.0f, 22.0f);
    canvas_stroke(c);

    canvas_set_stroke_rgba(c, 0.85f, 0.45f, 0.90f, 1.0f);
    canvas_set_line_width(c, 6.0f);
    canvas_set_line_cap(c, CANVAS_CAP_ROUND);
    canvas_begin_path(c);
    canvas_move_to(c, 30.0f, 150.0f);
    canvas_arc_to(c, 150.0f, 150.0f, 150.0f, 105.0f, 30.0f);
    canvas_line_to(c, 150.0f, 105.0f);
    canvas_stroke(c);

    save(c, "gallery/paths.png");
}

// roundRect with per-corner elliptical radii: four cards, each a different recipe
// of eight (x,y) corner radii, plus a wide capsule whose 200px radii are far
// bigger than its 36px height -- the CSS overlap rule scales them down to a clean
// half-height pill.
static void roundrect(void) {
    canvas *__single c = canvas_create(548, 232);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 548.0f, 232.0f);

    float const cw = 110.0f, ch = 120.0f, gap = 20.0f, x0 = 24.0f, y0 = 24.0f;
    struct {
        float r[8];          // tl_x,tl_y, tr_x,tr_y, br_x,br_y, bl_x,bl_y
        float c0[3], c1[3];  // gradient endpoint colours (rgb)
    } const card[4] = {
        // uniform circular corners
        { { 24.0f, 24.0f, 24.0f, 24.0f, 24.0f, 24.0f, 24.0f, 24.0f },
          { 0.99f, 0.74f, 0.28f }, { 0.95f, 0.40f, 0.30f } },
        // elliptical: wide-flat corners (rx != ry)
        { { 52.0f, 22.0f, 52.0f, 22.0f, 52.0f, 22.0f, 52.0f, 22.0f },
          { 0.40f, 0.85f, 0.55f }, { 0.15f, 0.55f, 0.62f } },
        // leaf: two opposite corners rounded big, the other two sharp
        { { 54.0f, 54.0f, 0.0f, 0.0f, 54.0f, 54.0f, 0.0f, 0.0f },
          { 0.45f, 0.72f, 0.98f }, { 0.28f, 0.40f, 0.95f } },
        // asymmetric grab-bag: every corner different
        { { 6.0f, 6.0f, 50.0f, 22.0f, 12.0f, 46.0f, 34.0f, 34.0f },
          { 0.92f, 0.55f, 0.96f }, { 0.58f, 0.28f, 0.86f } },
    };

    for (int i = 0; i < 4; i++) {
        float x = x0 + (float)i * (cw + gap);
        for (int pass = 0; pass < 2; pass++) {
            canvas_begin_path(c);
            canvas_round_rect_radii(c, x, y0, cw, ch,
                                    card[i].r[0], card[i].r[1], card[i].r[2],
                                    card[i].r[3], card[i].r[4], card[i].r[5],
                                    card[i].r[6], card[i].r[7]);
            if (pass == 0) {
                canvas_set_fill_linear_gradient(c, x, y0, x + cw, y0 + ch);
                canvas_add_fill_color_stop(c, 0.0f, card[i].c0[0], card[i].c0[1],
                                           card[i].c0[2], 1.0f);
                canvas_add_fill_color_stop(c, 1.0f, card[i].c1[0], card[i].c1[1],
                                           card[i].c1[2], 1.0f);
                canvas_fill(c);
            } else {
                canvas_set_stroke_rgba(c, 0.96f, 0.97f, 0.99f, 0.9f);
                canvas_set_line_width(c, 2.0f);
                canvas_stroke(c);
            }
        }
    }

    // Wide capsule: huge radii everywhere, clamped by the overlap rule.
    float const bx = 24.0f, by = 178.0f, bw = 500.0f, bh = 36.0f;
    canvas_set_fill_linear_gradient(c, bx, 0.0f, bx + bw, 0.0f);
    canvas_add_fill_color_stop(c, 0.00f, 0.99f, 0.80f, 0.26f, 1.0f);
    canvas_add_fill_color_stop(c, 0.50f, 0.95f, 0.35f, 0.45f, 1.0f);
    canvas_add_fill_color_stop(c, 1.00f, 0.35f, 0.55f, 0.98f, 1.0f);
    canvas_begin_path(c);
    canvas_round_rect_radii(c, bx, by, bw, bh, 200.0f, 200.0f, 200.0f, 200.0f,
                            200.0f, 200.0f, 200.0f, 200.0f);
    canvas_fill(c);

    save(c, "gallery/roundrect.png");
}

// strokeRect: outline rectangles without disturbing the current path.  A 3x2 grid
// shows the three joins on a thick outline, a dashed rect, a rotated-CTM quad with
// a gradient stroke, and the degenerate zero-extent rect (which strokes a line).
static void strokerect(void) {
    float const margin = 12.0f, cw = 148.0f, ch = 104.0f;
    canvas *__single c = canvas_create(468, 232);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 468.0f, 232.0f);

    char const *const labels[6] = { "miter join", "round join", "bevel join",
                                    "dashed", "rotated quad", "degenerate line" };
    for (int idx = 0; idx < 6; idx++) {
        int col = idx % 3, row = idx / 3;
        float ox = margin + (float)col * cw, oy = margin + (float)row * ch;
        float rx = ox + 26.0f, ry = oy + 20.0f, rw = 96.0f, rh = 46.0f;

        // Reset all line styles to defaults at the top of every cell.
        float const solid[1] = { 1.0f };
        canvas_set_line_dash(c, solid, 0);
        canvas_set_line_join(c, CANVAS_JOIN_MITER);
        canvas_set_line_cap(c, CANVAS_CAP_BUTT);

        if (idx == 0 || idx == 1 || idx == 2) {
            canvas_line_join const j[3] = { CANVAS_JOIN_MITER, CANVAS_JOIN_ROUND,
                                            CANVAS_JOIN_BEVEL };
            float const col3[3][3] = { { 0.96f, 0.56f, 0.26f },
                                       { 0.40f, 0.82f, 0.50f },
                                       { 0.36f, 0.62f, 0.95f } };
            canvas_set_line_join(c, j[idx]);
            canvas_set_line_width(c, 14.0f);
            canvas_set_stroke_rgba(c, col3[idx][0], col3[idx][1], col3[idx][2], 1.0f);
            canvas_stroke_rect(c, rx, ry, rw, rh);
        } else if (idx == 3) {
            float const dash[2] = { 11.0f, 7.0f };
            canvas_set_line_dash(c, dash, 2);
            canvas_set_line_width(c, 4.0f);
            canvas_set_line_cap(c, CANVAS_CAP_ROUND);
            canvas_set_stroke_rgba(c, 0.95f, 0.82f, 0.30f, 1.0f);
            canvas_stroke_rect(c, rx, ry, rw, rh);
        } else if (idx == 4) {
            // Rotated CTM strokes a rotated quad (corners go through the transform).
            canvas_save(c);
            canvas_translate(c, ox + 74.0f, oy + 42.0f);
            canvas_rotate(c, 0.32f);
            canvas_set_stroke_linear_gradient(c, -44.0f, 0.0f, 44.0f, 0.0f);
            canvas_add_stroke_color_stop(c, 0.0f, 0.30f, 0.90f, 0.95f, 1.0f);
            canvas_add_stroke_color_stop(c, 1.0f, 0.95f, 0.35f, 0.85f, 1.0f);
            canvas_set_line_width(c, 8.0f);
            canvas_set_line_join(c, CANVAS_JOIN_ROUND);
            canvas_stroke_rect(c, -44.0f, -22.0f, 88.0f, 44.0f);
            canvas_restore(c);
        } else {
            // h == 0 and w == 0 degenerate to round-capped lines: a crisp plus.
            float mx = ox + 74.0f, my = oy + 42.0f;
            canvas_set_line_width(c, 10.0f);
            canvas_set_line_cap(c, CANVAS_CAP_ROUND);
            canvas_set_stroke_rgba(c, 0.95f, 0.45f, 0.55f, 1.0f);
            canvas_stroke_rect(c, mx - 44.0f, my, 88.0f, 0.0f);
            canvas_stroke_rect(c, mx, my - 24.0f, 0.0f, 48.0f);
        }

        canvas_set_fill_rgba(c, 0.78f, 0.82f, 0.90f, 1.0f);
        canvas_set_font_size(c, 14.0f);
        canvas_fill_text(c, labels[idx], ox + 26.0f, oy + 92.0f);
    }

    save(c, "gallery/strokerect.png");
}

// createConicGradient: colours sweep clockwise around a centre.  A smooth rainbow
// wheel, a hard-stop "pie" (coincident stop offsets make crisp sector edges), and
// a conic-gradient *stroke* ring around a two-tone conic medallion.
static void conic(void) {
    canvas *__single c = canvas_create(440, 176);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 440.0f, 176.0f);

    float const cy = 78.0f, r = 56.0f;
    float const cx[3] = { 78.0f, 220.0f, 362.0f };
    char const *const labels[3] = { "rainbow wheel", "hard-stop sectors",
                                    "conic stroke" };
    int const ns = 13;

    // A: smooth rainbow wheel from the cosine palette (wraps red -> red).
    canvas_set_fill_conic_gradient(c, -TAU * 0.25f, cx[0], cy);
    for (int k = 0; k < ns; k++) {
        float t = (float)k / (float)(ns - 1);
        canvas_add_fill_color_stop(c, t, 0.5f + 0.5f * cosf(TAU * t),
                                   0.5f + 0.5f * cosf(TAU * (t + 0.33f)),
                                   0.5f + 0.5f * cosf(TAU * (t + 0.66f)), 1.0f);
    }
    canvas_begin_path(c);
    canvas_arc(c, cx[0], cy, r, 0.0f, TAU, false);
    canvas_fill(c);

    // B: five solid sectors via paired (coincident) stops -> hard edges.
    float const bnd[6] = { 0.00f, 0.18f, 0.40f, 0.55f, 0.78f, 1.00f };
    float const sect[5][3] = { { 0.97f, 0.78f, 0.24f }, { 0.20f, 0.78f, 0.70f },
                               { 0.95f, 0.45f, 0.40f }, { 0.42f, 0.40f, 0.92f },
                               { 0.45f, 0.82f, 0.45f } };
    canvas_set_fill_conic_gradient(c, -TAU * 0.25f, cx[1], cy);
    for (int s = 0; s < 5; s++) {
        canvas_add_fill_color_stop(c, bnd[s], sect[s][0], sect[s][1], sect[s][2],
                                   1.0f);
        canvas_add_fill_color_stop(c, bnd[s + 1], sect[s][0], sect[s][1],
                                   sect[s][2], 1.0f);
    }
    canvas_begin_path(c);
    canvas_arc(c, cx[1], cy, r, 0.0f, TAU, false);
    canvas_fill(c);

    // C: a two-tone conic medallion, ringed by a conic-gradient stroke.
    canvas_set_fill_conic_gradient(c, TAU * 0.1f, cx[2], cy);
    canvas_add_fill_color_stop(c, 0.00f, 0.82f, 0.84f, 0.90f, 1.0f);
    canvas_add_fill_color_stop(c, 0.25f, 0.30f, 0.33f, 0.42f, 1.0f);
    canvas_add_fill_color_stop(c, 0.50f, 0.82f, 0.84f, 0.90f, 1.0f);
    canvas_add_fill_color_stop(c, 0.75f, 0.30f, 0.33f, 0.42f, 1.0f);
    canvas_add_fill_color_stop(c, 1.00f, 0.82f, 0.84f, 0.90f, 1.0f);
    canvas_begin_path(c);
    canvas_arc(c, cx[2], cy, 24.0f, 0.0f, TAU, false);
    canvas_fill(c);

    canvas_set_stroke_conic_gradient(c, 0.0f, cx[2], cy);
    for (int k = 0; k < ns; k++) {
        float t = (float)k / (float)(ns - 1);
        canvas_add_stroke_color_stop(c, t, 0.5f + 0.5f * cosf(TAU * t),
                                     0.5f + 0.5f * cosf(TAU * (t + 0.33f)),
                                     0.5f + 0.5f * cosf(TAU * (t + 0.66f)), 1.0f);
    }
    canvas_set_line_width(c, 15.0f);
    canvas_begin_path(c);
    canvas_arc(c, cx[2], cy, 46.0f, 0.0f, TAU, false);
    canvas_close_path(c);
    canvas_stroke(c);

    canvas_set_fill_rgba(c, 0.80f, 0.83f, 0.90f, 1.0f);
    canvas_set_font_size(c, 14.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_CENTER);
    for (int i = 0; i < 3; i++) {
        canvas_fill_text(c, labels[i], cx[i], 166.0f);
    }
    canvas_set_text_align(c, CANVAS_ALIGN_START);

    save(c, "gallery/conic.png");
}

// A 32x32 seamless tile: an indigo ground, a gold gem at the centre, and coral
// quarter-dots in the corners that join into full dots across tile seams.
static void make_tile(uint8_t *__counted_by(32 * 32 * 4) t) {
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            int i = (y * 32 + x) * 4;
            float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
            float r = 0.16f, g = 0.18f, b = 0.36f;  // indigo ground
            float cdx = fx - 16.0f, cdy = fy - 16.0f;
            if (cdx * cdx + cdy * cdy < 10.5f * 10.5f) {  // centre gem
                r = 0.97f; g = 0.80f; b = 0.28f;
            }
            float gx = fx < 16.0f ? fx : fx - 32.0f;  // distance to nearest corner
            float gy = fy < 16.0f ? fy : fy - 32.0f;
            if (gx * gx + gy * gy < 7.0f * 7.0f) {  // corner dots (wrap into circles)
                r = 0.95f; g = 0.42f; b = 0.42f;
            }
            t[i]     = (uint8_t)(r * 255.0f + 0.5f);
            t[i + 1] = (uint8_t)(g * 255.0f + 0.5f);
            t[i + 2] = (uint8_t)(b * 255.0f + 0.5f);
            t[i + 3] = 255;
        }
    }
}

// createPattern: the same tile under each repeat mode (the un-tiled axes leave the
// dark ground showing), then the pattern used as a fill paint for a headline.
static void pattern(void) {
    canvas *__single c = canvas_create(474, 212);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 474.0f, 212.0f);

    uint8_t tile[32 * 32 * 4];
    make_tile(tile);

    canvas_pattern_repeat const modes[4] = { CANVAS_REPEAT, CANVAS_REPEAT_X,
                                             CANVAS_REPEAT_Y, CANVAS_NO_REPEAT };
    char const *const labels[4] = { "repeat", "repeat-x", "repeat-y",
                                    "no-repeat" };
    for (int i = 0; i < 4; i++) {
        float ox = 18.0f + (float)i * 114.0f, oy = 22.0f;
        // Anchor the tile grid to the panel corner via the CTM, then fill.
        canvas_save(c);
        canvas_translate(c, ox, oy);
        canvas_set_fill_pattern(c, tile, 32, 32, modes[i]);
        canvas_fill_rect(c, 0.0f, 0.0f, 96.0f, 96.0f);
        canvas_restore(c);

        canvas_set_stroke_rgba(c, 0.42f, 0.46f, 0.55f, 1.0f);
        canvas_set_line_width(c, 1.5f);
        canvas_stroke_rect(c, ox, oy, 96.0f, 96.0f);

        canvas_set_fill_rgba(c, 0.80f, 0.83f, 0.90f, 1.0f);
        canvas_set_font_size(c, 13.0f);
        canvas_set_text_align(c, CANVAS_ALIGN_CENTER);
        canvas_fill_text(c, labels[i], ox + 48.0f, 134.0f);
    }

    // The pattern is a fill paint like any other, so glyph coverage samples it too.
    canvas_set_fill_pattern(c, tile, 32, 32, CANVAS_REPEAT);
    canvas_set_font_size(c, 52.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_CENTER);
    canvas_fill_text(c, "canvas2d", 237.0f, 196.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_START);

    save(c, "gallery/pattern.png");
}

// imageSmoothingEnabled: the same 16x16 pixel-art source upscaled 8.75x with
// smoothing off (crisp nearest-neighbour blocks) and on (bilinear blend).
static void smoothing(void) {
    canvas *__single c = canvas_create(440, 210);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 440.0f, 210.0f);

    static char const art[16][17] = {
        "................", "...DDD....DDD...", "..DRRRD..DRRRD..",
        ".DRRHRRDDRRRRRD.", ".DRHHRRRRRRRRRD.", ".DRHRRRRRRRRRRD.",
        ".DRRRRRRRRRRRRD.", "..DRRRRRRRRRRD..", "...DRRRRRRRRD...",
        "....DRRRRRRD....", ".....DRRRRD.....", "......DRRD......",
        ".......DD.......", "................", "................",
        "................",
    };
    uint8_t src[16 * 16 * 4];
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            int i = (y * 16 + x) * 4;
            float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
            switch (art[y][x]) {
                case 'D': r = 0.60f; g = 0.08f; b = 0.16f; a = 1.0f; break;
                case 'R': r = 0.92f; g = 0.22f; b = 0.30f; a = 1.0f; break;
                case 'H': r = 0.99f; g = 0.66f; b = 0.69f; a = 1.0f; break;
                default: break;  // '.' stays transparent
            }
            src[i]     = (uint8_t)(r * 255.0f + 0.5f);
            src[i + 1] = (uint8_t)(g * 255.0f + 0.5f);
            src[i + 2] = (uint8_t)(b * 255.0f + 0.5f);
            src[i + 3] = (uint8_t)(a * 255.0f + 0.5f);
        }
    }

    // Source at 4x (nearest) so the 16x16 grid is legible.
    canvas_set_image_smoothing_enabled(c, false);
    canvas_draw_image_scaled(c, src, 16, 16, 24.0f, 62.0f, 64.0f, 64.0f);
    // Big nearest-neighbour upscale (blocky).
    canvas_draw_image_scaled(c, src, 16, 16, 120.0f, 24.0f, 140.0f, 140.0f);
    // Big bilinear upscale (smooth).
    canvas_set_image_smoothing_enabled(c, true);
    canvas_draw_image_scaled(c, src, 16, 16, 276.0f, 24.0f, 140.0f, 140.0f);

    canvas_set_fill_rgba(c, 0.80f, 0.83f, 0.90f, 1.0f);
    canvas_set_font_size(c, 14.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_CENTER);
    canvas_fill_text(c, "16x16 source", 56.0f, 186.0f);
    canvas_fill_text(c, "nearest", 190.0f, 186.0f);
    canvas_fill_text(c, "smooth (bilinear)", 346.0f, 186.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_START);

    save(c, "gallery/smoothing.png");
}

// textAlign / textBaseline.  Top: three words placed at one vertical anchor (each
// word names its own alignment).  Bottom: "Hg" set six ways against one horizontal
// baseline guide, so each mode's vertical shift is visible.
static void textgrid(void) {
    canvas *__single c = canvas_create(520, 256);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 520.0f, 256.0f);

    // --- textAlign: a vertical anchor, three self-naming words ---
    canvas_set_text_baseline(c, CANVAS_BASELINE_ALPHABETIC);
    canvas_set_text_align(c, CANVAS_ALIGN_START);
    canvas_set_fill_rgba(c, 0.62f, 0.66f, 0.74f, 1.0f);
    canvas_set_font_size(c, 13.0f);
    canvas_fill_text(c, "textAlign", 24.0f, 34.0f);

    canvas_set_stroke_rgba(c, 0.85f, 0.40f, 0.85f, 0.75f);
    canvas_set_line_width(c, 1.5f);
    canvas_begin_path(c);
    canvas_move_to(c, 270.0f, 44.0f);
    canvas_line_to(c, 270.0f, 124.0f);
    canvas_stroke(c);

    canvas_text_align const aligns[3] = { CANVAS_ALIGN_LEFT, CANVAS_ALIGN_CENTER,
                                          CANVAS_ALIGN_RIGHT };
    char const *const atext[3] = { "left", "center", "right" };
    float const ay[3] = { 64.0f, 92.0f, 120.0f };
    for (int i = 0; i < 3; i++) {
        canvas_set_text_align(c, aligns[i]);
        canvas_set_font_size(c, 24.0f);
        canvas_set_fill_rgba(c, 0.93f, 0.94f, 0.98f, 1.0f);
        canvas_fill_text(c, atext[i], 270.0f, ay[i]);
        canvas_set_fill_rgba(c, 0.40f, 0.85f, 0.95f, 1.0f);
        canvas_begin_path(c);
        canvas_arc(c, 270.0f, ay[i], 3.0f, 0.0f, TAU, false);
        canvas_fill(c);
    }

    // --- textBaseline: one horizontal baseline, "Hg" set six ways ---
    canvas_set_text_align(c, CANVAS_ALIGN_START);
    canvas_set_fill_rgba(c, 0.62f, 0.66f, 0.74f, 1.0f);
    canvas_set_font_size(c, 13.0f);
    canvas_fill_text(c, "textBaseline", 24.0f, 154.0f);

    canvas_set_stroke_rgba(c, 0.40f, 0.85f, 0.95f, 0.75f);
    canvas_set_line_width(c, 1.5f);
    canvas_begin_path(c);
    canvas_move_to(c, 24.0f, 200.0f);
    canvas_line_to(c, 496.0f, 200.0f);
    canvas_stroke(c);

    canvas_text_baseline const bl[6] = {
        CANVAS_BASELINE_TOP, CANVAS_BASELINE_HANGING, CANVAS_BASELINE_MIDDLE,
        CANVAS_BASELINE_ALPHABETIC, CANVAS_BASELINE_IDEOGRAPHIC,
        CANVAS_BASELINE_BOTTOM,
    };
    char const *const bname[6] = { "top",        "hang", "middle",
                                   "alphabetic", "ideo", "bottom" };
    for (int i = 0; i < 6; i++) {
        float cx = 30.0f + ((float)i + 0.5f) * ((490.0f - 30.0f) / 6.0f);
        canvas_set_text_align(c, CANVAS_ALIGN_CENTER);
        canvas_set_text_baseline(c, bl[i]);
        canvas_set_font_size(c, 28.0f);
        canvas_set_fill_rgba(c, 0.93f, 0.94f, 0.98f, 1.0f);
        canvas_fill_text(c, "Hg", cx, 200.0f);
        canvas_set_text_baseline(c, CANVAS_BASELINE_ALPHABETIC);
        canvas_set_font_size(c, 11.0f);
        canvas_set_fill_rgba(c, 0.62f, 0.66f, 0.74f, 1.0f);
        canvas_fill_text(c, bname[i], cx, 246.0f);
    }
    canvas_set_text_align(c, CANVAS_ALIGN_START);
    canvas_set_text_baseline(c, CANVAS_BASELINE_ALPHABETIC);

    save(c, "gallery/textgrid.png");
}

// measureText: draw a word at the alphabetic origin, then overlay its TextMetrics
// -- the tight actual bounding box, the looser font bounding box, the advance
// width, the hanging/alphabetic/ideographic baselines, and the origin point.
static void textmetrics(void) {
    canvas *__single c = canvas_create(560, 250);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 560.0f, 250.0f);

    char const *const word = "Graphics";
    float const x0 = 140.0f, y0 = 150.0f;
    canvas_set_text_align(c, CANVAS_ALIGN_START);
    canvas_set_text_baseline(c, CANVAS_BASELINE_ALPHABETIC);
    canvas_set_font_size(c, 62.0f);
    canvas_text_metrics m = canvas_measure_text_full(c, word);

    // Baselines first (behind everything): horizontal guides across the word.
    float lx0 = x0 - 16.0f, lx1 = x0 + m.width + 16.0f;
    float const solid[1] = { 1.0f }, dash[2] = { 5.0f, 4.0f };
    canvas_set_line_width(c, 1.4f);
    struct { float y; float r, g, b; char const *name; } base[3] = {
        { y0 - m.hanging_baseline,     0.95f, 0.85f, 0.35f, "hanging" },
        { y0,                          0.45f, 0.88f, 0.50f, "alphabetic" },
        { y0 - m.ideographic_baseline, 0.75f, 0.55f, 0.95f, "ideographic" },
    };
    for (int i = 0; i < 3; i++) {
        canvas_set_line_dash(c, i == 1 ? solid : dash, i == 1 ? 0 : 2);
        canvas_set_stroke_rgba(c, base[i].r, base[i].g, base[i].b, 0.95f);
        canvas_begin_path(c);
        canvas_move_to(c, lx0, base[i].y);
        canvas_line_to(c, lx1, base[i].y);
        canvas_stroke(c);
        canvas_set_fill_rgba(c, base[i].r, base[i].g, base[i].b, 1.0f);
        canvas_set_font_size(c, 11.0f);
        canvas_set_text_align(c, CANVAS_ALIGN_LEFT);
        canvas_fill_text(c, base[i].name, lx1 + 6.0f, base[i].y + 3.5f);
    }

    // The glyphs.
    canvas_set_line_dash(c, solid, 0);
    canvas_set_font_size(c, 62.0f);
    canvas_set_fill_rgba(c, 0.90f, 0.91f, 0.96f, 1.0f);
    canvas_fill_text(c, word, x0, y0);

    // Font bounding box (orange dashed) over the advance width.
    canvas_set_line_dash(c, dash, 2);
    canvas_set_line_width(c, 1.5f);
    canvas_set_stroke_rgba(c, 0.97f, 0.62f, 0.25f, 0.95f);
    canvas_stroke_rect(c, x0, y0 - m.font_bounding_box_ascent, m.width,
                       m.font_bounding_box_ascent + m.font_bounding_box_descent);
    canvas_set_fill_rgba(c, 0.97f, 0.62f, 0.25f, 1.0f);
    canvas_set_font_size(c, 11.0f);
    canvas_fill_text(c, "font box", x0 + 3.0f, y0 - m.font_bounding_box_ascent - 4.0f);

    // Actual (ink) bounding box (cyan solid).
    canvas_set_line_dash(c, solid, 0);
    canvas_set_line_width(c, 1.8f);
    canvas_set_stroke_rgba(c, 0.30f, 0.85f, 0.95f, 1.0f);
    float ab_l = x0 - m.actual_bounding_box_left, ab_t = y0 - m.actual_bounding_box_ascent;
    canvas_stroke_rect(c, ab_l, ab_t,
                       m.actual_bounding_box_left + m.actual_bounding_box_right,
                       m.actual_bounding_box_ascent + m.actual_bounding_box_descent);
    canvas_set_fill_rgba(c, 0.30f, 0.85f, 0.95f, 1.0f);
    canvas_fill_text(c, "actual box", ab_l + 2.0f, ab_t - 4.0f);

    // Advance-width measure below, with the measured value.
    float wy = y0 + m.font_bounding_box_descent + 26.0f;
    canvas_set_stroke_rgba(c, 0.88f, 0.90f, 0.96f, 1.0f);
    canvas_set_line_width(c, 1.4f);
    canvas_begin_path(c);
    canvas_move_to(c, x0, wy - 5.0f);
    canvas_line_to(c, x0, wy + 5.0f);
    canvas_move_to(c, x0, wy);
    canvas_line_to(c, x0 + m.width, wy);
    canvas_move_to(c, x0 + m.width, wy - 5.0f);
    canvas_line_to(c, x0 + m.width, wy + 5.0f);
    canvas_stroke(c);
    canvas_set_fill_rgba(c, 0.88f, 0.90f, 0.96f, 1.0f);
    canvas_set_font_size(c, 12.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_CENTER);
    canvas_fill_text(c, "advance width", x0 + m.width * 0.5f, wy + 18.0f);

    // Origin point.
    canvas_set_fill_rgba(c, 0.96f, 0.35f, 0.45f, 1.0f);
    canvas_begin_path(c);
    canvas_arc(c, x0, y0, 4.0f, 0.0f, TAU, false);
    canvas_fill(c);

    canvas_set_text_align(c, CANVAS_ALIGN_START);
    save(c, "gallery/textmetrics.png");
}

// maxWidth: the same phrase drawn unconstrained (it overflows the right marker)
// and with a maxWidth equal to the marked span (condensed horizontally to fit).
static void textmaxwidth(void) {
    canvas *__single c = canvas_create(520, 188);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 520.0f, 188.0f);

    char const *const phrase = "Condense me to fit the box!";
    float const L = 40.0f, maxw = 258.0f, R = L + maxw;
    float const gy0 = 18.0f, gy1 = 150.0f;

    // Shade the overflow zone (right of the marker) faint red.
    canvas_set_fill_rgba(c, 0.90f, 0.30f, 0.32f, 0.10f);
    canvas_fill_rect(c, R, gy0, 520.0f - R, gy1 - gy0);

    // The two maxWidth markers (dashed vertical guides).
    float const dash[2] = { 5.0f, 4.0f };
    canvas_set_line_dash(c, dash, 2);
    canvas_set_line_width(c, 1.4f);
    canvas_set_stroke_rgba(c, 0.55f, 0.59f, 0.68f, 0.85f);
    for (int k = 0; k < 2; k++) {
        float gx = k == 0 ? L : R;
        canvas_begin_path(c);
        canvas_move_to(c, gx, gy0);
        canvas_line_to(c, gx, gy1);
        canvas_stroke(c);
    }
    float const solid[1] = { 1.0f };
    canvas_set_line_dash(c, solid, 0);

    canvas_set_text_align(c, CANVAS_ALIGN_LEFT);
    canvas_set_text_baseline(c, CANVAS_BASELINE_ALPHABETIC);

    // Row A: no limit -> natural advance, spilling into the overflow zone.
    canvas_set_fill_rgba(c, 0.62f, 0.66f, 0.74f, 1.0f);
    canvas_set_font_size(c, 13.0f);
    canvas_fill_text(c, "without maxWidth", L, 40.0f);
    canvas_set_fill_rgba(c, 0.93f, 0.94f, 0.98f, 1.0f);
    canvas_set_font_size(c, 30.0f);
    canvas_fill_text(c, phrase, L, 74.0f);

    // Row B: maxWidth == the marked span -> condensed in x about the left anchor.
    canvas_set_fill_rgba(c, 0.62f, 0.66f, 0.74f, 1.0f);
    canvas_set_font_size(c, 13.0f);
    canvas_fill_text(c, "with maxWidth (condensed to fit)", L, 104.0f);
    canvas_set_fill_rgba(c, 0.45f, 0.88f, 0.55f, 1.0f);
    canvas_set_font_size(c, 30.0f);
    canvas_fill_text_max(c, phrase, L, 138.0f, maxw);

    // The maxWidth span bracket.
    canvas_set_stroke_rgba(c, 0.88f, 0.90f, 0.96f, 1.0f);
    canvas_set_line_width(c, 1.4f);
    canvas_begin_path(c);
    canvas_move_to(c, L, gy1 - 4.0f);
    canvas_line_to(c, L, gy1 + 4.0f);
    canvas_move_to(c, L, gy1);
    canvas_line_to(c, R, gy1);
    canvas_move_to(c, R, gy1 - 4.0f);
    canvas_line_to(c, R, gy1 + 4.0f);
    canvas_stroke(c);
    canvas_set_fill_rgba(c, 0.88f, 0.90f, 0.96f, 1.0f);
    canvas_set_font_size(c, 12.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_CENTER);
    canvas_fill_text(c, "maxWidth", (L + R) * 0.5f, gy1 + 20.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_START);

    save(c, "gallery/textmaxwidth.png");
}

// Hit testing: stipple a grid of sample points over a shape.  Left, isPointInPath
// on a pentagram under even-odd (the central pentagon reads as outside); right,
// isPointInStroke on a thick ring (only points within the stroke band hit).  All
// queries run first -- drawing the dots replaces the current path.
static void hittest(void) {
    canvas *__single c = canvas_create(480, 262);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 480.0f, 262.0f);

    int const step = 10, nx = 20, ny = 20;
    float const solid[1] = { 1.0f };

    // ---- isPointInPath: pentagram, even-odd ----
    star(c, 130.0f, 116.0f, 92.0f);
    canvas_set_line_dash(c, solid, 0);
    canvas_set_line_width(c, 1.3f);
    canvas_set_stroke_rgba(c, 0.55f, 0.58f, 0.66f, 0.55f);
    canvas_stroke(c);
    bool inA[20 * 20];
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            float px = 34.0f + (float)(i * step), py = 22.0f + (float)(j * step);
            inA[j * nx + i] = canvas_is_point_in_path(c, px, py, CANVAS_EVENODD);
        }
    }
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            float px = 34.0f + (float)(i * step), py = 22.0f + (float)(j * step);
            bool in = inA[j * nx + i];
            if (in) {
                canvas_set_fill_rgba(c, 0.30f, 0.85f, 0.70f, 1.0f);
            } else {
                canvas_set_fill_rgba(c, 0.42f, 0.45f, 0.52f, 0.85f);
            }
            canvas_begin_path(c);
            canvas_arc(c, px, py, in ? 2.7f : 1.5f, 0.0f, TAU, false);
            canvas_fill(c);
        }
    }

    // ---- isPointInStroke: a thick ring ----
    canvas_begin_path(c);
    canvas_arc(c, 350.0f, 116.0f, 66.0f, 0.0f, TAU, false);
    canvas_set_line_dash(c, solid, 0);
    canvas_set_line_width(c, 24.0f);
    bool inB[20 * 20];
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            float px = 254.0f + (float)(i * step), py = 22.0f + (float)(j * step);
            inB[j * nx + i] = canvas_is_point_in_stroke(c, px, py);
        }
    }
    canvas_set_stroke_rgba(c, 0.62f, 0.52f, 0.40f, 0.22f);  // faint stroke band
    canvas_stroke(c);
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            float px = 254.0f + (float)(i * step), py = 22.0f + (float)(j * step);
            bool in = inB[j * nx + i];
            if (in) {
                canvas_set_fill_rgba(c, 0.97f, 0.62f, 0.25f, 1.0f);
            } else {
                canvas_set_fill_rgba(c, 0.42f, 0.45f, 0.52f, 0.85f);
            }
            canvas_begin_path(c);
            canvas_arc(c, px, py, in ? 2.7f : 1.5f, 0.0f, TAU, false);
            canvas_fill(c);
        }
    }

    canvas_set_fill_rgba(c, 0.80f, 0.83f, 0.90f, 1.0f);
    canvas_set_font_size(c, 14.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_CENTER);
    canvas_fill_text(c, "isPointInPath", 130.0f, 248.0f);
    canvas_fill_text(c, "isPointInStroke", 350.0f, 248.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_START);

    save(c, "gallery/hittest.png");
}

// createImageData + putImageData dirty-rect: build one rainbow-ring image, stamp
// it whole on the left, and on the right write only a checkerboard of dirty
// sub-rects (the same image origin, so the tiles register into one picture).
static void dirtyrect(void) {
    canvas *__single c = canvas_create(518, 210);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 518.0f, 210.0f);

    int const W = 220, H = 150;
    int const Lx = 24, Ly = 24, Rx = 274, Ry = 24;

    int len = -1;
    uint8_t *img = canvas_create_image_data(c, W, H, &len);
    if (img) {
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int i = (y * W + x) * 4;
                float dx = (float)x - (float)W * 0.5f;
                float dy = (float)y - (float)H * 0.5f;
                float t = sqrtf(dx * dx + dy * dy) * (1.0f / 24.0f);  // ring period
                img[i]     = (uint8_t)(255.0f * (0.5f + 0.5f * cosf(TAU * t)) + 0.5f);
                img[i + 1] = (uint8_t)(255.0f * (0.5f + 0.5f * cosf(TAU * (t + 0.33f))) + 0.5f);
                img[i + 2] = (uint8_t)(255.0f * (0.5f + 0.5f * cosf(TAU * (t + 0.66f))) + 0.5f);
                img[i + 3] = 255;
            }
        }
        // Left: the whole image in one putImageData.
        canvas_put_image_data(c, img, len, W, H, Lx, Ly);
        // Right: only a checkerboard of dirty sub-rects is written.
        int const tile = 22;
        for (int j = 0; j * tile < H; j++) {
            for (int i = 0; i * tile < W; i++) {
                if (((i + j) & 1) == 0) {
                    canvas_put_image_data_dirty(c, img, len, W, H, Rx, Ry,
                                                i * tile, j * tile, tile, tile);
                }
            }
        }
        free(img);
    }

    canvas_set_stroke_rgba(c, 0.40f, 0.44f, 0.52f, 0.9f);
    canvas_set_line_width(c, 1.5f);
    canvas_stroke_rect(c, (float)Lx, (float)Ly, (float)W, (float)H);
    canvas_stroke_rect(c, (float)Rx, (float)Ry, (float)W, (float)H);

    canvas_set_fill_rgba(c, 0.80f, 0.83f, 0.90f, 1.0f);
    canvas_set_font_size(c, 14.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_CENTER);
    canvas_fill_text(c, "putImageData (full)", (float)Lx + (float)W * 0.5f, 196.0f);
    canvas_fill_text(c, "putImageData (dirty-rect)", (float)Rx + (float)W * 0.5f,
                     196.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_START);

    save(c, "gallery/dirtyrect.png");
}

// Path2D: a reusable path object, transformed at draw time.  Left, one petal path
// stamped under twelve rotations into a flower (the same object, different CTMs).
// Right, add_path composes a ring with its hole for an even-odd fill, and a star
// Path2D is stroked in the hole.
static void path2d_demo(void) {
    canvas *__single c = canvas_create(500, 240);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 500.0f, 240.0f);

    // One petal, built once around the origin, drawn under many transforms.
    canvas_path2d *__single petal = canvas_path2d_create();
    if (petal) {
        canvas_path2d_move_to(petal, 0.0f, 0.0f);
        canvas_path2d_bezier_curve_to(petal, 32.0f, -26.0f, 24.0f, -76.0f, 0.0f, -88.0f);
        canvas_path2d_bezier_curve_to(petal, -24.0f, -76.0f, -32.0f, -26.0f, 0.0f, 0.0f);
        canvas_path2d_close_path(petal);
        int const n = 12;
        for (int i = 0; i < n; i++) {
            float t = (float)i / (float)n;
            canvas_save(c);
            canvas_translate(c, 135.0f, 116.0f);
            canvas_rotate(c, t * TAU);
            canvas_set_fill_rgba(c, 0.5f + 0.5f * cosf(TAU * t),
                                 0.5f + 0.5f * cosf(TAU * (t + 0.33f)),
                                 0.5f + 0.5f * cosf(TAU * (t + 0.66f)), 0.85f);
            canvas_fill_path(c, petal, CANVAS_NONZERO);
            canvas_set_stroke_rgba(c, 1.0f, 1.0f, 1.0f, 0.5f);
            canvas_set_line_width(c, 1.2f);
            canvas_stroke_path(c, petal);
            canvas_restore(c);
        }
        canvas_path2d_destroy(petal);
    }

    // add_path: a ring and its hole composed into one path, filled even-odd.
    canvas_path2d *__single ring = canvas_path2d_create();
    canvas_path2d *__single hole = canvas_path2d_create();
    if (ring && hole) {
        canvas_path2d_arc(ring, 365.0f, 116.0f, 54.0f, 0.0f, TAU, false);
        canvas_path2d_arc(hole, 365.0f, 116.0f, 32.0f, 0.0f, TAU, false);
        canvas_path2d_add_path(ring, hole);
        canvas_set_fill_radial_gradient(c, 348.0f, 98.0f, 6.0f, 365.0f, 116.0f, 58.0f);
        canvas_add_fill_color_stop(c, 0.0f, 0.55f, 0.95f, 0.95f, 1.0f);
        canvas_add_fill_color_stop(c, 1.0f, 0.20f, 0.35f, 0.85f, 1.0f);
        canvas_fill_path(c, ring, CANVAS_EVENODD);
    }
    if (hole) {
        canvas_path2d_destroy(hole);
    }
    if (ring) {
        canvas_path2d_destroy(ring);
    }

    // A star Path2D, stroked in the hole.
    canvas_path2d *__single starp = canvas_path2d_create();
    if (starp) {
        for (int i = 0; i < 5; i++) {
            float a = -TAU * 0.25f + (float)i * (TAU * 0.4f);
            float px = 365.0f + 24.0f * cosf(a), py = 116.0f + 24.0f * sinf(a);
            if (i == 0) {
                canvas_path2d_move_to(starp, px, py);
            } else {
                canvas_path2d_line_to(starp, px, py);
            }
        }
        canvas_path2d_close_path(starp);
        canvas_set_stroke_rgba(c, 0.97f, 0.82f, 0.30f, 1.0f);
        canvas_set_line_width(c, 2.5f);
        canvas_set_line_join(c, CANVAS_JOIN_ROUND);
        canvas_stroke_path(c, starp);
        canvas_path2d_destroy(starp);
    }

    canvas_set_fill_rgba(c, 0.80f, 0.83f, 0.90f, 1.0f);
    canvas_set_font_size(c, 13.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_CENTER);
    canvas_fill_text(c, "one Path2D, 12 transforms", 135.0f, 226.0f);
    canvas_fill_text(c, "add_path · even-odd · stroke", 365.0f, 226.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_START);

    save(c, "gallery/path2d.png");
}

// Flood a box with rainbow stripes; only what falls inside the active clip
// survives, so the stripes trace out the clip shape.
static void clip_stripes(canvas *__single c, float x0, float y0, float x1, float y1) {
    int const n = 16;
    float bw = (x1 - x0) / (float)n;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)(n - 1);
        canvas_set_fill_rgba(c, 0.5f + 0.5f * cosf(TAU * t),
                             0.5f + 0.5f * cosf(TAU * (t + 0.33f)),
                             0.5f + 0.5f * cosf(TAU * (t + 0.66f)), 1.0f);
        canvas_fill_rect(c, x0 + (float)i * bw, y0, bw + 1.0f, y1 - y0);
    }
}

// Clipping: a circular window, the intersection of two discs, and a
// self-intersecting star window — each masking the same flood of stripes.
static void clipping(void) {
    canvas *__single c = canvas_create(300, 120);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 300.0f, 120.0f);

    canvas_save(c);
    canvas_begin_path(c);
    canvas_arc(c, 55.0f, 60.0f, 40.0f, 0.0f, TAU, false);
    canvas_clip(c);
    clip_stripes(c, 15.0f, 20.0f, 95.0f, 100.0f);
    canvas_restore(c);

    canvas_save(c);
    canvas_begin_path(c);
    canvas_arc(c, 135.0f, 60.0f, 38.0f, 0.0f, TAU, false);
    canvas_clip(c);
    canvas_begin_path(c);
    canvas_arc(c, 170.0f, 60.0f, 38.0f, 0.0f, TAU, false);
    canvas_clip(c);  // intersect: only the lens overlap survives
    clip_stripes(c, 95.0f, 20.0f, 210.0f, 100.0f);
    canvas_restore(c);

    canvas_save(c);
    star(c, 250.0f, 60.0f, 42.0f);
    canvas_clip(c);
    clip_stripes(c, 205.0f, 15.0f, 295.0f, 105.0f);
    canvas_restore(c);

    save(c, "gallery/clip.png");
}

// Gradients: a diagonal linear fill (outlined with a gradient stroke), an
// off-centre radial "sphere", and a multi-stop rainbow ramp.
static void gradients(void) {
    canvas *__single c = canvas_create(300, 120);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 300.0f, 120.0f);

    canvas_set_fill_linear_gradient(c, 20.0f, 20.0f, 100.0f, 100.0f);
    canvas_add_fill_color_stop(c, 0.0f, 0.99f, 0.80f, 0.26f, 1.0f);
    canvas_add_fill_color_stop(c, 1.0f, 0.90f, 0.22f, 0.42f, 1.0f);
    canvas_begin_path(c);
    canvas_round_rect(c, 20.0f, 20.0f, 80.0f, 80.0f, 16.0f);
    canvas_fill(c);
    // Outline it with a contrasting gradient stroke (cyan -> yellow, diagonal).
    canvas_set_stroke_linear_gradient(c, 20.0f, 20.0f, 100.0f, 100.0f);
    canvas_add_stroke_color_stop(c, 0.0f, 0.20f, 0.90f, 0.95f, 1.0f);
    canvas_add_stroke_color_stop(c, 1.0f, 0.95f, 0.95f, 0.20f, 1.0f);
    canvas_set_line_width(c, 5.0f);
    canvas_begin_path(c);
    canvas_round_rect(c, 20.0f, 20.0f, 80.0f, 80.0f, 16.0f);
    canvas_stroke(c);

    canvas_set_fill_radial_gradient(c, 140.0f, 46.0f, 3.0f, 150.0f, 60.0f, 44.0f);
    canvas_add_fill_color_stop(c, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_add_fill_color_stop(c, 0.4f, 0.30f, 0.65f, 0.95f, 1.0f);
    canvas_add_fill_color_stop(c, 1.0f, 0.05f, 0.10f, 0.35f, 1.0f);
    canvas_begin_path(c);
    canvas_arc(c, 150.0f, 60.0f, 44.0f, 0.0f, TAU, false);
    canvas_fill(c);

    canvas_set_fill_linear_gradient(c, 205.0f, 0.0f, 285.0f, 0.0f);
    canvas_add_fill_color_stop(c, 0.00f, 0.90f, 0.20f, 0.25f, 1.0f);
    canvas_add_fill_color_stop(c, 0.33f, 0.95f, 0.80f, 0.25f, 1.0f);
    canvas_add_fill_color_stop(c, 0.66f, 0.30f, 0.80f, 0.45f, 1.0f);
    canvas_add_fill_color_stop(c, 1.00f, 0.30f, 0.45f, 0.95f, 1.0f);
    canvas_begin_path(c);
    canvas_round_rect(c, 205.0f, 25.0f, 75.0f, 70.0f, 12.0f);
    canvas_fill(c);

    save(c, "gallery/gradients.png");
}

// Batching: a dense field of translucent discs, each its own canvas_fill, batched
// into one GPU command buffer flushed at write_png.
static uint32_t batch_rng = 0x1234567u;

static float batch_rand(void) {
    batch_rng = batch_rng * 1664525u + 1013904223u;
    return (float)(batch_rng >> 8) / 16777216.0f;  // [0,1)
}

static void batching(void) {
    canvas *__single c = canvas_create(300, 120);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.07f, 0.08f, 0.10f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 300.0f, 120.0f);

    canvas_set_global_alpha(c, 0.55f);
    for (int i = 0; i < 320; i++) {
        float x = batch_rand() * 300.0f;
        float y = batch_rand() * 120.0f;
        float r = 3.0f + batch_rand() * 9.0f;
        float t = batch_rand();
        canvas_set_fill_rgba(c, 0.5f + 0.5f * cosf(TAU * t),
                             0.5f + 0.5f * cosf(TAU * (t + 0.33f)),
                             0.5f + 0.5f * cosf(TAU * (t + 0.66f)), 1.0f);
        canvas_begin_path(c);
        canvas_arc(c, x, y, r, 0.0f, TAU, false);
        canvas_fill(c);
    }

    save(c, "gallery/batch.png");
}

// drawImage: a small procedural source, drawn 1:1 (crisp), scaled up (bilinear
// smoothing), and scaled + rotated through the transform.
static void drawimage(void) {
    canvas *__single c = canvas_create(300, 120);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 300.0f, 120.0f);

    // A 16x16 multi-hue source image (tightly packed RGBA8).
    uint8_t img[16 * 16 * 4];
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            int i = (y * 16 + x) * 4;
            img[i] = (uint8_t)(255.0f * (0.5f + 0.5f * cosf(TAU * (float)x / 16.0f)));
            img[i + 1] = (uint8_t)(255.0f * (0.5f + 0.5f * cosf(TAU * (float)y / 16.0f)));
            img[i + 2] = (uint8_t)(255.0f * (0.5f + 0.5f * cosf(TAU * (float)(x + y) / 16.0f)));
            img[i + 3] = 255;
        }
    }

    canvas_draw_image(c, img, 16, 16, 20.0f, 20.0f);                       // 1:1
    canvas_draw_image_scaled(c, img, 16, 16, 50.0f, 20.0f, 80.0f, 80.0f);  // bilinear
    canvas_save(c);
    canvas_translate(c, 235.0f, 60.0f);
    canvas_rotate(c, 0.5f);
    canvas_draw_image_scaled(c, img, 16, 16, -40.0f, -40.0f, 80.0f, 80.0f);  // rotated
    canvas_restore(c);

    save(c, "gallery/drawimage.png");
}

// Text: Libian TC glyph outlines rasterized by the same coverage fill as the
// shapes, so they take gradients, strokes, transforms, and alpha; one fill_text
// mixes Latin and Chinese.
static void text(void) {
    canvas *__single c = canvas_create(420, 170);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 420.0f, 170.0f);

    // Gradient-filled headline: Latin + Chinese in a single fill_text.
    canvas_set_fill_linear_gradient(c, 20.0f, 0.0f, 390.0f, 0.0f);
    canvas_add_fill_color_stop(c, 0.00f, 0.99f, 0.80f, 0.26f, 1.0f);
    canvas_add_fill_color_stop(c, 0.50f, 0.95f, 0.35f, 0.45f, 1.0f);
    canvas_add_fill_color_stop(c, 1.00f, 0.35f, 0.55f, 0.98f, 1.0f);
    canvas_set_font_size(c, 52.0f);
    canvas_fill_text(c, "canvas2d 畫布", 20.0f, 70.0f);

    // Stroked subtitle, also mixing scripts.
    canvas_set_stroke_rgba(c, 0.85f, 0.88f, 0.95f, 1.0f);
    canvas_set_line_width(c, 1.2f);
    canvas_set_line_join(c, CANVAS_JOIN_ROUND);
    canvas_set_font_size(c, 26.0f);
    canvas_stroke_text(c, "隸書 · Libian TC, in C", 22.0f, 116.0f);

    // Transformed (rotated) text shows the CTM applies to glyphs too.
    canvas_save(c);
    canvas_translate(c, 352.0f, 152.0f);
    canvas_rotate(c, -0.32f);
    canvas_set_fill_rgba(c, 0.45f, 0.85f, 0.55f, 1.0f);
    canvas_set_font_size(c, 26.0f);
    canvas_fill_text(c, "你好!", 0.0f, 0.0f);
    canvas_restore(c);

    save(c, "gallery/text.png");
}

// globalCompositeOperation: two overlapping discs over a gradient, under a range of
// blend modes.
static void blend(void) {
    struct { canvas_composite_op op; char const *name; } cell[6] = {
        { CANVAS_OP_MULTIPLY, "multiply" },   { CANVAS_OP_SCREEN, "screen" },
        { CANVAS_OP_OVERLAY, "overlay" },     { CANVAS_OP_DIFFERENCE, "difference" },
        { CANVAS_OP_HUE, "hue" },             { CANVAS_OP_LUMINOSITY, "luminosity" },
    };
    int const cw = 120, n = 6;
    canvas *__single c = canvas_create(cw * n, 150);
    if (!c) {
        return;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.13f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, (float)(cw * n), 150.0f);

    for (int i = 0; i < n; i++) {
        float ox = (float)(i * cw);
        // Backdrop: a diagonal gradient block (always source-over).
        canvas_set_global_composite_operation(c, CANVAS_OP_SOURCE_OVER);
        canvas_set_fill_linear_gradient(c, ox + 12.0f, 18.0f, ox + 108.0f, 112.0f);
        canvas_add_fill_color_stop(c, 0.0f, 0.20f, 0.65f, 0.95f, 1.0f);
        canvas_add_fill_color_stop(c, 1.0f, 0.98f, 0.85f, 0.30f, 1.0f);
        canvas_begin_path(c);
        canvas_round_rect(c, ox + 12.0f, 18.0f, 96.0f, 94.0f, 12.0f);
        canvas_fill(c);

        // Two overlapping discs under this cell's blend mode.
        canvas_set_global_composite_operation(c, cell[i].op);
        canvas_set_fill_rgba(c, 0.92f, 0.26f, 0.21f, 1.0f);
        canvas_begin_path(c);
        canvas_arc(c, ox + 48.0f, 56.0f, 28.0f, 0.0f, TAU, false);
        canvas_fill(c);
        canvas_set_fill_rgba(c, 0.18f, 0.85f, 0.42f, 1.0f);
        canvas_begin_path(c);
        canvas_arc(c, ox + 72.0f, 78.0f, 28.0f, 0.0f, TAU, false);
        canvas_fill(c);

        // Label.
        canvas_set_global_composite_operation(c, CANVAS_OP_SOURCE_OVER);
        canvas_set_fill_rgba(c, 0.90f, 0.92f, 0.96f, 1.0f);
        canvas_set_font_size(c, 15.0f);
        canvas_fill_text(c, cell[i].name, ox + 12.0f, 134.0f);
    }

    save(c, "gallery/blend.png");
}

int main(void) {
    shapes();
    winding();
    dashes();
    imagedata();
    dirtyrect();
    joinscaps();
    paths();
    roundrect();
    strokerect();
    path2d_demo();
    clipping();
    gradients();
    conic();
    pattern();
    batching();
    drawimage();
    smoothing();
    text();
    textgrid();
    textmetrics();
    textmaxwidth();
    hittest();
    blend();
    return 0;
}
