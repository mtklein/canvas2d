// Renders the showcase PNGs in gallery/.  Built like any consumer (public API,
// -std=c23 -fbounds-safety -Weverything), then run by `ninja images`.

#include "canvas.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static float const TAU = 6.2831853f;

// Profiling knob.  GALLERY_REPS renders every scene that many times so `sample` has
// a few seconds of the real rendering pipeline to profile (the one-shot gallery
// finishes in ~0.2 s, too brief to sample).  PNG encoding would dominate and distort
// such a profile, so save() writes files only on the final rep; the default
// (GALLERY_REPS unset -> one rep, g_skip_save stays false) renders and writes exactly
// as before, so `ninja images` is unchanged.
static bool g_skip_save = false;

static int gallery_reps(void) {
    char const *__null_terminated env = getenv("GALLERY_REPS");
    int reps = env ? atoi(env) : 1;
    return reps < 1 ? 1 : reps;
}

static void save(canvas *__single cv, char const *__null_terminated path) {
    if (!g_skip_save && !canvas_write_png(cv, path)) {
        (void)fprintf(stderr, "gallery: write failed: %s\n", path);
    }
    canvas_destroy(cv);
}

// Begin recording a scene to its committed gallery/<scene>.canvas program,
// emitted alongside the PNG.  Called at the TOP of every scene, before any
// draws, so the file captures the whole scene; recording finalizes when the
// scene's canvas is destroyed in save().  Like the PNG, only the final rep
// writes (GALLERY_REPS profiling reruns skip it), so a bare `ninja images` run
// emits each scene's program exactly once.  The program is self-contained --
// font/glyph/bitmap/shape blocks for the text, image blocks for the
// drawImage/putImageData/pattern sources, path blocks for the Path2D draws --
// so it replays on a machine without the fonts: the determinism gate
// (tests/test_replay_gallery.c) replays each one and byte-compares to the PNG.
static void record_scene(canvas *__single cv, char const *__null_terminated path) {
    if (!g_skip_save && !canvas_record_to(cv, path)) {
        (void)fprintf(stderr, "gallery: record failed: %s\n", path);
    }
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
    record_scene(c, "gallery/shapes.canvas");
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
    record_scene(c, "gallery/winding.canvas");
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
    record_scene(c, "gallery/dashes.canvas");
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
    record_scene(c, "gallery/imagedata.canvas");
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
    record_scene(c, "gallery/joins.canvas");
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 280.0f, 160.0f);

    enum canvas_line_join const js[3] = { CANVAS_JOIN_MITER, CANVAS_JOIN_ROUND,
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

    enum canvas_line_cap const cs[3] = { CANVAS_CAP_BUTT, CANVAS_CAP_ROUND,
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
    record_scene(c, "gallery/paths.canvas");
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
    record_scene(c, "gallery/roundrect.canvas");
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
    record_scene(c, "gallery/strokerect.canvas");
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
            enum canvas_line_join const j[3] = { CANVAS_JOIN_MITER, CANVAS_JOIN_ROUND,
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
    record_scene(c, "gallery/conic.canvas");
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
    record_scene(c, "gallery/pattern.canvas");
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 474.0f, 212.0f);

    uint8_t tile[32 * 32 * 4];
    make_tile(tile);

    enum canvas_pattern_repeat const modes[4] = { CANVAS_REPEAT, CANVAS_REPEAT_X,
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
    record_scene(c, "gallery/smoothing.canvas");
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
    record_scene(c, "gallery/textgrid.canvas");
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

    enum canvas_text_align const aligns[3] = { CANVAS_ALIGN_LEFT, CANVAS_ALIGN_CENTER,
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

    enum canvas_text_baseline const bl[6] = {
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
    record_scene(c, "gallery/textmetrics.canvas");
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
    record_scene(c, "gallery/textmaxwidth.canvas");
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
    record_scene(c, "gallery/hittest.canvas");
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
    record_scene(c, "gallery/dirtyrect.canvas");
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
    record_scene(c, "gallery/path2d.canvas");
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

// Paint a transparency checkerboard *behind* whatever is already in [x,y,w,h]
// (destination-over), so a Porter-Duff result's transparent areas read clearly.
static void checker_behind(canvas *__single c, float x, float y, float w, float h) {
    canvas_set_global_composite_operation(c, CANVAS_OP_DESTINATION_OVER);
    int const t = 13;
    for (int j = 0; (float)(j * t) < h; j++) {
        for (int i = 0; (float)(i * t) < w; i++) {
            if (((i + j) & 1) == 0) {
                canvas_set_fill_rgba(c, 0.84f, 0.85f, 0.88f, 1.0f);
            } else {
                canvas_set_fill_rgba(c, 0.68f, 0.70f, 0.75f, 1.0f);
            }
            canvas_fill_rect(c, x + (float)(i * t), y + (float)(j * t), (float)t,
                             (float)t);
        }
    }
    canvas_set_global_composite_operation(c, CANVAS_OP_SOURCE_OVER);
}

static void pd_dst(canvas *__single c, float ox, float oy) {  // blue square
    canvas_set_fill_rgba(c, 0.20f, 0.55f, 0.90f, 1.0f);
    canvas_fill_rect(c, ox + 16.0f, oy + 12.0f, 50.0f, 50.0f);
}

static void pd_src(canvas *__single c, float ox, float oy) {  // orange disc
    canvas_set_fill_rgba(c, 0.97f, 0.55f, 0.18f, 1.0f);
    canvas_begin_path(c);
    canvas_arc(c, ox + 62.0f, oy + 52.0f, 27.0f, 0.0f, TAU, false);
    canvas_fill(c);
}

// The 11 Porter-Duff compositing operators (how source alpha combines with the
// destination), each in its own cell: clip + clear to transparent, draw the blue
// "destination" square, then the orange "source" disc under the operator, then a
// checkerboard behind so the surviving regions read.  A legend names the shapes.
static void porterduff(void) {
    float const M = 16.0f, cellW = 118.0f, cellH = 99.0f;
    canvas *__single c = canvas_create(504, 329);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/porterduff.canvas");
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 504.0f, 329.0f);

    enum canvas_composite_op const ops[11] = {
        CANVAS_OP_SOURCE_OVER,      CANVAS_OP_SOURCE_IN,
        CANVAS_OP_SOURCE_OUT,       CANVAS_OP_SOURCE_ATOP,
        CANVAS_OP_DESTINATION_OVER, CANVAS_OP_DESTINATION_IN,
        CANVAS_OP_DESTINATION_OUT,  CANVAS_OP_DESTINATION_ATOP,
        CANVAS_OP_LIGHTER,          CANVAS_OP_XOR,
        CANVAS_OP_COPY,
    };
    char const *const names[11] = {
        "source-over",      "source-in",       "source-out",
        "source-atop",      "destination-over", "destination-in",
        "destination-out",  "destination-atop", "lighter",
        "xor",              "copy",
    };

    for (int idx = 0; idx < 11; idx++) {
        int col = idx % 4, row = idx / 4;
        float ox = M + (float)col * cellW, oy = M + (float)row * cellH;
        canvas_save(c);
        canvas_begin_path(c);
        canvas_rect(c, ox + 3.0f, oy + 3.0f, 112.0f, 78.0f);
        canvas_clip(c);
        canvas_clear_rect(c, ox + 3.0f, oy + 3.0f, 112.0f, 78.0f);
        pd_dst(c, ox, oy);  // destination (source-over)
        canvas_set_global_composite_operation(c, ops[idx]);
        pd_src(c, ox, oy);  // source under the operator
        checker_behind(c, ox + 3.0f, oy + 3.0f, 112.0f, 78.0f);
        canvas_restore(c);

        canvas_set_fill_rgba(c, 0.80f, 0.83f, 0.90f, 1.0f);
        canvas_set_font_size(c, 11.0f);
        canvas_set_text_align(c, CANVAS_ALIGN_CENTER);
        canvas_fill_text(c, names[idx], ox + cellW * 0.5f, oy + 93.0f);
    }

    // Legend cell (bottom-right): the two shapes and what they are.
    float lox = M + 3.0f * cellW, loy = M + 2.0f * cellH;
    pd_dst(c, lox, loy - 4.0f);
    canvas_set_fill_rgba(c, 0.20f, 0.55f, 0.90f, 1.0f);
    canvas_fill_rect(c, lox + 20.0f, loy + 14.0f, 30.0f, 22.0f);
    canvas_set_fill_rgba(c, 0.85f, 0.88f, 0.93f, 1.0f);
    canvas_set_font_size(c, 12.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_LEFT);
    canvas_fill_text(c, "destination", lox + 56.0f, loy + 30.0f);
    canvas_set_fill_rgba(c, 0.97f, 0.55f, 0.18f, 1.0f);
    canvas_begin_path(c);
    canvas_arc(c, lox + 35.0f, loy + 58.0f, 13.0f, 0.0f, TAU, false);
    canvas_fill(c);
    canvas_set_fill_rgba(c, 0.85f, 0.88f, 0.93f, 1.0f);
    canvas_fill_text(c, "source", lox + 56.0f, loy + 62.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_START);

    save(c, "gallery/porterduff.png");
}

// drawImage subrect: build a sprite atlas on a scratch canvas, read it back to
// RGBA8, then pull individual tiles out of it with the source-rect overload --
// the atlas on the left, four tiles extracted and enlarged on the right.
static void subrect(void) {
    int const AW = 160, AH = 80;  // 4x2 tiles of 40px
    uint8_t atlas[160 * 80 * 4];
    canvas *__single ac = canvas_create(AW, AH);
    if (ac) {
        for (int k = 0; k < 8; k++) {
            int tx = k % 4, ty = k / 4;
            float ox = (float)(tx * 40), oy = (float)(ty * 40);
            float cx = ox + 20.0f, cy = oy + 20.0f;
            float t = (float)k / 8.0f;
            canvas_set_fill_rgba(ac, 0.5f + 0.5f * cosf(TAU * t),
                                 0.5f + 0.5f * cosf(TAU * (t + 0.33f)),
                                 0.5f + 0.5f * cosf(TAU * (t + 0.66f)), 1.0f);
            canvas_fill_rect(ac, ox, oy, 40.0f, 40.0f);
            canvas_set_fill_rgba(ac, 0.98f, 0.99f, 1.0f, 0.92f);
            switch (k % 4) {
                case 0:  // circle
                    canvas_begin_path(ac);
                    canvas_arc(ac, cx, cy, 14.0f, 0.0f, TAU, false);
                    canvas_fill(ac);
                    break;
                case 1:  // square
                    canvas_fill_rect(ac, cx - 13.0f, cy - 13.0f, 26.0f, 26.0f);
                    break;
                case 2:  // triangle
                    canvas_begin_path(ac);
                    canvas_move_to(ac, cx, cy - 15.0f);
                    canvas_line_to(ac, cx + 14.0f, cy + 12.0f);
                    canvas_line_to(ac, cx - 14.0f, cy + 12.0f);
                    canvas_close_path(ac);
                    canvas_fill(ac);
                    break;
                default:  // star
                    star(ac, cx, cy, 16.0f);
                    canvas_fill(ac);
                    break;
            }
        }
        canvas_read_rgba(ac, atlas, AW * AH * 4);
        canvas_destroy(ac);
    }

    canvas *__single c = canvas_create(468, 196);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/subrect.canvas");
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 468.0f, 196.0f);

    // Left: the whole atlas at 1.5x, with a grid showing the tile cells.
    canvas_draw_image_scaled(c, atlas, AW, AH, 20.0f, 30.0f, 240.0f, 120.0f);
    canvas_set_stroke_rgba(c, 0.20f, 0.22f, 0.28f, 0.9f);
    canvas_set_line_width(c, 1.0f);
    for (int i = 1; i < 4; i++) {
        canvas_begin_path(c);
        canvas_move_to(c, 20.0f + (float)i * 60.0f, 30.0f);
        canvas_line_to(c, 20.0f + (float)i * 60.0f, 150.0f);
        canvas_stroke(c);
    }
    canvas_begin_path(c);
    canvas_move_to(c, 20.0f, 90.0f);
    canvas_line_to(c, 260.0f, 90.0f);
    canvas_stroke(c);

    // Right: four chosen tiles pulled out via draw_image_subrect, enlarged 2x2.
    int const pick[4] = { 1, 6, 3, 4 };
    float const ts = 66.0f, gap = 8.0f, dx0 = 300.0f, dy0 = 30.0f;
    for (int m = 0; m < 4; m++) {
        int k = pick[m], tx = k % 4, ty = k / 4;
        float dx = dx0 + (float)(m % 2) * (ts + gap);
        float dy = dy0 + (float)(m / 2) * (ts + gap);
        canvas_draw_image_subrect(c, atlas, AW, AH, (float)(tx * 40),
                                  (float)(ty * 40), 40.0f, 40.0f, dx, dy, ts, ts);
    }

    canvas_set_fill_rgba(c, 0.80f, 0.83f, 0.90f, 1.0f);
    canvas_set_font_size(c, 14.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_CENTER);
    canvas_fill_text(c, "sprite atlas", 140.0f, 178.0f);
    canvas_fill_text(c, "pulled via subrect", 370.0f, 178.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_START);

    save(c, "gallery/subrect.png");
}

// One motif (a teal square with a gold "F") centred at the origin; the caller's
// transform deforms it.  "F" has no symmetry, so reflection and shear read at a
// glance.
static void affine_motif(canvas *__single c) {
    canvas_set_fill_rgba(c, 0.25f, 0.70f, 0.78f, 0.85f);
    canvas_fill_rect(c, -32.0f, -32.0f, 64.0f, 64.0f);
    canvas_set_stroke_rgba(c, 0.95f, 0.97f, 1.0f, 0.95f);
    canvas_set_line_width(c, 2.0f);
    canvas_stroke_rect(c, -32.0f, -32.0f, 64.0f, 64.0f);
    canvas_set_fill_rgba(c, 0.99f, 0.85f, 0.30f, 1.0f);
    canvas_set_font_size(c, 54.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_CENTER);
    canvas_set_text_baseline(c, CANVAS_BASELINE_MIDDLE);
    canvas_fill_text(c, "F", 0.0f, 0.0f);
}

// canvas_transform: arbitrary affine matrices beyond translate/rotate/scale.  Each
// cell applies one matrix to the motif (solid), with the identity footprint behind
// it (dashed), so the shear / scale / reflection is visible.
static void affine(void) {
    float const M = 14.0f, cellW = 150.0f, cellH = 122.0f;
    canvas *__single c = canvas_create(478, 272);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/affine.canvas");
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 478.0f, 272.0f);

    struct { float a, b, c, d; char const *name; } const mat[6] = {
        { 1.0f, 0.0f, 0.0f, 1.0f, "identity" },
        { 1.0f, 0.0f, 0.5f, 1.0f, "horizontal skew" },
        { 1.0f, 0.45f, 0.0f, 1.0f, "vertical skew" },
        { 1.3f, 0.0f, 0.0f, 0.65f, "anisotropic scale" },
        { -1.0f, 0.0f, 0.0f, 1.0f, "reflect (a = -1)" },
        { 1.0f, 0.3f, 0.42f, 1.0f, "shear x + y" },
    };
    float const dash[2] = { 5.0f, 4.0f }, solid[1] = { 1.0f };

    for (int i = 0; i < 6; i++) {
        int col = i % 3, row = i / 3;
        float ox = M + (float)col * cellW, oy = M + (float)row * cellH;
        canvas_save(c);
        canvas_translate(c, ox + 75.0f, oy + 52.0f);
        // Identity footprint (dashed), drawn before the matrix is applied.
        canvas_set_line_dash(c, dash, 2);
        canvas_set_line_width(c, 1.2f);
        canvas_set_stroke_rgba(c, 0.45f, 0.48f, 0.55f, 0.85f);
        canvas_stroke_rect(c, -32.0f, -32.0f, 64.0f, 64.0f);
        canvas_set_line_dash(c, solid, 0);
        // Apply the matrix, draw the motif deformed.
        canvas_transform(c, mat[i].a, mat[i].b, mat[i].c, mat[i].d, 0.0f, 0.0f);
        affine_motif(c);
        canvas_restore(c);

        canvas_set_text_baseline(c, CANVAS_BASELINE_ALPHABETIC);
        canvas_set_text_align(c, CANVAS_ALIGN_CENTER);
        canvas_set_fill_rgba(c, 0.80f, 0.83f, 0.90f, 1.0f);
        canvas_set_font_size(c, 12.0f);
        canvas_fill_text(c, mat[i].name, ox + 75.0f, oy + 108.0f);
    }
    canvas_set_text_align(c, CANVAS_ALIGN_START);

    save(c, "gallery/affine.png");
}

// miterLimit and lineDashOffset.  Top: one sharp V stroked at four miter limits --
// below the spike's ratio (~4.7) the join falls back to a bevel, above it the
// miter spike survives.  Bottom: one dash pattern at five offsets, the phase
// marching left (a frozen marching-ants animation).
static void miterdash(void) {
    canvas *__single c = canvas_create(480, 258);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/miterdash.canvas");
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 480.0f, 258.0f);

    canvas_set_text_align(c, CANVAS_ALIGN_LEFT);
    canvas_set_text_baseline(c, CANVAS_BASELINE_ALPHABETIC);
    canvas_set_fill_rgba(c, 0.62f, 0.66f, 0.74f, 1.0f);
    canvas_set_font_size(c, 13.0f);
    canvas_fill_text(c, "miterLimit", 16.0f, 28.0f);

    // Four identical sharp Vs (miter ratio ~4.7), at increasing miter limits.
    float const solid[1] = { 1.0f };
    float const limits[4] = { 2.0f, 4.0f, 6.0f, 10.0f };
    char const *const mlabel[4] = { "limit 2", "limit 4", "limit 6", "limit 10" };
    canvas_set_line_dash(c, solid, 0);
    canvas_set_line_join(c, CANVAS_JOIN_MITER);
    canvas_set_line_cap(c, CANVAS_CAP_BUTT);
    canvas_set_line_width(c, 14.0f);
    for (int i = 0; i < 4; i++) {
        float cx = 90.0f + (float)i * 100.0f;
        canvas_set_miter_limit(c, limits[i]);
        canvas_set_stroke_rgba(c, 0.95f, 0.55f, 0.30f, 1.0f);
        canvas_begin_path(c);
        canvas_move_to(c, cx - 16.0f, 44.0f);
        canvas_line_to(c, cx, 112.0f);
        canvas_line_to(c, cx + 16.0f, 44.0f);
        canvas_stroke(c);
        canvas_set_fill_rgba(c, 0.80f, 0.83f, 0.90f, 1.0f);
        canvas_set_font_size(c, 12.0f);
        canvas_set_text_align(c, CANVAS_ALIGN_CENTER);
        canvas_fill_text(c, mlabel[i], cx, 164.0f);
    }
    canvas_set_miter_limit(c, 10.0f);  // restore default

    canvas_set_text_align(c, CANVAS_ALIGN_LEFT);
    canvas_set_fill_rgba(c, 0.62f, 0.66f, 0.74f, 1.0f);
    canvas_set_font_size(c, 13.0f);
    canvas_fill_text(c, "lineDashOffset", 16.0f, 186.0f);

    // One dash pattern, five offsets: the phase marches across the rows.
    float const dash[2] = { 18.0f, 10.0f };
    float const offs[5] = { 0.0f, 6.0f, 12.0f, 18.0f, 24.0f };
    char const *const olabel[5] = { "0", "6", "12", "18", "24" };
    canvas_set_line_dash(c, dash, 2);
    canvas_set_line_width(c, 6.0f);
    canvas_set_line_cap(c, CANVAS_CAP_BUTT);
    for (int i = 0; i < 5; i++) {
        float y = 200.0f + (float)i * 12.0f;
        canvas_set_line_dash_offset(c, offs[i]);
        canvas_set_stroke_rgba(c, 0.40f, 0.82f, 0.95f, 1.0f);
        canvas_begin_path(c);
        canvas_move_to(c, 80.0f, y);
        canvas_line_to(c, 462.0f, y);
        canvas_stroke(c);
        canvas_set_fill_rgba(c, 0.62f, 0.66f, 0.74f, 1.0f);
        canvas_set_font_size(c, 11.0f);
        canvas_set_text_align(c, CANVAS_ALIGN_RIGHT);
        canvas_fill_text(c, olabel[i], 68.0f, y + 4.0f);
    }
    canvas_set_line_dash(c, solid, 0);
    canvas_set_line_dash_offset(c, 0.0f);
    canvas_set_text_align(c, CANVAS_ALIGN_START);

    save(c, "gallery/miterdash.png");
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
    record_scene(c, "gallery/clip.canvas");
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
    record_scene(c, "gallery/gradients.canvas");
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

// A dense field of translucent discs, each its own canvas_fill, composited in
// order onto the shared target (the alpha overlap shows ordering is preserved).
static uint32_t batch_rng = 0x1234567u;

static float batch_rand(void) {
    batch_rng = batch_rng * 1664525u + 1013904223u;
    return (float)(batch_rng >> 8) / 16777216.0f;  // [0,1)
}

static void batching(void) {
    batch_rng = 0x1234567u;  // reseed so the scene is reproducible regardless of how
                             // many times it runs (GALLERY_REPS calls it repeatedly)
    canvas *__single c = canvas_create(300, 120);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/batch.canvas");
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
    record_scene(c, "gallery/drawimage.canvas");
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
    record_scene(c, "gallery/text.canvas");
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

// globalCompositeOperation: all fifteen blend modes -- the eleven separable plus
// the four non-separable -- each compositing two overlapping discs over the same
// diagonal gradient backdrop (the W3C composite+blend formula, the checked-C blend
// kernel).
static void blend(void) {
    struct { enum canvas_composite_op op; char const *name; } const cell[15] = {
        { CANVAS_OP_MULTIPLY, "multiply" },       { CANVAS_OP_SCREEN, "screen" },
        { CANVAS_OP_OVERLAY, "overlay" },         { CANVAS_OP_DARKEN, "darken" },
        { CANVAS_OP_LIGHTEN, "lighten" },         { CANVAS_OP_COLOR_DODGE, "color-dodge" },
        { CANVAS_OP_COLOR_BURN, "color-burn" },   { CANVAS_OP_HARD_LIGHT, "hard-light" },
        { CANVAS_OP_SOFT_LIGHT, "soft-light" },   { CANVAS_OP_DIFFERENCE, "difference" },
        { CANVAS_OP_EXCLUSION, "exclusion" },     { CANVAS_OP_HUE, "hue" },
        { CANVAS_OP_SATURATION, "saturation" },   { CANVAS_OP_COLOR, "color" },
        { CANVAS_OP_LUMINOSITY, "luminosity" },
    };
    float const M = 12.0f, cellW = 140.0f, cellH = 124.0f;
    canvas *__single c = canvas_create(724, 396);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/blend.canvas");
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.13f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 724.0f, 396.0f);

    for (int i = 0; i < 15; i++) {
        int col = i % 5, row = i / 5;
        float ox = M + (float)col * cellW, oy = M + (float)row * cellH;

        // Backdrop: a diagonal gradient block (always source-over).
        canvas_set_global_composite_operation(c, CANVAS_OP_SOURCE_OVER);
        canvas_set_fill_linear_gradient(c, ox + 18.0f, oy + 12.0f, ox + 122.0f,
                                        oy + 100.0f);
        canvas_add_fill_color_stop(c, 0.0f, 0.20f, 0.65f, 0.95f, 1.0f);
        canvas_add_fill_color_stop(c, 1.0f, 0.98f, 0.85f, 0.30f, 1.0f);
        canvas_begin_path(c);
        canvas_round_rect(c, ox + 18.0f, oy + 12.0f, 104.0f, 88.0f, 12.0f);
        canvas_fill(c);

        // Two overlapping discs under this cell's blend mode.
        canvas_set_global_composite_operation(c, cell[i].op);
        canvas_set_fill_rgba(c, 0.92f, 0.26f, 0.21f, 1.0f);
        canvas_begin_path(c);
        canvas_arc(c, ox + 54.0f, oy + 42.0f, 26.0f, 0.0f, TAU, false);
        canvas_fill(c);
        canvas_set_fill_rgba(c, 0.18f, 0.85f, 0.42f, 1.0f);
        canvas_begin_path(c);
        canvas_arc(c, ox + 80.0f, oy + 66.0f, 26.0f, 0.0f, TAU, false);
        canvas_fill(c);

        // Label.
        canvas_set_global_composite_operation(c, CANVAS_OP_SOURCE_OVER);
        canvas_set_fill_rgba(c, 0.90f, 0.92f, 0.96f, 1.0f);
        canvas_set_font_size(c, 13.0f);
        canvas_set_text_align(c, CANVAS_ALIGN_CENTER);
        canvas_fill_text(c, cell[i].name, ox + cellW * 0.5f, oy + 116.0f);
    }
    canvas_set_text_align(c, CANVAS_ALIGN_START);

    save(c, "gallery/blend.png");
}

// Shadows: a sharp drop shadow, a soft blurred shadow, and a text shadow.  Each
// is the op's coverage blurred (the in-tree box blur), tinted, offset, and
// composited under the shape -- all in checked C, so both backends match.
static void shadows(void) {
    canvas *__single c = canvas_create(400, 130);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/shadows.canvas");
    canvas_set_fill_rgba(c, 0.93f, 0.94f, 0.96f, 1.0f);  // light ground
    canvas_fill_rect(c, 0.0f, 0.0f, 400.0f, 130.0f);

    // Sharp offset drop shadow under a rounded rectangle.
    canvas_set_shadow_color_rgba(c, 0.0f, 0.0f, 0.0f, 0.45f);
    canvas_set_shadow_blur(c, 0.0f);
    canvas_set_shadow_offset_x(c, 7.0f);
    canvas_set_shadow_offset_y(c, 7.0f);
    canvas_set_fill_rgba(c, 0.92f, 0.30f, 0.34f, 1.0f);
    canvas_begin_path(c);
    canvas_round_rect(c, 30.0f, 32.0f, 72.0f, 62.0f, 12.0f);
    canvas_fill(c);

    // Soft, blurred shadow under a disc.
    canvas_set_shadow_color_rgba(c, 0.10f, 0.20f, 0.45f, 0.8f);
    canvas_set_shadow_blur(c, 16.0f);
    canvas_set_shadow_offset_x(c, 0.0f);
    canvas_set_shadow_offset_y(c, 5.0f);
    canvas_set_fill_rgba(c, 0.35f, 0.70f, 0.95f, 1.0f);
    canvas_begin_path(c);
    canvas_arc(c, 185.0f, 60.0f, 34.0f, 0.0f, TAU, false);
    canvas_fill(c);

    // Text shadow.
    canvas_set_shadow_color_rgba(c, 0.0f, 0.0f, 0.0f, 0.5f);
    canvas_set_shadow_blur(c, 3.0f);
    canvas_set_shadow_offset_x(c, 2.0f);
    canvas_set_shadow_offset_y(c, 3.0f);
    canvas_set_fill_rgba(c, 0.15f, 0.55f, 0.35f, 1.0f);
    canvas_set_font_size(c, 34.0f);
    canvas_fill_text(c, "shadow", 250.0f, 78.0f);

    save(c, "gallery/shadows.png");
}

// Color emoji: Core Text falls back to AppleColorEmoji and the run's color glyphs
// are drawn as RGBA8 bitmaps (the second text boundary), composited like any other
// paint -- so emoji mix inline with text and take the transform and shadow.
static void emoji(void) {
    canvas *__single c = canvas_create(520, 250);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/emoji.canvas");
    canvas_set_fill_rgba(c, 0.94f, 0.94f, 0.96f, 1.0f);  // light ground (shows shadows)
    canvas_fill_rect(c, 0.0f, 0.0f, 520.0f, 250.0f);

    canvas_set_text_align(c, CANVAS_ALIGN_LEFT);
    canvas_set_text_baseline(c, CANVAS_BASELINE_ALPHABETIC);

    // Inline with Latin + Chinese in a single fill_text.
    canvas_set_fill_rgba(c, 0.16f, 0.17f, 0.22f, 1.0f);
    canvas_set_font_size(c, 30.0f);
    canvas_fill_text(c, "canvas2d 🎨 畫布", 26.0f, 50.0f);

    // A row of color-bitmap emoji.
    canvas_set_font_size(c, 46.0f);
    canvas_fill_text(c, "🌈🚀🌸🍕🐙⭐🎉🍎", 26.0f, 122.0f);

    // Emoji take the pipeline: a drop shadow, a rotation, a larger scale.
    float const cxs[3] = { 95.0f, 260.0f, 425.0f };
    canvas_set_text_align(c, CANVAS_ALIGN_CENTER);
    canvas_set_text_baseline(c, CANVAS_BASELINE_MIDDLE);

    canvas_set_shadow_color_rgba(c, 0.0f, 0.0f, 0.0f, 0.55f);
    canvas_set_shadow_blur(c, 6.0f);
    canvas_set_shadow_offset_x(c, 3.0f);
    canvas_set_shadow_offset_y(c, 4.0f);
    canvas_set_font_size(c, 52.0f);
    canvas_fill_text(c, "🎨", cxs[0], 186.0f);
    canvas_set_shadow_color_rgba(c, 0.0f, 0.0f, 0.0f, 0.0f);  // disable
    canvas_set_shadow_blur(c, 0.0f);
    canvas_set_shadow_offset_x(c, 0.0f);
    canvas_set_shadow_offset_y(c, 0.0f);

    canvas_save(c);
    canvas_translate(c, cxs[1], 186.0f);
    canvas_rotate(c, -0.38f);
    canvas_set_font_size(c, 52.0f);
    canvas_fill_text(c, "🚀", 0.0f, 0.0f);
    canvas_restore(c);

    canvas_set_font_size(c, 70.0f);
    canvas_fill_text(c, "🌈", cxs[2], 186.0f);

    canvas_set_text_baseline(c, CANVAS_BASELINE_ALPHABETIC);
    canvas_set_fill_rgba(c, 0.40f, 0.43f, 0.50f, 1.0f);
    canvas_set_font_size(c, 13.0f);
    char const *const labels[3] = { "shadow", "rotate", "scale" };
    for (int i = 0; i < 3; i++) {
        canvas_fill_text(c, labels[i], cxs[i], 232.0f);
    }
    canvas_set_text_align(c, CANVAS_ALIGN_START);

    save(c, "gallery/emoji.png");
}

// Mip quality on a ruler.  The classic way to judge minification quality is an
// animation zooming over time; laying the sweep along x captures the same
// information in one still.  One emoji at geometrically increasing sizes --
// equal steps cross mip levels at equal rates, so level-selection popping would
// read as periodic sharpness banding -- overlapping at 80% alpha so compositing
// shows, and running past the 160px canonical capture into honest upscale
// softness.  The second row repeats the ramp under progressive rotation: level
// selection answers the transformed device footprint, not the nominal font size.
static void emojiscale(void) {
    canvas *__single c = canvas_create(700, 600);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/emojiscale.canvas");
    canvas_set_fill_rgba(c, 0.94f, 0.94f, 0.96f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 700.0f, 600.0f);

    canvas_set_text_align(c, CANVAS_ALIGN_LEFT);
    canvas_set_text_baseline(c, CANVAS_BASELINE_ALPHABETIC);

    int const n = 12;
    float const s0 = 8.0f, s1 = 200.0f;
    canvas_set_global_alpha(c, 0.8f);
    float x = 14.0f;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)(n - 1);
        float size = s0 * powf(s1 / s0, t);
        canvas_set_font_size(c, size);
        canvas_fill_text(c, "🚀", x, 240.0f);
        x += size * 0.72f;  // tighter than the advance: the overlaps blend
    }
    x = 14.0f;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)(n - 1);
        float size = s0 * powf(s1 / s0, t);
        canvas_save(c);
        canvas_translate(c, x, 545.0f);
        canvas_rotate(c, -0.11f * (float)i);
        canvas_set_font_size(c, size);
        canvas_fill_text(c, "🚀", 0.0f, 0.0f);
        canvas_restore(c);
        x += size * 0.72f;
    }
    canvas_set_global_alpha(c, 1.0f);

    canvas_set_fill_rgba(c, 0.40f, 0.43f, 0.50f, 1.0f);
    canvas_set_font_size(c, 13.0f);
    canvas_fill_text(c, "8px → 200px, geometric; canonical capture 160px", 14.0f, 268.0f);
    canvas_fill_text(c, "the same ramp, progressively rotated", 14.0f, 580.0f);

    save(c, "gallery/emojiscale.png");
}

// Text shaping + font fallback: one fill_text per line, each a greeting in a
// different script.  Core Text picks the right fallback font per run, joins Arabic
// contextually, lays RTL out right-to-left, reorders Devanagari -- all rendered by
// the same coverage rasterizer (and color emoji as bitmaps).
static void shaping(void) {
    canvas *__single c = canvas_create(500, 348);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/shaping.canvas");
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 500.0f, 348.0f);

    canvas_set_text_align(c, CANVAS_ALIGN_LEFT);
    canvas_set_text_baseline(c, CANVAS_BASELINE_ALPHABETIC);

    canvas_set_fill_linear_gradient(c, 36.0f, 0.0f, 360.0f, 0.0f);
    canvas_add_fill_color_stop(c, 0.0f, 0.99f, 0.80f, 0.26f, 1.0f);
    canvas_add_fill_color_stop(c, 1.0f, 0.35f, 0.55f, 0.98f, 1.0f);
    canvas_set_font_size(c, 21.0f);
    canvas_fill_text(c, "one fill_text, every script", 36.0f, 46.0f);

    struct { char const *greeting; char const *label; } const row[8] = {
        { "Hello, world",  "Latin" },
        { "你好,世界",      "Chinese" },
        { "こんにちは",      "Japanese" },
        { "Привет, мир",   "Cyrillic" },
        { "Γειά σου",      "Greek" },
        { "안녕하세요",       "Korean" },
        { "नमस्ते",          "Devanagari - reorders" },
        { "👋 🌍 🎉",        "color emoji" },
    };
    for (int i = 0; i < 8; i++) {
        float y = 92.0f + (float)i * 31.0f;
        float t = (float)i / 8.0f;
        canvas_set_fill_rgba(c, 0.5f + 0.5f * cosf(TAU * t),
                             0.5f + 0.5f * cosf(TAU * (t + 0.33f)),
                             0.5f + 0.5f * cosf(TAU * (t + 0.66f)), 1.0f);
        canvas_set_font_size(c, 28.0f);
        canvas_fill_text(c, row[i].greeting, 40.0f, y);
        canvas_set_fill_rgba(c, 0.55f, 0.59f, 0.67f, 1.0f);
        canvas_set_font_size(c, 12.0f);
        canvas_fill_text(c, row[i].label, 320.0f, y - 4.0f);
    }

    canvas_set_fill_rgba(c, 0.62f, 0.66f, 0.74f, 1.0f);
    canvas_set_font_size(c, 13.0f);
    canvas_fill_text(c, "Core Text fallback + shaping, one coverage rasterizer",
                     40.0f, 338.0f);

    save(c, "gallery/shaping.png");
}

// Proper RTL: the direction attribute drives bidi layout.  Hebrew and Arabic
// paragraphs hang from the right margin (start == right under rtl), the
// Arabic joining contextually; a mixed line shows the rtl paragraph
// reordering embedded Latin; and the bottom rows hang one bidi string off a
// single anchor under every direction x start/end pairing -- the alignment
// flips with direction, and the string itself reorders.
static void rtl(void) {
    canvas *__single c = canvas_create(500, 330);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/rtl.canvas");
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 500.0f, 330.0f);

    // Headline, ltr.
    canvas_set_fill_linear_gradient(c, 36.0f, 0.0f, 464.0f, 0.0f);
    canvas_add_fill_color_stop(c, 0.0f, 0.99f, 0.80f, 0.26f, 1.0f);
    canvas_add_fill_color_stop(c, 1.0f, 0.35f, 0.55f, 0.98f, 1.0f);
    canvas_set_font_size(c, 20.0f);
    canvas_fill_text(c, "direction: rtl", 36.0f, 40.0f);

    // RTL paragraphs from the right margin: start anchors right under rtl.
    float const right = 464.0f;
    canvas_set_direction(c, CANVAS_DIRECTION_RTL);
    canvas_set_text_align(c, CANVAS_ALIGN_START);
    canvas_set_font_size(c, 30.0f);
    canvas_set_fill_rgba(c, 0.95f, 0.83f, 0.45f, 1.0f);
    canvas_fill_text(c, "שלום עולם", right, 88.0f);          // Hebrew
    canvas_set_fill_rgba(c, 0.55f, 0.85f, 0.65f, 1.0f);
    canvas_fill_text(c, "مرحبا بالعالم", right, 130.0f);     // Arabic, joined
    canvas_set_font_size(c, 26.0f);
    canvas_set_fill_rgba(c, 0.70f, 0.75f, 0.95f, 1.0f);
    canvas_fill_text(c, "טקסט עם canvas2d בפנים", right, 172.0f);  // mixed bidi

    // Captions, back to ltr at the left margin.
    canvas_set_direction(c, CANVAS_DIRECTION_LTR);
    canvas_set_text_align(c, CANVAS_ALIGN_LEFT);
    canvas_set_fill_rgba(c, 0.55f, 0.59f, 0.67f, 1.0f);
    canvas_set_font_size(c, 12.0f);
    canvas_fill_text(c, "Hebrew", 36.0f, 84.0f);
    canvas_fill_text(c, "Arabic joins", 36.0f, 126.0f);
    canvas_fill_text(c, "mixed bidi", 36.0f, 168.0f);

    // start/end resolve against direction: four rows off one anchor line.
    float const ax = 250.0f;
    canvas_set_fill_rgba(c, 0.85f, 0.35f, 0.35f, 1.0f);
    canvas_fill_rect(c, ax - 0.5f, 196.0f, 1.0f, 122.0f);
    struct {
        enum canvas_direction dir;
        enum canvas_text_align align;
        char const *label;
    } const row[4] = {
        { CANVAS_DIRECTION_LTR, CANVAS_ALIGN_START, "ltr start" },
        { CANVAS_DIRECTION_LTR, CANVAS_ALIGN_END,   "ltr end" },
        { CANVAS_DIRECTION_RTL, CANVAS_ALIGN_START, "rtl start" },
        { CANVAS_DIRECTION_RTL, CANVAS_ALIGN_END,   "rtl end" },
    };
    for (int i = 0; i < 4; i++) {
        float y = 222.0f + (float)i * 28.0f;
        canvas_set_direction(c, row[i].dir);
        canvas_set_text_align(c, row[i].align);
        canvas_set_fill_rgba(c, 0.90f, 0.92f, 0.96f, 1.0f);
        canvas_set_font_size(c, 18.0f);
        canvas_fill_text(c, "אב ab", ax, y);
        canvas_set_direction(c, CANVAS_DIRECTION_LTR);
        canvas_set_text_align(c, CANVAS_ALIGN_LEFT);
        canvas_set_fill_rgba(c, 0.55f, 0.59f, 0.67f, 1.0f);
        canvas_set_font_size(c, 12.0f);
        canvas_fill_text(c, row[i].label, 36.0f, y);
    }

    save(c, "gallery/rtl.png");
}

// The CSS filter functions: each cell paints the same motif -- a
// diagonal-gradient rounded tile under two translucent discs -- through one
// filter (top-left unfiltered for reference).  Each colour function is a
// precompiled matrix kernel over the op's premultiplied tile; the partial
// alpha is what makes the premultiplied forms visible.  The fourth row is
// blur() -- three box passes over the same tile, the painted region grown by
// the spread -- alone at two strengths, then chained after a colour function
// (the list applies in call order, so saturate sees the already-soft pixels).
// The last row is drop-shadow() -- the drawing composited over a blurred,
// offset, tinted copy of its own alpha -- plain black, a translucent violet,
// and chained after grayscale(1), the order making the gray drawing keep its
// coloured shadow.
static void filters(void) {
    struct {
        void (*add)(canvas *__single cv, float amount);
        float amt;
        void (*add2)(canvas *__single cv, float amount);  // chained second entry
        float amt2;
        bool shadow;                  // append a drop-shadow() after add/add2
        float sdx, sdy, sblur;        // its offset + blur
        float sr, sg, sb, sa;         // its colour
        char const *label;
    } const cell[15] = {
        { .label = "none" },
        { .add = canvas_add_filter_brightness, .amt = 1.5f,       .label = "brightness(1.5)" },
        { .add = canvas_add_filter_contrast,   .amt = 2.0f,       .label = "contrast(2)" },
        { .add = canvas_add_filter_grayscale,  .amt = 1.0f,       .label = "grayscale(1)" },
        { .add = canvas_add_filter_hue_rotate, .amt = TAU / 3.0f, .label = "hue-rotate(120deg)" },
        { .add = canvas_add_filter_invert,     .amt = 1.0f,       .label = "invert(1)" },
        { .add = canvas_add_filter_opacity,    .amt = 0.35f,      .label = "opacity(0.35)" },
        { .add = canvas_add_filter_saturate,   .amt = 3.0f,       .label = "saturate(3)" },
        { .add = canvas_add_filter_sepia,      .amt = 1.0f,       .label = "sepia(1)" },
        { .add = canvas_add_filter_blur,       .amt = 1.5f,       .label = "blur(1.5)" },
        { .add = canvas_add_filter_blur,       .amt = 3.0f,       .label = "blur(3)" },
        { .add = canvas_add_filter_blur,       .amt = 3.0f,
          .add2 = canvas_add_filter_saturate,  .amt2 = 3.0f,      .label = "blur(3) saturate(3)" },
        { .shadow = true, .sdx = 3.0f, .sdy = 3.0f, .sblur = 2.0f,
          .sr = 0.0f, .sg = 0.0f, .sb = 0.0f, .sa = 1.0f,
          .label = "drop-shadow(3 3 2)" },
        { .shadow = true, .sdx = 6.0f, .sdy = 6.0f, .sblur = 3.0f,
          .sr = 0.55f, .sg = 0.25f, .sb = 0.95f, .sa = 0.6f,
          .label = "drop-shadow(violet 60%)" },
        { .add = canvas_add_filter_grayscale,  .amt = 1.0f,
          .shadow = true, .sdx = 6.0f, .sdy = 6.0f, .sblur = 3.0f,
          .sr = 0.55f, .sg = 0.25f, .sb = 0.95f, .sa = 0.85f,
          .label = "grayscale(1) drop-shadow" },
    };
    float const M = 12.0f, cellW = 140.0f, cellH = 124.0f;
    canvas *__single c = canvas_create(444, 644);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/filters.canvas");
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.13f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, 444.0f, 644.0f);

    for (int i = 0; i < 15; i++) {
        int col = i % 3, row = i / 3;
        float ox = M + (float)col * cellW, oy = M + (float)row * cellH;

        // This cell's filter (one function, a two-entry chain, or a chain
        // ending in drop-shadow -- always in list order).
        canvas_set_filter_none(c);
        if (cell[i].add) {
            cell[i].add(c, cell[i].amt);
        }
        if (cell[i].add2) {
            cell[i].add2(c, cell[i].amt2);
        }
        if (cell[i].shadow) {
            canvas_add_filter_drop_shadow(c, cell[i].sdx, cell[i].sdy,
                                          cell[i].sblur, cell[i].sr, cell[i].sg,
                                          cell[i].sb, cell[i].sa);
        }

        // The motif, filtered: a gradient tile, then two translucent discs.
        canvas_set_fill_linear_gradient(c, ox + 18.0f, oy + 12.0f,
                                        ox + 122.0f, oy + 100.0f);
        canvas_add_fill_color_stop(c, 0.0f, 0.98f, 0.55f, 0.15f, 1.0f);
        canvas_add_fill_color_stop(c, 0.5f, 0.85f, 0.25f, 0.55f, 1.0f);
        canvas_add_fill_color_stop(c, 1.0f, 0.20f, 0.55f, 0.95f, 1.0f);
        canvas_begin_path(c);
        canvas_round_rect(c, ox + 18.0f, oy + 12.0f, 104.0f, 88.0f, 12.0f);
        canvas_fill(c);

        canvas_set_fill_rgba(c, 0.15f, 0.90f, 0.45f, 0.55f);
        canvas_begin_path(c);
        canvas_arc(c, ox + 52.0f, oy + 44.0f, 24.0f, 0.0f, TAU, false);
        canvas_fill(c);
        canvas_set_fill_rgba(c, 0.95f, 0.20f, 0.20f, 0.55f);
        canvas_begin_path(c);
        canvas_arc(c, ox + 84.0f, oy + 64.0f, 24.0f, 0.0f, TAU, false);
        canvas_fill(c);

        // Label, with the filter cleared so every caption reads uniformly.
        canvas_set_filter_none(c);
        canvas_set_fill_rgba(c, 0.90f, 0.92f, 0.96f, 1.0f);
        canvas_set_font_size(c, 13.0f);
        canvas_set_text_align(c, CANVAS_ALIGN_CENTER);
        canvas_fill_text(c, cell[i].label, ox + cellW * 0.5f, oy + 116.0f);
    }
    canvas_set_text_align(c, CANVAS_ALIGN_START);

    save(c, "gallery/filters.png");
}

static void render_all(void) {
    shapes();
    affine();
    winding();
    dashes();
    imagedata();
    dirtyrect();
    joinscaps();
    miterdash();
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
    subrect();
    text();
    textgrid();
    textmetrics();
    textmaxwidth();
    porterduff();
    hittest();
    blend();
    shadows();
    emoji();
    emojiscale();
    shaping();
    rtl();
    filters();
}

int main(void) {
    int reps = gallery_reps();
    // GALLERY_NO_SAVE suppresses ALL writes: a profiling run (profile_scene.sh)
    // gets sampled then killed, and the renderer is now fast enough to finish
    // its reps and reach the file-writing one before the kill lands -- which
    // once caught a committed .canvas mid-write, truncated.  Under the knob the
    // process can be killed (or finish) at any moment without touching the
    // committed gallery.
    bool const no_save = getenv("GALLERY_NO_SAVE") != NULL;
    for (int rep = 0; rep < reps; rep++) {
        g_skip_save = no_save || rep < reps - 1;  // write files only on the final rep
        render_all();
    }
    return 0;
}
