// Renders the showcase PNGs in gallery/.  Built like any consumer (public API,
// -std=c23 -fbounds-safety -Weverything), then run by `ninja images`.  One
// deliberate exception: the selection scene reaches into the internal text API
// (canvas2d_text.h) for shaped-line hit-testing geometry -- selection spans and
// caret positions have no public mirror yet -- while its drawing stays public
// ops, so its program replays fontless like every other scene's.

#include "canvas2d.h"
#include "canvas2d_image.h"
#include "canvas2d_path2d.h"
#include "canvas2d_color.h"
#include "canvas2d_text.h"

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
    int const reps = env ? atoi(env) : 1;
    return reps < 1 ? 1 : reps;
}

static void save(struct canvas2d_context *__single cv, char const *__null_terminated path) {
    if (!g_skip_save && !canvas2d_write_png(cv, path)) {
        (void)fprintf(stderr, "gallery: write failed: %s\n", path);
    }
    canvas2d_free(cv);
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
static void record_scene(struct canvas2d_context *__single cv, char const *__null_terminated path) {
    if (!g_skip_save && !canvas2d_record_to(cv, path)) {
        (void)fprintf(stderr, "gallery: record failed: %s\n", path);
    }
}

static void star(struct canvas2d_context *__single cv, float cx, float cy, float r) {
    canvas2d_begin_path(cv);
    for (int i = 0; i < 5; i++) {
        float const a = -TAU * 0.25f + (float)i * (TAU * 0.4f);
        if (i == 0) {
            canvas2d_move_to(cv, cx + r * cosf(a), cy + r * sinf(a));
        } else {
            canvas2d_line_to(cv, cx + r * cosf(a), cy + r * sinf(a));
        }
    }
    canvas2d_close_path(cv);
}

// Transforms, alpha, filled curves/arcs, and strokes.
static void shapes(void) {
    struct canvas2d_context *__single c = canvas2d(240, 180, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/shapes.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.12f, 0.12f, 0.16f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 240.0f, 180.0f);

    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.90f, 0.25f, 0.25f, 1.0f);
    canvas2d_fill_rect(c, 20.0f, 20.0f, 55.0f, 55.0f);

    canvas2d_save(c);
    canvas2d_translate(c, 150.0f, 52.0f);
    canvas2d_rotate(c, 0.5f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.25f, 0.80f, 0.35f, 1.0f);
    canvas2d_fill_rect(c, -28.0f, -28.0f, 56.0f, 56.0f);
    canvas2d_restore(c);

    canvas2d_set_global_alpha(c, 0.5f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.25f, 0.45f, 0.95f, 1.0f);
    canvas2d_fill_rect(c, 55.0f, 95.0f, 110.0f, 65.0f);
    canvas2d_set_global_alpha(c, 1.0f);

    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.30f, 0.85f, 0.85f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_move_to(c, 100.0f, 28.0f);
    canvas2d_bezier_curve_to(c, 150.0f, 8.0f, 150.0f, 88.0f, 100.0f, 68.0f);
    canvas2d_bezier_curve_to(c, 70.0f, 58.0f, 70.0f, 38.0f, 100.0f, 28.0f);
    canvas2d_close_path(c);
    canvas2d_fill(c, CANVAS2D_NONZERO);

    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.85f, 0.30f, 0.75f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_arc(c, 198.0f, 135.0f, 28.0f, 0.0f, TAU, false);
    canvas2d_fill(c, CANVAS2D_NONZERO);
    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 1.0f, 0.60f, 0.10f, 1.0f);
    canvas2d_set_line_width(c, 4.0f);
    canvas2d_begin_path(c);
    canvas2d_arc(c, 198.0f, 135.0f, 28.0f, 0.0f, TAU, false);
    canvas2d_close_path(c);
    canvas2d_stroke(c);

    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.95f, 0.95f, 0.98f, 1.0f);
    canvas2d_set_line_width(c, 5.0f);
    canvas2d_begin_path(c);
    canvas2d_move_to(c, 20.0f, 125.0f);
    canvas2d_line_to(c, 45.0f, 100.0f);
    canvas2d_line_to(c, 70.0f, 125.0f);
    canvas2d_line_to(c, 95.0f, 100.0f);
    canvas2d_stroke(c);

    save(c, "gallery/shapes.png");
}

// Winding rules: a donut (nonzero hole) and a pentagram filled both ways.
static void winding(void) {
    struct canvas2d_context *__single c = canvas2d(300, 120, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/winding.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 300.0f, 120.0f);

    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.95f, 0.80f, 0.25f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_rect(c, 20.0f, 20.0f, 80.0f, 80.0f);
    canvas2d_move_to(c, 85.0f, 35.0f);
    canvas2d_line_to(c, 35.0f, 35.0f);
    canvas2d_line_to(c, 35.0f, 85.0f);
    canvas2d_line_to(c, 85.0f, 85.0f);
    canvas2d_close_path(c);
    canvas2d_fill(c, CANVAS2D_NONZERO);

    star(c, 160.0f, 60.0f, 40.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.90f, 0.30f, 0.40f, 1.0f);
    canvas2d_fill(c, CANVAS2D_NONZERO);

    star(c, 250.0f, 60.0f, 40.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.30f, 0.75f, 0.95f, 1.0f);
    canvas2d_fill(c, CANVAS2D_EVENODD);

    save(c, "gallery/winding.png");
}

// Line dashing: a few patterns, an offset, and a dashed circle.
static void dashes(void) {
    struct canvas2d_context *__single c = canvas2d(260, 120, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/dashes.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 260.0f, 120.0f);
    canvas2d_set_line_width(c, 4.0f);

    float const d1[2] = { 14.0f, 9.0f };
    canvas2d_set_line_dash(c, d1, 2);
    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.90f, 0.45f, 0.45f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_move_to(c, 15.0f, 26.0f);
    canvas2d_line_to(c, 245.0f, 26.0f);
    canvas2d_stroke(c);

    float const d2[4] = { 2.0f, 7.0f, 14.0f, 7.0f };
    canvas2d_set_line_dash(c, d2, 4);
    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.45f, 0.85f, 0.55f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_move_to(c, 15.0f, 50.0f);
    canvas2d_line_to(c, 245.0f, 50.0f);
    canvas2d_stroke(c);

    float const d3[2] = { 2.0f, 6.0f };
    canvas2d_set_line_dash(c, d3, 2);
    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.55f, 0.65f, 0.95f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_move_to(c, 15.0f, 72.0f);
    canvas2d_line_to(c, 245.0f, 72.0f);
    canvas2d_stroke(c);

    float const d4[2] = { 11.0f, 8.0f };
    canvas2d_set_line_dash(c, d4, 2);
    canvas2d_set_line_width(c, 3.0f);
    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.95f, 0.85f, 0.30f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_arc(c, 130.0f, 98.0f, 16.0f, 0.0f, TAU, false);
    canvas2d_close_path(c);
    canvas2d_stroke(c);

    save(c, "gallery/dashes.png");
}

// getImageData / putImageData: capture a motif and stamp copies of it.
static void imagedata(void) {
    struct canvas2d_context *__single c = canvas2d(240, 90, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/imagedata.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.12f, 0.12f, 0.16f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 240.0f, 90.0f);

    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.95f, 0.80f, 0.25f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_arc(c, 30.0f, 45.0f, 17.0f, 0.0f, TAU, false);
    canvas2d_fill(c, CANVAS2D_NONZERO);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.85f, 0.30f, 0.75f, 1.0f);
    canvas2d_fill_rect(c, 12.0f, 27.0f, 14.0f, 14.0f);

    int const blen = 44 * 44 * 4;
    uint8_t *__counted_by(blen) block = malloc((size_t)blen);
    if (block) {
        canvas2d_get_image_data(c, CANVAS2D_CS_SRGB, 8, 23, 44, 44, block, blen);
        for (int k = 1; k < 5; k++) {
            canvas2d_put_image_data(c, CANVAS2D_CS_SRGB, block, blen, 44, 44, 8 + k * 46, 23);
        }
        free(block);
    }

    save(c, "gallery/imagedata.png");
}

// Line joins (miter / round / bevel) on sharp Vs, and caps (butt / round /
// square) on short segments.
static void joinscaps(void) {
    struct canvas2d_context *__single c = canvas2d(280, 160, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/joins.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 280.0f, 160.0f);

    enum canvas2d_line_join const js[3] = { CANVAS2D_JOIN_MITER, CANVAS2D_JOIN_ROUND,
                                     CANVAS2D_JOIN_BEVEL };
    canvas2d_set_line_width(c, 16.0f);
    for (int k = 0; k < 3; k++) {
        canvas2d_set_line_join(c, js[k]);
        canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.95f, 0.55f, 0.35f, 1.0f);
        float const cx = 55.0f + (float)k * 85.0f;
        canvas2d_begin_path(c);
        canvas2d_move_to(c, cx - 28.0f, 80.0f);
        canvas2d_line_to(c, cx, 26.0f);
        canvas2d_line_to(c, cx + 28.0f, 80.0f);
        canvas2d_stroke(c);
    }

    enum canvas2d_line_cap const cs[3] = { CANVAS2D_CAP_BUTT, CANVAS2D_CAP_ROUND,
                                    CANVAS2D_CAP_SQUARE };
    canvas2d_set_line_join(c, CANVAS2D_JOIN_MITER);
    for (int k = 0; k < 3; k++) {
        canvas2d_set_line_cap(c, cs[k]);
        canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.40f, 0.80f, 0.95f, 1.0f);
        float const x0 = 45.0f + (float)k * 80.0f;
        canvas2d_begin_path(c);
        canvas2d_move_to(c, x0, 130.0f);
        canvas2d_line_to(c, x0 + 45.0f, 130.0f);
        canvas2d_stroke(c);
    }

    save(c, "gallery/joins.png");
}

// Path primitives: a filled ellipse, a rounded rectangle, and an arcTo fillet.
static void paths(void) {
    struct canvas2d_context *__single c = canvas2d(280, 170, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/paths.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 280.0f, 170.0f);

    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.40f, 0.65f, 0.95f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_ellipse(c, 72.0f, 52.0f, 52.0f, 28.0f, 0.0f, 0.0f, TAU, false);
    canvas2d_fill(c, CANVAS2D_NONZERO);

    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.55f, 0.85f, 0.45f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_round_rect(c, 158.0f, 16.0f, 100.0f, 72.0f, 22.0f);
    canvas2d_fill(c, CANVAS2D_NONZERO);
    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.95f, 0.60f, 0.20f, 1.0f);
    canvas2d_set_line_width(c, 4.0f);
    canvas2d_begin_path(c);
    canvas2d_round_rect(c, 158.0f, 16.0f, 100.0f, 72.0f, 22.0f);
    canvas2d_stroke(c);

    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.85f, 0.45f, 0.90f, 1.0f);
    canvas2d_set_line_width(c, 6.0f);
    canvas2d_set_line_cap(c, CANVAS2D_CAP_ROUND);
    canvas2d_begin_path(c);
    canvas2d_move_to(c, 30.0f, 150.0f);
    canvas2d_arc_to(c, 150.0f, 150.0f, 150.0f, 105.0f, 30.0f);
    canvas2d_line_to(c, 150.0f, 105.0f);
    canvas2d_stroke(c);

    save(c, "gallery/paths.png");
}

// roundRect with per-corner elliptical radii: four cards, each a different recipe
// of eight (x,y) corner radii, plus a wide capsule whose 200px radii are far
// bigger than its 36px height -- the CSS overlap rule scales them down to a clean
// half-height pill.
static void roundrect(void) {
    struct canvas2d_context *__single c = canvas2d(548, 232, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/roundrect.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 548.0f, 232.0f);

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
        float const x = x0 + (float)i * (cw + gap);
        for (int pass = 0; pass < 2; pass++) {
            canvas2d_begin_path(c);
            canvas2d_round_rect_radii(c, x, y0, cw, ch,
                                    card[i].r[0], card[i].r[1], card[i].r[2],
                                    card[i].r[3], card[i].r[4], card[i].r[5],
                                    card[i].r[6], card[i].r[7]);
            if (pass == 0) {
                canvas2d_set_fill_linear_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, x, y0, x + cw, y0 + ch);
                canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.0f, card[i].c0[0], card[i].c0[1],
                                           card[i].c0[2], 1.0f);
                canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 1.0f, card[i].c1[0], card[i].c1[1],
                                           card[i].c1[2], 1.0f);
                canvas2d_fill(c, CANVAS2D_NONZERO);
            } else {
                canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.96f, 0.97f, 0.99f, 0.9f);
                canvas2d_set_line_width(c, 2.0f);
                canvas2d_stroke(c);
            }
        }
    }

    // Wide capsule: huge radii everywhere, clamped by the overlap rule.
    float const bx = 24.0f, by = 178.0f, bw = 500.0f, bh = 36.0f;
    canvas2d_set_fill_linear_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, bx, 0.0f, bx + bw, 0.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.00f, 0.99f, 0.80f, 0.26f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.50f, 0.95f, 0.35f, 0.45f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 1.00f, 0.35f, 0.55f, 0.98f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_round_rect_radii(c, bx, by, bw, bh, 200.0f, 200.0f, 200.0f, 200.0f,
                            200.0f, 200.0f, 200.0f, 200.0f);
    canvas2d_fill(c, CANVAS2D_NONZERO);

    save(c, "gallery/roundrect.png");
}

// strokeRect: outline rectangles without disturbing the current path.  A 3x2 grid
// shows the three joins on a thick outline, a dashed rect, a rotated-CTM quad with
// a gradient stroke, and the degenerate zero-extent rect (which strokes a line).
static void strokerect(void) {
    float const margin = 12.0f, cw = 148.0f, ch = 104.0f;
    struct canvas2d_context *__single c = canvas2d(468, 232, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/strokerect.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 468.0f, 232.0f);

    char const *const labels[6] = { "miter join", "round join", "bevel join",
                                    "dashed", "rotated quad", "degenerate line" };
    for (int idx = 0; idx < 6; idx++) {
        int col = idx % 3, row = idx / 3;
        float ox = margin + (float)col * cw, oy = margin + (float)row * ch;
        float rx = ox + 26.0f, ry = oy + 20.0f, rw = 96.0f, rh = 46.0f;

        // Reset all line styles to defaults at the top of every cell.
        float const solid[1] = { 1.0f };
        canvas2d_set_line_dash(c, solid, 0);
        canvas2d_set_line_join(c, CANVAS2D_JOIN_MITER);
        canvas2d_set_line_cap(c, CANVAS2D_CAP_BUTT);

        if (idx == 0 || idx == 1 || idx == 2) {
            enum canvas2d_line_join const j[3] = { CANVAS2D_JOIN_MITER, CANVAS2D_JOIN_ROUND,
                                            CANVAS2D_JOIN_BEVEL };
            float const col3[3][3] = { { 0.96f, 0.56f, 0.26f },
                                       { 0.40f, 0.82f, 0.50f },
                                       { 0.36f, 0.62f, 0.95f } };
            canvas2d_set_line_join(c, j[idx]);
            canvas2d_set_line_width(c, 14.0f);
            canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, col3[idx][0], col3[idx][1], col3[idx][2], 1.0f);
            canvas2d_stroke_rect(c, rx, ry, rw, rh);
        } else if (idx == 3) {
            float const dash[2] = { 11.0f, 7.0f };
            canvas2d_set_line_dash(c, dash, 2);
            canvas2d_set_line_width(c, 4.0f);
            canvas2d_set_line_cap(c, CANVAS2D_CAP_ROUND);
            canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.95f, 0.82f, 0.30f, 1.0f);
            canvas2d_stroke_rect(c, rx, ry, rw, rh);
        } else if (idx == 4) {
            // Rotated CTM strokes a rotated quad (corners go through the transform).
            canvas2d_save(c);
            canvas2d_translate(c, ox + 74.0f, oy + 42.0f);
            canvas2d_rotate(c, 0.32f);
            canvas2d_set_stroke_linear_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, -44.0f, 0.0f, 44.0f, 0.0f);
            canvas2d_add_stroke_color_stop(c, CANVAS2D_CS_SRGB, 0.0f, 0.30f, 0.90f, 0.95f, 1.0f);
            canvas2d_add_stroke_color_stop(c, CANVAS2D_CS_SRGB, 1.0f, 0.95f, 0.35f, 0.85f, 1.0f);
            canvas2d_set_line_width(c, 8.0f);
            canvas2d_set_line_join(c, CANVAS2D_JOIN_ROUND);
            canvas2d_stroke_rect(c, -44.0f, -22.0f, 88.0f, 44.0f);
            canvas2d_restore(c);
        } else {
            // h == 0 and w == 0 degenerate to round-capped lines: a crisp plus.
            float mx = ox + 74.0f, my = oy + 42.0f;
            canvas2d_set_line_width(c, 10.0f);
            canvas2d_set_line_cap(c, CANVAS2D_CAP_ROUND);
            canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.95f, 0.45f, 0.55f, 1.0f);
            canvas2d_stroke_rect(c, mx - 44.0f, my, 88.0f, 0.0f);
            canvas2d_stroke_rect(c, mx, my - 24.0f, 0.0f, 48.0f);
        }

        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.78f, 0.82f, 0.90f, 1.0f);
        canvas2d_set_font_size(c, 14.0f);
        canvas2d_fill_text(c, labels[idx], ox + 26.0f, oy + 92.0f);
    }

    save(c, "gallery/strokerect.png");
}

// createConicGradient: colours sweep clockwise around a centre.  A smooth rainbow
// wheel, a hard-stop "pie" (coincident stop offsets make crisp sector edges), and
// a conic-gradient *stroke* ring around a two-tone conic medallion.
static void conic(void) {
    struct canvas2d_context *__single c = canvas2d(440, 176, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/conic.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 440.0f, 176.0f);

    float const cy = 78.0f, r = 56.0f;
    float const cx[3] = { 78.0f, 220.0f, 362.0f };
    char const *const labels[3] = { "rainbow wheel", "hard-stop sectors",
                                    "conic stroke" };
    int const ns = 13;

    // A: smooth rainbow wheel from the cosine palette (wraps red -> red).
    canvas2d_set_fill_conic_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, -TAU * 0.25f, cx[0], cy);
    for (int k = 0; k < ns; k++) {
        float t = (float)k / (float)(ns - 1);
        canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, t, 0.5f + 0.5f * cosf(TAU * t),
                                   0.5f + 0.5f * cosf(TAU * (t + 0.33f)),
                                   0.5f + 0.5f * cosf(TAU * (t + 0.66f)), 1.0f);
    }
    canvas2d_begin_path(c);
    canvas2d_arc(c, cx[0], cy, r, 0.0f, TAU, false);
    canvas2d_fill(c, CANVAS2D_NONZERO);

    // B: five solid sectors via paired (coincident) stops -> hard edges.
    float const bnd[6] = { 0.00f, 0.18f, 0.40f, 0.55f, 0.78f, 1.00f };
    float const sect[5][3] = { { 0.97f, 0.78f, 0.24f }, { 0.20f, 0.78f, 0.70f },
                               { 0.95f, 0.45f, 0.40f }, { 0.42f, 0.40f, 0.92f },
                               { 0.45f, 0.82f, 0.45f } };
    canvas2d_set_fill_conic_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, -TAU * 0.25f, cx[1], cy);
    for (int s = 0; s < 5; s++) {
        canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, bnd[s], sect[s][0], sect[s][1], sect[s][2],
                                   1.0f);
        canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, bnd[s + 1], sect[s][0], sect[s][1],
                                   sect[s][2], 1.0f);
    }
    canvas2d_begin_path(c);
    canvas2d_arc(c, cx[1], cy, r, 0.0f, TAU, false);
    canvas2d_fill(c, CANVAS2D_NONZERO);

    // C: a two-tone conic medallion, ringed by a conic-gradient stroke.
    canvas2d_set_fill_conic_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, TAU * 0.1f, cx[2], cy);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.00f, 0.82f, 0.84f, 0.90f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.25f, 0.30f, 0.33f, 0.42f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.50f, 0.82f, 0.84f, 0.90f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.75f, 0.30f, 0.33f, 0.42f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 1.00f, 0.82f, 0.84f, 0.90f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_arc(c, cx[2], cy, 24.0f, 0.0f, TAU, false);
    canvas2d_fill(c, CANVAS2D_NONZERO);

    canvas2d_set_stroke_conic_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, 0.0f, cx[2], cy);
    for (int k = 0; k < ns; k++) {
        float const t = (float)k / (float)(ns - 1);
        canvas2d_add_stroke_color_stop(c, CANVAS2D_CS_SRGB, t, 0.5f + 0.5f * cosf(TAU * t),
                                     0.5f + 0.5f * cosf(TAU * (t + 0.33f)),
                                     0.5f + 0.5f * cosf(TAU * (t + 0.66f)), 1.0f);
    }
    canvas2d_set_line_width(c, 15.0f);
    canvas2d_begin_path(c);
    canvas2d_arc(c, cx[2], cy, 46.0f, 0.0f, TAU, false);
    canvas2d_close_path(c);
    canvas2d_stroke(c);

    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.80f, 0.83f, 0.90f, 1.0f);
    canvas2d_set_font_size(c, 14.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
    for (int i = 0; i < 3; i++) {
        canvas2d_fill_text(c, labels[i], cx[i], 166.0f);
    }
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);

    save(c, "gallery/conic.png");
}

// A 32x32 seamless tile: an indigo ground, a gold gem at the centre, and coral
// quarter-dots in the corners that join into full dots across tile seams.
static void make_tile(uint8_t *__counted_by(32 * 32 * 4) t) {
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            int const i = (y * 32 + x) * 4;
            float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
            float r = 0.16f, g = 0.18f, b = 0.36f;  // indigo ground
            float cdx = fx - 16.0f, cdy = fy - 16.0f;
            if (cdx * cdx + cdy * cdy < 10.5f * 10.5f) {  // centre gem
                r = 0.97f; g = 0.80f; b = 0.28f;
            }
            float const gx = fx < 16.0f ? fx : fx - 32.0f;  // distance to nearest corner
            float const gy = fy < 16.0f ? fy : fy - 32.0f;
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
    struct canvas2d_context *__single c = canvas2d(474, 212, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/pattern.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 474.0f, 212.0f);

    uint8_t tile[32 * 32 * 4];
    make_tile(tile);

    enum canvas2d_pattern_repeat const modes[4] = { CANVAS2D_REPEAT, CANVAS2D_REPEAT_X,
                                             CANVAS2D_REPEAT_Y, CANVAS2D_NO_REPEAT };
    char const *const labels[4] = { "repeat", "repeat-x", "repeat-y",
                                    "no-repeat" };
    for (int i = 0; i < 4; i++) {
        float ox = 18.0f + (float)i * 114.0f, oy = 22.0f;
        // Anchor the tile grid to the panel corner via the CTM, then fill.
        canvas2d_save(c);
        canvas2d_translate(c, ox, oy);
        canvas2d_set_fill_pattern(c, CANVAS2D_CS_SRGB, tile, 32, 32, modes[i]);
        canvas2d_fill_rect(c, 0.0f, 0.0f, 96.0f, 96.0f);
        canvas2d_restore(c);

        canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.42f, 0.46f, 0.55f, 1.0f);
        canvas2d_set_line_width(c, 1.5f);
        canvas2d_stroke_rect(c, ox, oy, 96.0f, 96.0f);

        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.80f, 0.83f, 0.90f, 1.0f);
        canvas2d_set_font_size(c, 13.0f);
        canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
        canvas2d_fill_text(c, labels[i], ox + 48.0f, 134.0f);
    }

    // The pattern is a fill paint like any other, so glyph coverage samples it too.
    canvas2d_set_fill_pattern(c, CANVAS2D_CS_SRGB, tile, 32, 32, CANVAS2D_REPEAT);
    canvas2d_set_font_size(c, 52.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
    canvas2d_fill_text(c, "canvas2d", 237.0f, 196.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);

    save(c, "gallery/pattern.png");
}

// imageSmoothingEnabled: the same 16x16 pixel-art source upscaled 8.75x with
// smoothing off (crisp nearest-neighbour blocks) and on (bilinear blend).
static void smoothing(void) {
    struct canvas2d_context *__single c = canvas2d(440, 210, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/smoothing.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 440.0f, 210.0f);

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
            int const i = (y * 16 + x) * 4;
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
    canvas2d_set_image_smoothing_enabled(c, false);
    canvas2d_draw_bitmap_scaled(c, CANVAS2D_CS_SRGB, src, 16, 16, 24.0f, 62.0f, 64.0f, 64.0f);
    // Big nearest-neighbour upscale (blocky).
    canvas2d_draw_bitmap_scaled(c, CANVAS2D_CS_SRGB, src, 16, 16, 120.0f, 24.0f, 140.0f, 140.0f);
    // Big bilinear upscale (smooth).
    canvas2d_set_image_smoothing_enabled(c, true);
    canvas2d_draw_bitmap_scaled(c, CANVAS2D_CS_SRGB, src, 16, 16, 276.0f, 24.0f, 140.0f, 140.0f);

    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.80f, 0.83f, 0.90f, 1.0f);
    canvas2d_set_font_size(c, 14.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
    canvas2d_fill_text(c, "16x16 source", 56.0f, 186.0f);
    canvas2d_fill_text(c, "nearest", 190.0f, 186.0f);
    canvas2d_fill_text(c, "smooth (bilinear)", 346.0f, 186.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);

    save(c, "gallery/smoothing.png");
}

// textAlign / textBaseline.  Top: three words placed at one vertical anchor (each
// word names its own alignment).  Bottom: "Hg" set six ways against one horizontal
// baseline guide, so each mode's vertical shift is visible.
static void textgrid(void) {
    struct canvas2d_context *__single c = canvas2d(520, 256, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/textgrid.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 520.0f, 256.0f);

    // --- textAlign: a vertical anchor, three self-naming words ---
    canvas2d_set_text_baseline(c, CANVAS2D_BASELINE_ALPHABETIC);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.62f, 0.66f, 0.74f, 1.0f);
    canvas2d_set_font_size(c, 13.0f);
    canvas2d_fill_text(c, "textAlign", 24.0f, 34.0f);

    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.85f, 0.40f, 0.85f, 0.75f);
    canvas2d_set_line_width(c, 1.5f);
    canvas2d_begin_path(c);
    canvas2d_move_to(c, 270.0f, 44.0f);
    canvas2d_line_to(c, 270.0f, 124.0f);
    canvas2d_stroke(c);

    enum canvas2d_text_align const aligns[3] = { CANVAS2D_ALIGN_LEFT, CANVAS2D_ALIGN_CENTER,
                                          CANVAS2D_ALIGN_RIGHT };
    char const *const atext[3] = { "left", "center", "right" };
    float const ay[3] = { 64.0f, 92.0f, 120.0f };
    for (int i = 0; i < 3; i++) {
        canvas2d_set_text_align(c, aligns[i]);
        canvas2d_set_font_size(c, 24.0f);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.93f, 0.94f, 0.98f, 1.0f);
        canvas2d_fill_text(c, atext[i], 270.0f, ay[i]);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.40f, 0.85f, 0.95f, 1.0f);
        canvas2d_begin_path(c);
        canvas2d_arc(c, 270.0f, ay[i], 3.0f, 0.0f, TAU, false);
        canvas2d_fill(c, CANVAS2D_NONZERO);
    }

    // --- textBaseline: one horizontal baseline, "Hg" set six ways ---
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.62f, 0.66f, 0.74f, 1.0f);
    canvas2d_set_font_size(c, 13.0f);
    canvas2d_fill_text(c, "textBaseline", 24.0f, 154.0f);

    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.40f, 0.85f, 0.95f, 0.75f);
    canvas2d_set_line_width(c, 1.5f);
    canvas2d_begin_path(c);
    canvas2d_move_to(c, 24.0f, 200.0f);
    canvas2d_line_to(c, 496.0f, 200.0f);
    canvas2d_stroke(c);

    enum canvas2d_text_baseline const bl[6] = {
        CANVAS2D_BASELINE_TOP, CANVAS2D_BASELINE_HANGING, CANVAS2D_BASELINE_MIDDLE,
        CANVAS2D_BASELINE_ALPHABETIC, CANVAS2D_BASELINE_IDEOGRAPHIC,
        CANVAS2D_BASELINE_BOTTOM,
    };
    char const *const bname[6] = { "top",        "hang", "middle",
                                   "alphabetic", "ideo", "bottom" };
    for (int i = 0; i < 6; i++) {
        float const cx = 30.0f + ((float)i + 0.5f) * ((490.0f - 30.0f) / 6.0f);
        canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
        canvas2d_set_text_baseline(c, bl[i]);
        canvas2d_set_font_size(c, 28.0f);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.93f, 0.94f, 0.98f, 1.0f);
        canvas2d_fill_text(c, "Hg", cx, 200.0f);
        canvas2d_set_text_baseline(c, CANVAS2D_BASELINE_ALPHABETIC);
        canvas2d_set_font_size(c, 11.0f);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.62f, 0.66f, 0.74f, 1.0f);
        canvas2d_fill_text(c, bname[i], cx, 246.0f);
    }
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);
    canvas2d_set_text_baseline(c, CANVAS2D_BASELINE_ALPHABETIC);

    save(c, "gallery/textgrid.png");
}

// measureText: draw a word at the alphabetic origin, then overlay its TextMetrics
// -- the tight actual bounding box, the looser font bounding box, the advance
// width, the hanging/alphabetic/ideographic baselines, and the origin point.
static void textmetrics(void) {
    struct canvas2d_context *__single c = canvas2d(560, 250, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/textmetrics.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 560.0f, 250.0f);

    char const *const word = "Graphics";
    float const x0 = 140.0f, y0 = 150.0f;
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);
    canvas2d_set_text_baseline(c, CANVAS2D_BASELINE_ALPHABETIC);
    canvas2d_set_font_size(c, 62.0f);
    canvas2d_text_metrics const m = canvas2d_measure_text_full(c, word);

    // Baselines first (behind everything): horizontal guides across the word.
    float lx0 = x0 - 16.0f, lx1 = x0 + m.width + 16.0f;
    float const solid[1] = { 1.0f }, dash[2] = { 5.0f, 4.0f };
    canvas2d_set_line_width(c, 1.4f);
    struct { float y; float r, g, b; char const *name; } base[3] = {
        { y0 - m.hanging_baseline,     0.95f, 0.85f, 0.35f, "hanging" },
        { y0,                          0.45f, 0.88f, 0.50f, "alphabetic" },
        { y0 - m.ideographic_baseline, 0.75f, 0.55f, 0.95f, "ideographic" },
    };
    for (int i = 0; i < 3; i++) {
        canvas2d_set_line_dash(c, i == 1 ? solid : dash, i == 1 ? 0 : 2);
        canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, base[i].r, base[i].g, base[i].b, 0.95f);
        canvas2d_begin_path(c);
        canvas2d_move_to(c, lx0, base[i].y);
        canvas2d_line_to(c, lx1, base[i].y);
        canvas2d_stroke(c);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, base[i].r, base[i].g, base[i].b, 1.0f);
        canvas2d_set_font_size(c, 11.0f);
        canvas2d_set_text_align(c, CANVAS2D_ALIGN_LEFT);
        canvas2d_fill_text(c, base[i].name, lx1 + 6.0f, base[i].y + 3.5f);
    }

    // The glyphs.
    canvas2d_set_line_dash(c, solid, 0);
    canvas2d_set_font_size(c, 62.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.90f, 0.91f, 0.96f, 1.0f);
    canvas2d_fill_text(c, word, x0, y0);

    // Font bounding box (orange dashed) over the advance width.
    canvas2d_set_line_dash(c, dash, 2);
    canvas2d_set_line_width(c, 1.5f);
    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.97f, 0.62f, 0.25f, 0.95f);
    canvas2d_stroke_rect(c, x0, y0 - m.font_bounding_box_ascent, m.width,
                       m.font_bounding_box_ascent + m.font_bounding_box_descent);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.97f, 0.62f, 0.25f, 1.0f);
    canvas2d_set_font_size(c, 11.0f);
    canvas2d_fill_text(c, "font box", x0 + 3.0f, y0 - m.font_bounding_box_ascent - 4.0f);

    // Actual (ink) bounding box (cyan solid).
    canvas2d_set_line_dash(c, solid, 0);
    canvas2d_set_line_width(c, 1.8f);
    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.30f, 0.85f, 0.95f, 1.0f);
    float ab_l = x0 - m.actual_bounding_box_left, ab_t = y0 - m.actual_bounding_box_ascent;
    canvas2d_stroke_rect(c, ab_l, ab_t,
                       m.actual_bounding_box_left + m.actual_bounding_box_right,
                       m.actual_bounding_box_ascent + m.actual_bounding_box_descent);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.30f, 0.85f, 0.95f, 1.0f);
    canvas2d_fill_text(c, "actual box", ab_l + 2.0f, ab_t - 4.0f);

    // Advance-width measure below, with the measured value.
    float const wy = y0 + m.font_bounding_box_descent + 26.0f;
    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.88f, 0.90f, 0.96f, 1.0f);
    canvas2d_set_line_width(c, 1.4f);
    canvas2d_begin_path(c);
    canvas2d_move_to(c, x0, wy - 5.0f);
    canvas2d_line_to(c, x0, wy + 5.0f);
    canvas2d_move_to(c, x0, wy);
    canvas2d_line_to(c, x0 + m.width, wy);
    canvas2d_move_to(c, x0 + m.width, wy - 5.0f);
    canvas2d_line_to(c, x0 + m.width, wy + 5.0f);
    canvas2d_stroke(c);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.88f, 0.90f, 0.96f, 1.0f);
    canvas2d_set_font_size(c, 12.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
    canvas2d_fill_text(c, "advance width", x0 + m.width * 0.5f, wy + 18.0f);

    // Origin point.
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.96f, 0.35f, 0.45f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_arc(c, x0, y0, 4.0f, 0.0f, TAU, false);
    canvas2d_fill(c, CANVAS2D_NONZERO);

    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);
    save(c, "gallery/textmetrics.png");
}

// maxWidth: the same phrase drawn unconstrained (it overflows the right marker)
// and with a maxWidth equal to the marked span (condensed horizontally to fit).
static void textmaxwidth(void) {
    struct canvas2d_context *__single c = canvas2d(520, 188, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/textmaxwidth.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 520.0f, 188.0f);

    char const *const phrase = "Condense me to fit the box!";
    float const L = 40.0f, maxw = 258.0f, R = L + maxw;
    float const gy0 = 18.0f, gy1 = 150.0f;

    // Shade the overflow zone (right of the marker) faint red.
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.90f, 0.30f, 0.32f, 0.10f);
    canvas2d_fill_rect(c, R, gy0, 520.0f - R, gy1 - gy0);

    // The two maxWidth markers (dashed vertical guides).
    float const dash[2] = { 5.0f, 4.0f };
    canvas2d_set_line_dash(c, dash, 2);
    canvas2d_set_line_width(c, 1.4f);
    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.55f, 0.59f, 0.68f, 0.85f);
    for (int k = 0; k < 2; k++) {
        float const gx = k == 0 ? L : R;
        canvas2d_begin_path(c);
        canvas2d_move_to(c, gx, gy0);
        canvas2d_line_to(c, gx, gy1);
        canvas2d_stroke(c);
    }
    float const solid[1] = { 1.0f };
    canvas2d_set_line_dash(c, solid, 0);

    canvas2d_set_text_align(c, CANVAS2D_ALIGN_LEFT);
    canvas2d_set_text_baseline(c, CANVAS2D_BASELINE_ALPHABETIC);

    // Row A: no limit -> natural advance, spilling into the overflow zone.
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.62f, 0.66f, 0.74f, 1.0f);
    canvas2d_set_font_size(c, 13.0f);
    canvas2d_fill_text(c, "without maxWidth", L, 40.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.93f, 0.94f, 0.98f, 1.0f);
    canvas2d_set_font_size(c, 30.0f);
    canvas2d_fill_text(c, phrase, L, 74.0f);

    // Row B: maxWidth == the marked span -> condensed in x about the left anchor.
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.62f, 0.66f, 0.74f, 1.0f);
    canvas2d_set_font_size(c, 13.0f);
    canvas2d_fill_text(c, "with maxWidth (condensed to fit)", L, 104.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.45f, 0.88f, 0.55f, 1.0f);
    canvas2d_set_font_size(c, 30.0f);
    canvas2d_fill_text_max(c, phrase, L, 138.0f, maxw);

    // The maxWidth span bracket.
    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.88f, 0.90f, 0.96f, 1.0f);
    canvas2d_set_line_width(c, 1.4f);
    canvas2d_begin_path(c);
    canvas2d_move_to(c, L, gy1 - 4.0f);
    canvas2d_line_to(c, L, gy1 + 4.0f);
    canvas2d_move_to(c, L, gy1);
    canvas2d_line_to(c, R, gy1);
    canvas2d_move_to(c, R, gy1 - 4.0f);
    canvas2d_line_to(c, R, gy1 + 4.0f);
    canvas2d_stroke(c);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.88f, 0.90f, 0.96f, 1.0f);
    canvas2d_set_font_size(c, 12.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
    canvas2d_fill_text(c, "maxWidth", (L + R) * 0.5f, gy1 + 20.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);

    save(c, "gallery/textmaxwidth.png");
}

// Hit testing: stipple a grid of sample points over a shape.  Left, isPointInPath
// on a pentagram under even-odd (the central pentagon reads as outside); right,
// isPointInStroke on a thick ring (only points within the stroke band hit).  All
// queries run first -- drawing the dots replaces the current path.
static void hittest(void) {
    struct canvas2d_context *__single c = canvas2d(480, 262, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/hittest.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 480.0f, 262.0f);

    int const step = 10, nx = 20, ny = 20;
    float const solid[1] = { 1.0f };

    // ---- isPointInPath: pentagram, even-odd ----
    star(c, 130.0f, 116.0f, 92.0f);
    canvas2d_set_line_dash(c, solid, 0);
    canvas2d_set_line_width(c, 1.3f);
    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.55f, 0.58f, 0.66f, 0.55f);
    canvas2d_stroke(c);
    bool inA[20 * 20];
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            float px = 34.0f + (float)(i * step), py = 22.0f + (float)(j * step);
            inA[j * nx + i] = canvas2d_is_point_in_path(c, px, py, CANVAS2D_EVENODD);
        }
    }
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            float px = 34.0f + (float)(i * step), py = 22.0f + (float)(j * step);
            bool in = inA[j * nx + i];
            if (in) {
                canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.30f, 0.85f, 0.70f, 1.0f);
            } else {
                canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.42f, 0.45f, 0.52f, 0.85f);
            }
            canvas2d_begin_path(c);
            canvas2d_arc(c, px, py, in ? 2.7f : 1.5f, 0.0f, TAU, false);
            canvas2d_fill(c, CANVAS2D_NONZERO);
        }
    }

    // ---- isPointInStroke: a thick ring ----
    canvas2d_begin_path(c);
    canvas2d_arc(c, 350.0f, 116.0f, 66.0f, 0.0f, TAU, false);
    canvas2d_set_line_dash(c, solid, 0);
    canvas2d_set_line_width(c, 24.0f);
    bool inB[20 * 20];
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            float px = 254.0f + (float)(i * step), py = 22.0f + (float)(j * step);
            inB[j * nx + i] = canvas2d_is_point_in_stroke(c, px, py);
        }
    }
    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.62f, 0.52f, 0.40f, 0.22f);  // faint stroke band
    canvas2d_stroke(c);
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            float px = 254.0f + (float)(i * step), py = 22.0f + (float)(j * step);
            bool const in = inB[j * nx + i];
            if (in) {
                canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.97f, 0.62f, 0.25f, 1.0f);
            } else {
                canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.42f, 0.45f, 0.52f, 0.85f);
            }
            canvas2d_begin_path(c);
            canvas2d_arc(c, px, py, in ? 2.7f : 1.5f, 0.0f, TAU, false);
            canvas2d_fill(c, CANVAS2D_NONZERO);
        }
    }

    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.80f, 0.83f, 0.90f, 1.0f);
    canvas2d_set_font_size(c, 14.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
    canvas2d_fill_text(c, "isPointInPath", 130.0f, 248.0f);
    canvas2d_fill_text(c, "isPointInStroke", 350.0f, 248.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);

    save(c, "gallery/hittest.png");
}

// createImageData + putImageData dirty-rect: build one rainbow-ring image, stamp
// it whole on the left, and on the right write only a checkerboard of dirty
// sub-rects (the same image origin, so the tiles register into one picture).
static void dirtyrect(void) {
    struct canvas2d_context *__single c = canvas2d(518, 210, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/dirtyrect.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 518.0f, 210.0f);

    int const W = 220, H = 150;
    int const Lx = 24, Ly = 24, Rx = 274, Ry = 24;

    int len = -1;
    uint8_t *img = canvas2d_create_image_data(W, H, &len);
    if (img) {
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int i = (y * W + x) * 4;
                float const dx = (float)x - (float)W * 0.5f;
                float const dy = (float)y - (float)H * 0.5f;
                float const t = sqrtf(dx * dx + dy * dy) * (1.0f / 24.0f);  // ring period
                img[i]     = (uint8_t)(255.0f * (0.5f + 0.5f * cosf(TAU * t)) + 0.5f);
                img[i + 1] = (uint8_t)(255.0f * (0.5f + 0.5f * cosf(TAU * (t + 0.33f))) + 0.5f);
                img[i + 2] = (uint8_t)(255.0f * (0.5f + 0.5f * cosf(TAU * (t + 0.66f))) + 0.5f);
                img[i + 3] = 255;
            }
        }
        // Left: the whole image in one putImageData.
        canvas2d_put_image_data(c, CANVAS2D_CS_SRGB, img, len, W, H, Lx, Ly);
        // Right: only a checkerboard of dirty sub-rects is written.
        int const tile = 22;
        for (int j = 0; j * tile < H; j++) {
            for (int i = 0; i * tile < W; i++) {
                if (((i + j) & 1) == 0) {
                    canvas2d_put_image_data_dirty(c, CANVAS2D_CS_SRGB, img, len, W, H, Rx, Ry,
                                                i * tile, j * tile, tile, tile);
                }
            }
        }
        free(img);
    }

    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.40f, 0.44f, 0.52f, 0.9f);
    canvas2d_set_line_width(c, 1.5f);
    canvas2d_stroke_rect(c, (float)Lx, (float)Ly, (float)W, (float)H);
    canvas2d_stroke_rect(c, (float)Rx, (float)Ry, (float)W, (float)H);

    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.80f, 0.83f, 0.90f, 1.0f);
    canvas2d_set_font_size(c, 14.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
    canvas2d_fill_text(c, "putImageData (full)", (float)Lx + (float)W * 0.5f, 196.0f);
    canvas2d_fill_text(c, "putImageData (dirty-rect)", (float)Rx + (float)W * 0.5f,
                     196.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);

    save(c, "gallery/dirtyrect.png");
}

// Path2D: a reusable path object, transformed at draw time.  Left, one petal path
// stamped under twelve rotations into a flower (the same object, different CTMs).
// Right, add_path composes a ring with its hole for an even-odd fill, and a star
// Path2D is stroked in the hole.
static void path2d_demo(void) {
    struct canvas2d_context *__single c = canvas2d(500, 240, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/path2d.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 500.0f, 240.0f);

    // One petal, built once around the origin, drawn under many transforms.
    struct canvas2d_path2d *__single petal = canvas2d_path2d();
    if (petal) {
        canvas2d_path2d_move_to(petal, 0.0f, 0.0f);
        canvas2d_path2d_bezier_curve_to(petal, 32.0f, -26.0f, 24.0f, -76.0f, 0.0f, -88.0f);
        canvas2d_path2d_bezier_curve_to(petal, -24.0f, -76.0f, -32.0f, -26.0f, 0.0f, 0.0f);
        canvas2d_path2d_close_path(petal);
        int const n = 12;
        for (int i = 0; i < n; i++) {
            float const t = (float)i / (float)n;
            canvas2d_save(c);
            canvas2d_translate(c, 135.0f, 116.0f);
            canvas2d_rotate(c, t * TAU);
            canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.5f + 0.5f * cosf(TAU * t),
                                 0.5f + 0.5f * cosf(TAU * (t + 0.33f)),
                                 0.5f + 0.5f * cosf(TAU * (t + 0.66f)), 0.85f);
            canvas2d_fill_path(c, petal, CANVAS2D_NONZERO);
            canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 1.0f, 1.0f, 1.0f, 0.5f);
            canvas2d_set_line_width(c, 1.2f);
            canvas2d_stroke_path(c, petal);
            canvas2d_restore(c);
        }
        canvas2d_path2d_free(petal);
    }

    // add_path: a ring and its hole composed into one path, filled even-odd.
    struct canvas2d_path2d *__single ring = canvas2d_path2d();
    struct canvas2d_path2d *__single hole = canvas2d_path2d();
    if (ring && hole) {
        canvas2d_path2d_arc(ring, 365.0f, 116.0f, 54.0f, 0.0f, TAU, false);
        canvas2d_path2d_arc(hole, 365.0f, 116.0f, 32.0f, 0.0f, TAU, false);
        canvas2d_path2d_add_path(ring, hole);
        canvas2d_set_fill_radial_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, 348.0f, 98.0f, 6.0f, 365.0f, 116.0f, 58.0f);
        canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.0f, 0.55f, 0.95f, 0.95f, 1.0f);
        canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 1.0f, 0.20f, 0.35f, 0.85f, 1.0f);
        canvas2d_fill_path(c, ring, CANVAS2D_EVENODD);
    }
    if (hole) {
        canvas2d_path2d_free(hole);
    }
    if (ring) {
        canvas2d_path2d_free(ring);
    }

    // A star Path2D, stroked in the hole.
    struct canvas2d_path2d *__single starp = canvas2d_path2d();
    if (starp) {
        for (int i = 0; i < 5; i++) {
            float const a = -TAU * 0.25f + (float)i * (TAU * 0.4f);
            float px = 365.0f + 24.0f * cosf(a), py = 116.0f + 24.0f * sinf(a);
            if (i == 0) {
                canvas2d_path2d_move_to(starp, px, py);
            } else {
                canvas2d_path2d_line_to(starp, px, py);
            }
        }
        canvas2d_path2d_close_path(starp);
        canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.97f, 0.82f, 0.30f, 1.0f);
        canvas2d_set_line_width(c, 2.5f);
        canvas2d_set_line_join(c, CANVAS2D_JOIN_ROUND);
        canvas2d_stroke_path(c, starp);
        canvas2d_path2d_free(starp);
    }

    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.80f, 0.83f, 0.90f, 1.0f);
    canvas2d_set_font_size(c, 13.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
    canvas2d_fill_text(c, "one Path2D, 12 transforms", 135.0f, 226.0f);
    canvas2d_fill_text(c, "add_path · even-odd · stroke", 365.0f, 226.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);

    save(c, "gallery/path2d.png");
}

// Paint a transparency checkerboard *behind* whatever is already in [x,y,w,h]
// (destination-over), so a Porter-Duff result's transparent areas read clearly.
static void checker_behind(struct canvas2d_context *__single c, float x, float y, float w, float h) {
    canvas2d_set_global_composite_operation(c, CANVAS2D_OP_DESTINATION_OVER);
    int const t = 13;
    for (int j = 0; (float)(j * t) < h; j++) {
        for (int i = 0; (float)(i * t) < w; i++) {
            if (((i + j) & 1) == 0) {
                canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.84f, 0.85f, 0.88f, 1.0f);
            } else {
                canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.68f, 0.70f, 0.75f, 1.0f);
            }
            canvas2d_fill_rect(c, x + (float)(i * t), y + (float)(j * t), (float)t,
                             (float)t);
        }
    }
    canvas2d_set_global_composite_operation(c, CANVAS2D_OP_SOURCE_OVER);
}

static void pd_dst(struct canvas2d_context *__single c, float ox, float oy) {  // blue square
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.20f, 0.55f, 0.90f, 1.0f);
    canvas2d_fill_rect(c, ox + 16.0f, oy + 12.0f, 50.0f, 50.0f);
}

static void pd_src(struct canvas2d_context *__single c, float ox, float oy) {  // orange disc
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.97f, 0.55f, 0.18f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_arc(c, ox + 62.0f, oy + 52.0f, 27.0f, 0.0f, TAU, false);
    canvas2d_fill(c, CANVAS2D_NONZERO);
}

// The 11 Porter-Duff compositing operators (how source alpha combines with the
// destination), each in its own cell: clip + clear to transparent, draw the blue
// "destination" square, then the orange "source" disc under the operator, then a
// checkerboard behind so the surviving regions read.  A legend names the shapes.
static void porterduff(void) {
    float const M = 16.0f, cellW = 118.0f, cellH = 99.0f;
    struct canvas2d_context *__single c = canvas2d(504, 329, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/porterduff.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 504.0f, 329.0f);

    enum canvas2d_composite_op const ops[11] = {
        CANVAS2D_OP_SOURCE_OVER,      CANVAS2D_OP_SOURCE_IN,
        CANVAS2D_OP_SOURCE_OUT,       CANVAS2D_OP_SOURCE_ATOP,
        CANVAS2D_OP_DESTINATION_OVER, CANVAS2D_OP_DESTINATION_IN,
        CANVAS2D_OP_DESTINATION_OUT,  CANVAS2D_OP_DESTINATION_ATOP,
        CANVAS2D_OP_LIGHTER,          CANVAS2D_OP_XOR,
        CANVAS2D_OP_COPY,
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
        canvas2d_save(c);
        canvas2d_begin_path(c);
        canvas2d_rect(c, ox + 3.0f, oy + 3.0f, 112.0f, 78.0f);
        canvas2d_clip(c, CANVAS2D_NONZERO);
        canvas2d_clear_rect(c, ox + 3.0f, oy + 3.0f, 112.0f, 78.0f);
        pd_dst(c, ox, oy);  // destination (source-over)
        canvas2d_set_global_composite_operation(c, ops[idx]);
        pd_src(c, ox, oy);  // source under the operator
        checker_behind(c, ox + 3.0f, oy + 3.0f, 112.0f, 78.0f);
        canvas2d_restore(c);

        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.80f, 0.83f, 0.90f, 1.0f);
        canvas2d_set_font_size(c, 11.0f);
        canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
        canvas2d_fill_text(c, names[idx], ox + cellW * 0.5f, oy + 93.0f);
    }

    // Legend cell (bottom-right): the two shapes and what they are.
    float lox = M + 3.0f * cellW, loy = M + 2.0f * cellH;
    pd_dst(c, lox, loy - 4.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.20f, 0.55f, 0.90f, 1.0f);
    canvas2d_fill_rect(c, lox + 20.0f, loy + 14.0f, 30.0f, 22.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.85f, 0.88f, 0.93f, 1.0f);
    canvas2d_set_font_size(c, 12.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_LEFT);
    canvas2d_fill_text(c, "destination", lox + 56.0f, loy + 30.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.97f, 0.55f, 0.18f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_arc(c, lox + 35.0f, loy + 58.0f, 13.0f, 0.0f, TAU, false);
    canvas2d_fill(c, CANVAS2D_NONZERO);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.85f, 0.88f, 0.93f, 1.0f);
    canvas2d_fill_text(c, "source", lox + 56.0f, loy + 62.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);

    save(c, "gallery/porterduff.png");
}

// drawImage subrect: build a sprite atlas on a scratch canvas, read it back to
// RGBA8, then pull individual tiles out of it with the source-rect overload --
// the atlas on the left, four tiles extracted and enlarged on the right.
static void subrect(void) {
    int const AW = 160, AH = 80;  // 4x2 tiles of 40px
    uint8_t atlas[160 * 80 * 4];
    struct canvas2d_context *__single ac = canvas2d(AW, AH, CANVAS2D_CS_SRGB);
    if (ac) {
        for (int k = 0; k < 8; k++) {
            int tx = k % 4, ty = k / 4;
            float ox = (float)(tx * 40), oy = (float)(ty * 40);
            float cx = ox + 20.0f, cy = oy + 20.0f;
            float const t = (float)k / 8.0f;
            canvas2d_set_fill_rgba(ac, CANVAS2D_CS_SRGB, 0.5f + 0.5f * cosf(TAU * t),
                                 0.5f + 0.5f * cosf(TAU * (t + 0.33f)),
                                 0.5f + 0.5f * cosf(TAU * (t + 0.66f)), 1.0f);
            canvas2d_fill_rect(ac, ox, oy, 40.0f, 40.0f);
            canvas2d_set_fill_rgba(ac, CANVAS2D_CS_SRGB, 0.98f, 0.99f, 1.0f, 0.92f);
            switch (k % 4) {
                case 0:  // circle
                    canvas2d_begin_path(ac);
                    canvas2d_arc(ac, cx, cy, 14.0f, 0.0f, TAU, false);
                    canvas2d_fill(ac, CANVAS2D_NONZERO);
                    break;
                case 1:  // square
                    canvas2d_fill_rect(ac, cx - 13.0f, cy - 13.0f, 26.0f, 26.0f);
                    break;
                case 2:  // triangle
                    canvas2d_begin_path(ac);
                    canvas2d_move_to(ac, cx, cy - 15.0f);
                    canvas2d_line_to(ac, cx + 14.0f, cy + 12.0f);
                    canvas2d_line_to(ac, cx - 14.0f, cy + 12.0f);
                    canvas2d_close_path(ac);
                    canvas2d_fill(ac, CANVAS2D_NONZERO);
                    break;
                default:  // star
                    star(ac, cx, cy, 16.0f);
                    canvas2d_fill(ac, CANVAS2D_NONZERO);
                    break;
            }
        }
        canvas2d_read_rgba(ac, CANVAS2D_CS_SRGB, atlas, AW * AH * 4);
        canvas2d_free(ac);
    }

    struct canvas2d_context *__single c = canvas2d(468, 196, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/subrect.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 468.0f, 196.0f);

    // Left: the whole atlas at 1.5x, with a grid showing the tile cells.
    canvas2d_draw_bitmap_scaled(c, CANVAS2D_CS_SRGB, atlas, AW, AH, 20.0f, 30.0f, 240.0f, 120.0f);
    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.20f, 0.22f, 0.28f, 0.9f);
    canvas2d_set_line_width(c, 1.0f);
    for (int i = 1; i < 4; i++) {
        canvas2d_begin_path(c);
        canvas2d_move_to(c, 20.0f + (float)i * 60.0f, 30.0f);
        canvas2d_line_to(c, 20.0f + (float)i * 60.0f, 150.0f);
        canvas2d_stroke(c);
    }
    canvas2d_begin_path(c);
    canvas2d_move_to(c, 20.0f, 90.0f);
    canvas2d_line_to(c, 260.0f, 90.0f);
    canvas2d_stroke(c);

    // Right: four chosen tiles pulled out via draw_image_subrect, enlarged 2x2.
    int const pick[4] = { 1, 6, 3, 4 };
    float const ts = 66.0f, gap = 8.0f, dx0 = 300.0f, dy0 = 30.0f;
    for (int m = 0; m < 4; m++) {
        int k = pick[m], tx = k % 4, ty = k / 4;
        float const dx = dx0 + (float)(m % 2) * (ts + gap);
        float const dy = dy0 + (float)(m / 2) * (ts + gap);
        canvas2d_draw_bitmap_subrect(c, CANVAS2D_CS_SRGB, atlas, AW, AH, (float)(tx * 40),
                                  (float)(ty * 40), 40.0f, 40.0f, dx, dy, ts, ts);
    }

    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.80f, 0.83f, 0.90f, 1.0f);
    canvas2d_set_font_size(c, 14.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
    canvas2d_fill_text(c, "sprite atlas", 140.0f, 178.0f);
    canvas2d_fill_text(c, "pulled via subrect", 370.0f, 178.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);

    save(c, "gallery/subrect.png");
}

// One motif (a teal square with a gold "F") centred at the origin; the caller's
// transform deforms it.  "F" has no symmetry, so reflection and shear read at a
// glance.
static void affine_motif(struct canvas2d_context *__single c) {
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.25f, 0.70f, 0.78f, 0.85f);
    canvas2d_fill_rect(c, -32.0f, -32.0f, 64.0f, 64.0f);
    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.95f, 0.97f, 1.0f, 0.95f);
    canvas2d_set_line_width(c, 2.0f);
    canvas2d_stroke_rect(c, -32.0f, -32.0f, 64.0f, 64.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.99f, 0.85f, 0.30f, 1.0f);
    canvas2d_set_font_size(c, 54.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
    canvas2d_set_text_baseline(c, CANVAS2D_BASELINE_MIDDLE);
    canvas2d_fill_text(c, "F", 0.0f, 0.0f);
}

// canvas2d_transform: arbitrary affine matrices beyond translate/rotate/scale.  Each
// cell applies one matrix to the motif (solid), with the identity footprint behind
// it (dashed), so the shear / scale / reflection is visible.
static void affine(void) {
    float const M = 14.0f, cellW = 150.0f, cellH = 122.0f;
    struct canvas2d_context *__single c = canvas2d(478, 272, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/affine.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 478.0f, 272.0f);

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
        canvas2d_save(c);
        canvas2d_translate(c, ox + 75.0f, oy + 52.0f);
        // Identity footprint (dashed), drawn before the matrix is applied.
        canvas2d_set_line_dash(c, dash, 2);
        canvas2d_set_line_width(c, 1.2f);
        canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.45f, 0.48f, 0.55f, 0.85f);
        canvas2d_stroke_rect(c, -32.0f, -32.0f, 64.0f, 64.0f);
        canvas2d_set_line_dash(c, solid, 0);
        // Apply the matrix, draw the motif deformed.
        canvas2d_transform(c, mat[i].a, mat[i].b, mat[i].c, mat[i].d, 0.0f, 0.0f);
        affine_motif(c);
        canvas2d_restore(c);

        canvas2d_set_text_baseline(c, CANVAS2D_BASELINE_ALPHABETIC);
        canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.80f, 0.83f, 0.90f, 1.0f);
        canvas2d_set_font_size(c, 12.0f);
        canvas2d_fill_text(c, mat[i].name, ox + 75.0f, oy + 108.0f);
    }
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);

    save(c, "gallery/affine.png");
}

// miterLimit and lineDashOffset.  Top: one sharp V stroked at four miter limits --
// below the spike's ratio (~4.7) the join falls back to a bevel, above it the
// miter spike survives.  Bottom: one dash pattern at five offsets, the phase
// marching left (a frozen marching-ants animation).
static void miterdash(void) {
    struct canvas2d_context *__single c = canvas2d(480, 258, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/miterdash.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 480.0f, 258.0f);

    canvas2d_set_text_align(c, CANVAS2D_ALIGN_LEFT);
    canvas2d_set_text_baseline(c, CANVAS2D_BASELINE_ALPHABETIC);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.62f, 0.66f, 0.74f, 1.0f);
    canvas2d_set_font_size(c, 13.0f);
    canvas2d_fill_text(c, "miterLimit", 16.0f, 28.0f);

    // Four identical sharp Vs (miter ratio ~4.7), at increasing miter limits.
    float const solid[1] = { 1.0f };
    float const limits[4] = { 2.0f, 4.0f, 6.0f, 10.0f };
    char const *const mlabel[4] = { "limit 2", "limit 4", "limit 6", "limit 10" };
    canvas2d_set_line_dash(c, solid, 0);
    canvas2d_set_line_join(c, CANVAS2D_JOIN_MITER);
    canvas2d_set_line_cap(c, CANVAS2D_CAP_BUTT);
    canvas2d_set_line_width(c, 14.0f);
    for (int i = 0; i < 4; i++) {
        float const cx = 90.0f + (float)i * 100.0f;
        canvas2d_set_miter_limit(c, limits[i]);
        canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.95f, 0.55f, 0.30f, 1.0f);
        canvas2d_begin_path(c);
        canvas2d_move_to(c, cx - 16.0f, 44.0f);
        canvas2d_line_to(c, cx, 112.0f);
        canvas2d_line_to(c, cx + 16.0f, 44.0f);
        canvas2d_stroke(c);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.80f, 0.83f, 0.90f, 1.0f);
        canvas2d_set_font_size(c, 12.0f);
        canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
        canvas2d_fill_text(c, mlabel[i], cx, 164.0f);
    }
    canvas2d_set_miter_limit(c, 10.0f);  // restore default

    canvas2d_set_text_align(c, CANVAS2D_ALIGN_LEFT);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.62f, 0.66f, 0.74f, 1.0f);
    canvas2d_set_font_size(c, 13.0f);
    canvas2d_fill_text(c, "lineDashOffset", 16.0f, 186.0f);

    // One dash pattern, five offsets: the phase marches across the rows.
    float const dash[2] = { 18.0f, 10.0f };
    float const offs[5] = { 0.0f, 6.0f, 12.0f, 18.0f, 24.0f };
    char const *const olabel[5] = { "0", "6", "12", "18", "24" };
    canvas2d_set_line_dash(c, dash, 2);
    canvas2d_set_line_width(c, 6.0f);
    canvas2d_set_line_cap(c, CANVAS2D_CAP_BUTT);
    for (int i = 0; i < 5; i++) {
        float const y = 200.0f + (float)i * 12.0f;
        canvas2d_set_line_dash_offset(c, offs[i]);
        canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.40f, 0.82f, 0.95f, 1.0f);
        canvas2d_begin_path(c);
        canvas2d_move_to(c, 80.0f, y);
        canvas2d_line_to(c, 462.0f, y);
        canvas2d_stroke(c);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.62f, 0.66f, 0.74f, 1.0f);
        canvas2d_set_font_size(c, 11.0f);
        canvas2d_set_text_align(c, CANVAS2D_ALIGN_RIGHT);
        canvas2d_fill_text(c, olabel[i], 68.0f, y + 4.0f);
    }
    canvas2d_set_line_dash(c, solid, 0);
    canvas2d_set_line_dash_offset(c, 0.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);

    save(c, "gallery/miterdash.png");
}

// Flood a box with rainbow stripes; only what falls inside the active clip
// survives, so the stripes trace out the clip shape.
static void clip_stripes(struct canvas2d_context *__single c, float x0, float y0, float x1, float y1) {
    int const n = 16;
    float const bw = (x1 - x0) / (float)n;
    for (int i = 0; i < n; i++) {
        float const t = (float)i / (float)(n - 1);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.5f + 0.5f * cosf(TAU * t),
                             0.5f + 0.5f * cosf(TAU * (t + 0.33f)),
                             0.5f + 0.5f * cosf(TAU * (t + 0.66f)), 1.0f);
        canvas2d_fill_rect(c, x0 + (float)i * bw, y0, bw + 1.0f, y1 - y0);
    }
}

// Clipping: a circular window, the intersection of two discs, and a
// self-intersecting star window — each masking the same flood of stripes.
static void clipping(void) {
    struct canvas2d_context *__single c = canvas2d(300, 120, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/clip.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 300.0f, 120.0f);

    canvas2d_save(c);
    canvas2d_begin_path(c);
    canvas2d_arc(c, 55.0f, 60.0f, 40.0f, 0.0f, TAU, false);
    canvas2d_clip(c, CANVAS2D_NONZERO);
    clip_stripes(c, 15.0f, 20.0f, 95.0f, 100.0f);
    canvas2d_restore(c);

    canvas2d_save(c);
    canvas2d_begin_path(c);
    canvas2d_arc(c, 135.0f, 60.0f, 38.0f, 0.0f, TAU, false);
    canvas2d_clip(c, CANVAS2D_NONZERO);
    canvas2d_begin_path(c);
    canvas2d_arc(c, 170.0f, 60.0f, 38.0f, 0.0f, TAU, false);
    canvas2d_clip(c, CANVAS2D_NONZERO);  // intersect: only the lens overlap survives
    clip_stripes(c, 95.0f, 20.0f, 210.0f, 100.0f);
    canvas2d_restore(c);

    canvas2d_save(c);
    star(c, 250.0f, 60.0f, 42.0f);
    canvas2d_clip(c, CANVAS2D_NONZERO);
    clip_stripes(c, 205.0f, 15.0f, 295.0f, 105.0f);
    canvas2d_restore(c);

    save(c, "gallery/clip.png");
}

// Gradients: a diagonal linear fill (outlined with a gradient stroke), an
// off-centre radial "sphere", and a multi-stop rainbow ramp.
static void gradients(void) {
    struct canvas2d_context *__single c = canvas2d(300, 120, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/gradients.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 300.0f, 120.0f);

    canvas2d_set_fill_linear_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, 20.0f, 20.0f, 100.0f, 100.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.0f, 0.99f, 0.80f, 0.26f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 1.0f, 0.90f, 0.22f, 0.42f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_round_rect(c, 20.0f, 20.0f, 80.0f, 80.0f, 16.0f);
    canvas2d_fill(c, CANVAS2D_NONZERO);
    // Outline it with a contrasting gradient stroke (cyan -> yellow, diagonal).
    canvas2d_set_stroke_linear_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, 20.0f, 20.0f, 100.0f, 100.0f);
    canvas2d_add_stroke_color_stop(c, CANVAS2D_CS_SRGB, 0.0f, 0.20f, 0.90f, 0.95f, 1.0f);
    canvas2d_add_stroke_color_stop(c, CANVAS2D_CS_SRGB, 1.0f, 0.95f, 0.95f, 0.20f, 1.0f);
    canvas2d_set_line_width(c, 5.0f);
    canvas2d_begin_path(c);
    canvas2d_round_rect(c, 20.0f, 20.0f, 80.0f, 80.0f, 16.0f);
    canvas2d_stroke(c);

    canvas2d_set_fill_radial_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, 140.0f, 46.0f, 3.0f, 150.0f, 60.0f, 44.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.4f, 0.30f, 0.65f, 0.95f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 1.0f, 0.05f, 0.10f, 0.35f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_arc(c, 150.0f, 60.0f, 44.0f, 0.0f, TAU, false);
    canvas2d_fill(c, CANVAS2D_NONZERO);

    canvas2d_set_fill_linear_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, 205.0f, 0.0f, 285.0f, 0.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.00f, 0.90f, 0.20f, 0.25f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.33f, 0.95f, 0.80f, 0.25f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.66f, 0.30f, 0.80f, 0.45f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 1.00f, 0.30f, 0.45f, 0.95f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_round_rect(c, 205.0f, 25.0f, 75.0f, 70.0f, 12.0f);
    canvas2d_fill(c, CANVAS2D_NONZERO);

    save(c, "gallery/gradients.png");
}

// A dense field of translucent discs, each its own canvas2d_fill, composited in
// order onto the shared target (the alpha overlap shows ordering is preserved).
static uint32_t batch_rng = 0x1234567u;

static float batch_rand(void) {
    batch_rng = batch_rng * 1664525u + 1013904223u;
    return (float)(batch_rng >> 8) / 16777216.0f;  // [0,1)
}

static void batching(void) {
    batch_rng = 0x1234567u;  // reseed so the scene is reproducible regardless of how
                             // many times it runs (GALLERY_REPS calls it repeatedly)
    struct canvas2d_context *__single c = canvas2d(300, 120, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/batch.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.07f, 0.08f, 0.10f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 300.0f, 120.0f);

    canvas2d_set_global_alpha(c, 0.55f);
    for (int i = 0; i < 320; i++) {
        float const x = batch_rand() * 300.0f;
        float const y = batch_rand() * 120.0f;
        float const r = 3.0f + batch_rand() * 9.0f;
        float const t = batch_rand();
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.5f + 0.5f * cosf(TAU * t),
                             0.5f + 0.5f * cosf(TAU * (t + 0.33f)),
                             0.5f + 0.5f * cosf(TAU * (t + 0.66f)), 1.0f);
        canvas2d_begin_path(c);
        canvas2d_arc(c, x, y, r, 0.0f, TAU, false);
        canvas2d_fill(c, CANVAS2D_NONZERO);
    }

    save(c, "gallery/batch.png");
}

// drawImage: a small procedural source, drawn 1:1 (crisp), scaled up (bilinear
// smoothing), and scaled + rotated through the transform.
static void drawimage(void) {
    struct canvas2d_context *__single c = canvas2d(300, 120, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/drawimage.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 300.0f, 120.0f);

    // A 16x16 multi-hue source image (tightly packed RGBA8).
    uint8_t img[16 * 16 * 4];
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            int const i = (y * 16 + x) * 4;
            img[i] = (uint8_t)(255.0f * (0.5f + 0.5f * cosf(TAU * (float)x / 16.0f)));
            img[i + 1] = (uint8_t)(255.0f * (0.5f + 0.5f * cosf(TAU * (float)y / 16.0f)));
            img[i + 2] = (uint8_t)(255.0f * (0.5f + 0.5f * cosf(TAU * (float)(x + y) / 16.0f)));
            img[i + 3] = 255;
        }
    }

    canvas2d_draw_bitmap(c, CANVAS2D_CS_SRGB, img, 16, 16, 20.0f, 20.0f);                       // 1:1
    canvas2d_draw_bitmap_scaled(c, CANVAS2D_CS_SRGB, img, 16, 16, 50.0f, 20.0f, 80.0f, 80.0f);  // bilinear
    canvas2d_save(c);
    canvas2d_translate(c, 235.0f, 60.0f);
    canvas2d_rotate(c, 0.5f);
    canvas2d_draw_bitmap_scaled(c, CANVAS2D_CS_SRGB, img, 16, 16, -40.0f, -40.0f, 80.0f, 80.0f);  // rotated
    canvas2d_restore(c);

    save(c, "gallery/drawimage.png");
}

// Text: Libian TC glyph outlines rasterized by the same coverage fill as the
// shapes, so they take gradients, strokes, transforms, and alpha; one fill_text
// mixes Latin and Chinese.
static void text(void) {
    struct canvas2d_context *__single c = canvas2d(420, 170, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/text.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 420.0f, 170.0f);

    // Gradient-filled headline: Latin + Chinese in a single fill_text.
    canvas2d_set_fill_linear_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, 20.0f, 0.0f, 390.0f, 0.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.00f, 0.99f, 0.80f, 0.26f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.50f, 0.95f, 0.35f, 0.45f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 1.00f, 0.35f, 0.55f, 0.98f, 1.0f);
    canvas2d_set_font_size(c, 52.0f);
    canvas2d_fill_text(c, "canvas2d 畫布", 20.0f, 70.0f);

    // Stroked subtitle, also mixing scripts.
    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.85f, 0.88f, 0.95f, 1.0f);
    canvas2d_set_line_width(c, 1.2f);
    canvas2d_set_line_join(c, CANVAS2D_JOIN_ROUND);
    canvas2d_set_font_size(c, 26.0f);
    canvas2d_stroke_text(c, "隸書 · Libian TC, in C", 22.0f, 116.0f);

    // Transformed (rotated) text shows the CTM applies to glyphs too.
    canvas2d_save(c);
    canvas2d_translate(c, 352.0f, 152.0f);
    canvas2d_rotate(c, -0.32f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.45f, 0.85f, 0.55f, 1.0f);
    canvas2d_set_font_size(c, 26.0f);
    canvas2d_fill_text(c, "你好!", 0.0f, 0.0f);
    canvas2d_restore(c);

    save(c, "gallery/text.png");
}

// letterSpacing / wordSpacing: the same line drawn three ways -- default
// (no spacing), positive letterSpacing (extra advance after each cluster), and
// positive wordSpacing (extra advance at each space).  The spacing is baked into
// the shaped advances, so it widens the line and the spaces visibly.
static void textspacing(void) {
    struct canvas2d_context *__single c = canvas2d(420, 160, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/textspacing.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 420.0f, 160.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.92f, 0.93f, 0.96f, 1.0f);
    canvas2d_set_font_size(c, 26.0f);

    char const *__null_terminated line = "spaced out text";

    // Default: no spacing.
    canvas2d_fill_text(c, line, 20.0f, 45.0f);

    // Positive letterSpacing: extra advance after every cluster.
    canvas2d_set_letter_spacing(c, 4.0f);
    canvas2d_fill_text(c, line, 20.0f, 90.0f);
    canvas2d_set_letter_spacing(c, 0.0f);

    // Positive wordSpacing: extra advance at each space.
    canvas2d_set_word_spacing(c, 16.0f);
    canvas2d_fill_text(c, line, 20.0f, 135.0f);
    canvas2d_set_word_spacing(c, 0.0f);

    save(c, "gallery/textspacing.png");
}

// globalCompositeOperation: all fifteen blend modes -- the eleven separable plus
// the four non-separable -- each compositing two overlapping discs over the same
// diagonal gradient backdrop (the W3C composite+blend formula, the checked-C blend
// kernel).
static void blend(void) {
    struct { enum canvas2d_composite_op op; char const *name; } const cell[15] = {
        { CANVAS2D_OP_MULTIPLY, "multiply" },       { CANVAS2D_OP_SCREEN, "screen" },
        { CANVAS2D_OP_OVERLAY, "overlay" },         { CANVAS2D_OP_DARKEN, "darken" },
        { CANVAS2D_OP_LIGHTEN, "lighten" },         { CANVAS2D_OP_COLOR_DODGE, "color-dodge" },
        { CANVAS2D_OP_COLOR_BURN, "color-burn" },   { CANVAS2D_OP_HARD_LIGHT, "hard-light" },
        { CANVAS2D_OP_SOFT_LIGHT, "soft-light" },   { CANVAS2D_OP_DIFFERENCE, "difference" },
        { CANVAS2D_OP_EXCLUSION, "exclusion" },     { CANVAS2D_OP_HUE, "hue" },
        { CANVAS2D_OP_SATURATION, "saturation" },   { CANVAS2D_OP_COLOR, "color" },
        { CANVAS2D_OP_LUMINOSITY, "luminosity" },
    };
    float const M = 12.0f, cellW = 140.0f, cellH = 124.0f;
    struct canvas2d_context *__single c = canvas2d(724, 396, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/blend.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.13f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 724.0f, 396.0f);

    for (int i = 0; i < 15; i++) {
        int col = i % 5, row = i / 5;
        float ox = M + (float)col * cellW, oy = M + (float)row * cellH;

        // Backdrop: a diagonal gradient block (always source-over).
        canvas2d_set_global_composite_operation(c, CANVAS2D_OP_SOURCE_OVER);
        canvas2d_set_fill_linear_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, ox + 18.0f, oy + 12.0f, ox + 122.0f,
                                        oy + 100.0f);
        canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.0f, 0.20f, 0.65f, 0.95f, 1.0f);
        canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 1.0f, 0.98f, 0.85f, 0.30f, 1.0f);
        canvas2d_begin_path(c);
        canvas2d_round_rect(c, ox + 18.0f, oy + 12.0f, 104.0f, 88.0f, 12.0f);
        canvas2d_fill(c, CANVAS2D_NONZERO);

        // Two overlapping discs under this cell's blend mode.
        canvas2d_set_global_composite_operation(c, cell[i].op);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.92f, 0.26f, 0.21f, 1.0f);
        canvas2d_begin_path(c);
        canvas2d_arc(c, ox + 54.0f, oy + 42.0f, 26.0f, 0.0f, TAU, false);
        canvas2d_fill(c, CANVAS2D_NONZERO);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.18f, 0.85f, 0.42f, 1.0f);
        canvas2d_begin_path(c);
        canvas2d_arc(c, ox + 80.0f, oy + 66.0f, 26.0f, 0.0f, TAU, false);
        canvas2d_fill(c, CANVAS2D_NONZERO);

        // Label.
        canvas2d_set_global_composite_operation(c, CANVAS2D_OP_SOURCE_OVER);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.90f, 0.92f, 0.96f, 1.0f);
        canvas2d_set_font_size(c, 13.0f);
        canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
        canvas2d_fill_text(c, cell[i].name, ox + cellW * 0.5f, oy + 116.0f);
    }
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);

    save(c, "gallery/blend.png");
}

// Shadows: a sharp drop shadow, a soft blurred shadow, and a text shadow.  Each
// is the op's alpha blurred (the in-tree box blur), tinted, offset, and
// composited under the shape.  The bottom row steps one
// sharp shadow across quarter-pixel offsets: subpixel placement lands on a
// 1/256th-px grid (a 2-tap lerp), so the edge ramps instead of snapping a
// whole column at a time.
static void shadows(void) {
    struct canvas2d_context *__single c = canvas2d(400, 185, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/shadows.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.93f, 0.94f, 0.96f, 1.0f);  // light ground
    canvas2d_fill_rect(c, 0.0f, 0.0f, 400.0f, 185.0f);

    // Sharp offset drop shadow under a rounded rectangle.
    canvas2d_set_shadow_color_rgba(c, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 0.45f);
    canvas2d_set_shadow_blur(c, 0.0f);
    canvas2d_set_shadow_offset_x(c, 7.0f);
    canvas2d_set_shadow_offset_y(c, 7.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.92f, 0.30f, 0.34f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_round_rect(c, 30.0f, 32.0f, 72.0f, 62.0f, 12.0f);
    canvas2d_fill(c, CANVAS2D_NONZERO);

    // Soft, blurred shadow under a disc.
    canvas2d_set_shadow_color_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.20f, 0.45f, 0.8f);
    canvas2d_set_shadow_blur(c, 16.0f);
    canvas2d_set_shadow_offset_x(c, 0.0f);
    canvas2d_set_shadow_offset_y(c, 5.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.35f, 0.70f, 0.95f, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_arc(c, 185.0f, 60.0f, 34.0f, 0.0f, TAU, false);
    canvas2d_fill(c, CANVAS2D_NONZERO);

    // Text shadow.
    canvas2d_set_shadow_color_rgba(c, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 0.5f);
    canvas2d_set_shadow_blur(c, 3.0f);
    canvas2d_set_shadow_offset_x(c, 2.0f);
    canvas2d_set_shadow_offset_y(c, 3.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.15f, 0.55f, 0.35f, 1.0f);
    canvas2d_set_font_size(c, 34.0f);
    canvas2d_fill_text(c, "shadow", 250.0f, 78.0f);

    // Subpixel offsets: the same sharp shadow at offsets 6.0 / 6.25 / 6.5 /
    // 6.75 / 7.0 (both axes).  The leading edges ramp smoothly across the
    // quarter-pixel steps -- whole-pixel snapping would make the first four
    // identical and the last jump a full column.
    canvas2d_set_shadow_color_rgba(c, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 0.55f);
    canvas2d_set_shadow_blur(c, 0.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.45f, 0.40f, 0.85f, 1.0f);
    for (int i = 0; i < 5; i++) {
        float const off = 6.0f + 0.25f * (float)i;
        canvas2d_set_shadow_offset_x(c, off);
        canvas2d_set_shadow_offset_y(c, off);
        canvas2d_fill_rect(c, 36.0f + 64.0f * (float)i, 138.0f, 24.0f, 24.0f);
    }

    save(c, "gallery/shadows.png");
}

// Color emoji: Core Text falls back to AppleColorEmoji and the run's color glyphs
// are drawn as RGBA8 bitmaps (the second text boundary), composited like any other
// paint -- so emoji mix inline with text and take the transform and shadow.
static void emoji(void) {
    struct canvas2d_context *__single c = canvas2d(520, 250, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/emoji.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.94f, 0.94f, 0.96f, 1.0f);  // light ground (shows shadows)
    canvas2d_fill_rect(c, 0.0f, 0.0f, 520.0f, 250.0f);

    canvas2d_set_text_align(c, CANVAS2D_ALIGN_LEFT);
    canvas2d_set_text_baseline(c, CANVAS2D_BASELINE_ALPHABETIC);

    // Inline with Latin + Chinese in a single fill_text.
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.16f, 0.17f, 0.22f, 1.0f);
    canvas2d_set_font_size(c, 30.0f);
    canvas2d_fill_text(c, "canvas2d 🎨 畫布", 26.0f, 50.0f);

    // A row of color-bitmap emoji.
    canvas2d_set_font_size(c, 46.0f);
    canvas2d_fill_text(c, "🌈🚀🌸🍕🐙⭐🎉🍎", 26.0f, 122.0f);

    // Emoji take the pipeline: a drop shadow, a rotation, a larger scale.
    float const cxs[3] = { 95.0f, 260.0f, 425.0f };
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
    canvas2d_set_text_baseline(c, CANVAS2D_BASELINE_MIDDLE);

    canvas2d_set_shadow_color_rgba(c, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 0.55f);
    canvas2d_set_shadow_blur(c, 6.0f);
    canvas2d_set_shadow_offset_x(c, 3.0f);
    canvas2d_set_shadow_offset_y(c, 4.0f);
    canvas2d_set_font_size(c, 52.0f);
    canvas2d_fill_text(c, "🎨", cxs[0], 186.0f);
    canvas2d_set_shadow_color_rgba(c, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 0.0f);  // disable
    canvas2d_set_shadow_blur(c, 0.0f);
    canvas2d_set_shadow_offset_x(c, 0.0f);
    canvas2d_set_shadow_offset_y(c, 0.0f);

    canvas2d_save(c);
    canvas2d_translate(c, cxs[1], 186.0f);
    canvas2d_rotate(c, -0.38f);
    canvas2d_set_font_size(c, 52.0f);
    canvas2d_fill_text(c, "🚀", 0.0f, 0.0f);
    canvas2d_restore(c);

    canvas2d_set_font_size(c, 70.0f);
    canvas2d_fill_text(c, "🌈", cxs[2], 186.0f);

    canvas2d_set_text_baseline(c, CANVAS2D_BASELINE_ALPHABETIC);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.40f, 0.43f, 0.50f, 1.0f);
    canvas2d_set_font_size(c, 13.0f);
    char const *const labels[3] = { "shadow", "rotate", "scale" };
    for (int i = 0; i < 3; i++) {
        canvas2d_fill_text(c, labels[i], cxs[i], 232.0f);
    }
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);

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
    struct canvas2d_context *__single c = canvas2d(700, 600, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/emojiscale.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.94f, 0.94f, 0.96f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 700.0f, 600.0f);

    canvas2d_set_text_align(c, CANVAS2D_ALIGN_LEFT);
    canvas2d_set_text_baseline(c, CANVAS2D_BASELINE_ALPHABETIC);

    int const n = 12;
    float const s0 = 8.0f, s1 = 200.0f;
    canvas2d_set_global_alpha(c, 0.8f);
    float x = 14.0f;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)(n - 1);
        float size = s0 * powf(s1 / s0, t);
        canvas2d_set_font_size(c, size);
        canvas2d_fill_text(c, "🚀", x, 240.0f);
        x += size * 0.72f;  // tighter than the advance: the overlaps blend
    }
    x = 14.0f;
    for (int i = 0; i < n; i++) {
        float const t = (float)i / (float)(n - 1);
        float const size = s0 * powf(s1 / s0, t);
        canvas2d_save(c);
        canvas2d_translate(c, x, 545.0f);
        canvas2d_rotate(c, -0.11f * (float)i);
        canvas2d_set_font_size(c, size);
        canvas2d_fill_text(c, "🚀", 0.0f, 0.0f);
        canvas2d_restore(c);
        x += size * 0.72f;
    }
    canvas2d_set_global_alpha(c, 1.0f);

    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.40f, 0.43f, 0.50f, 1.0f);
    canvas2d_set_font_size(c, 13.0f);
    canvas2d_fill_text(c, "8px → 200px, geometric; canonical capture 160px", 14.0f, 268.0f);
    canvas2d_fill_text(c, "the same ramp, progressively rotated", 14.0f, 580.0f);

    save(c, "gallery/emojiscale.png");
}

// What the BT.2100 (16-bit Rec.2020 / PQ) output enables, on a linear canvas
// where extended values survive: a shallow gradient that stays smooth at 16-bit
// over a simulated 8-bit version of the same ramp (which bands); saturated
// colours past the sRGB gamut (authored in Rec.2020, converted to extended
// linear sRGB); and fills brighter than sRGB white (linear values above 1.0,
// which PQ carries as HDR highlights).  The wide-gamut and brighter-than-white
// rows need a wide-gamut HDR display; on sRGB / SDR they gamut-map and clip.  The
// program carries a `working_space linear` line and replays onto a linear canvas.
static void extendedrange(void) {
    int const w = 480, h = 600;
    struct canvas2d_context *__single c = canvas2d(w, h, CANVAS2D_CS_LINEAR_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/extendedrange.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_LEFT);
    canvas2d_set_text_baseline(c, CANVAS2D_BASELINE_ALPHABETIC);
    canvas2d_set_font_size(c, 13.0f);

    float const lx = 20.0f, gw = 440.0f;

    // A. Precision: the same shallow grey ramp quantized to the sRGB 8-bit grid
    // (top, banded) over full 16-bit (bottom, smooth) -- legacy on top, new on
    // bottom, to parallel the wide-gamut row.  The 8-bit row draws one bar per
    // distinct output byte (the staircase the old 8-bit PNG would have shown); the
    // 16-bit row is a linear gradient.
    float const lo = 0.045f, hi = 0.062f;  // shallow: ~11 byte levels across gw
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.70f, 0.74f, 0.82f, 1.0f);
    canvas2d_fill_text(c, "shallow gradient: 8-bit (top, banded) vs 16-bit (bottom, smooth)",
                     lx, 28.0f);
    canvas2d_set_fill_linear_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, lx, 0.0f, lx + gw, 0.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_LINEAR_SRGB, 0.0f, lo, lo, lo, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_LINEAR_SRGB, 1.0f, hi, hi, hi, 1.0f);
    canvas2d_fill_rect(c, lx, 70.0f, gw, 26.0f);

    int const gwi = (int)gw;
    int prev = (int)(canvas2d_linear_to_srgb(lo) * 255.0f + 0.5f);
    float band_x0 = lx;
    for (int px = 1; px <= gwi; px++) {
        float const t = (float)px / gw;
        int const byte = (int)(canvas2d_linear_to_srgb(lo + (hi - lo) * t) * 255.0f + 0.5f);
        if (byte != prev || px == gwi) {
            float const q = (float)prev / 255.0f;
            canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, q, q, q, 1.0f);
            canvas2d_fill_rect(c, band_x0, 40.0f, (lx + (float)px) - band_x0, 26.0f);
            band_x0 = lx + (float)px;
            prev = byte;
        }
    }

    // B. Wide gamut: each hue's sRGB primary (top) over its Rec.2020 primary
    // (bottom), the latter authored in Rec.2020 -> extended linear sRGB.
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.70f, 0.74f, 0.82f, 1.0f);
    canvas2d_fill_text(c, "wide gamut: sRGB (top) vs Rec.2020 (bottom)", lx, 132.0f);
    canvas2d_rgb const prim[3] = {
        { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
    };
    for (int i = 0; i < 3; i++) {
        float const x = lx + (float)i * 150.0f;
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_LINEAR_SRGB, prim[i].r, prim[i].g, prim[i].b, 1.0f);
        canvas2d_fill_rect(c, x, 144.0f, 130.0f, 34.0f);
        canvas2d_rgb const wide = canvas2d_rec2020_to_linear_srgb(prim[i]);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_LINEAR_SRGB, wide.r, wide.g, wide.b, 1.0f);
        canvas2d_fill_rect(c, x, 180.0f, 130.0f, 34.0f);
    }

    // C. Brighter than sRGB white: fills above linear 1.0, which PQ carries as
    // HDR highlights (1.0 == the reference white, ~203 cd/m^2).
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.70f, 0.74f, 0.82f, 1.0f);
    canvas2d_fill_text(c, "brighter than sRGB white: 1x .. 8x (HDR highlights)", lx, 252.0f);
    float const mult[4] = { 1.0f, 2.0f, 4.0f, 8.0f };
    char const *const mlabel[4] = { "1x", "2x", "4x", "8x" };
    for (int i = 0; i < 4; i++) {
        float const x = lx + (float)i * 112.0f;
        float const k = mult[i];
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_LINEAR_SRGB, k, k, k, 1.0f);
        canvas2d_fill_rect(c, x, 264.0f, 96.0f, 56.0f);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.08f, 0.08f, 0.10f, 1.0f);
        canvas2d_fill_text(c, mlabel[i], x + 8.0f, 308.0f);
    }

    // D. Extended gradients: the ramp itself carries extended values.  An HDR grey
    // ramp (reference white -> 6x) over a wide-gamut ramp (sRGB red -> Rec.2020
    // red); both far endpoints are outside [0,1] and the gradient interpolates
    // straight through them.
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.70f, 0.74f, 0.82f, 1.0f);
    canvas2d_fill_text(c, "extended gradients: HDR white->6x (top), sRGB->Rec.2020 red (bottom)",
                     lx, 356.0f);
    canvas2d_set_fill_linear_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, lx, 0.0f, lx + gw, 0.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_LINEAR_SRGB, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_LINEAR_SRGB, 1.0f, 6.0f, 6.0f, 6.0f, 1.0f);
    canvas2d_fill_rect(c, lx, 368.0f, gw, 26.0f);
    canvas2d_rgb const wred = canvas2d_rec2020_to_linear_srgb((canvas2d_rgb){ 1.0f, 0.0f, 0.0f });
    canvas2d_set_fill_linear_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, lx, 0.0f, lx + gw, 0.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_LINEAR_SRGB, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_LINEAR_SRGB, 1.0f, wred.r, wred.g, wred.b, 1.0f);
    canvas2d_fill_rect(c, lx, 398.0f, gw, 26.0f);

    // E. HDR shadow: a shape whose shadow colour is brighter than sRGB white -- on
    // an HDR display the glow reads past paper-white.  Blur, no offset, so the
    // glow is a halo around the shape (the shadow tint no longer clamps).
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.70f, 0.74f, 0.82f, 1.0f);
    canvas2d_fill_text(c, "HDR shadow: a glow brighter than sRGB white", lx, 462.0f);
    canvas2d_set_shadow_color_rgba(c, CANVAS2D_CS_LINEAR_SRGB, 0.0f, 5.0f, 5.0f, 1.0f);
    canvas2d_set_shadow_blur(c, 16.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.55f, 0.57f, 0.62f, 1.0f);
    canvas2d_fill_rect(c, 180.0f, 500.0f, 120.0f, 44.0f);
    canvas2d_set_shadow_color_rgba(c, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 0.0f);  // shadow off
    canvas2d_set_shadow_blur(c, 0.0f);

    save(c, "gallery/extendedrange.png");
}

// rgba-float16 ImageData: the float16 get/put twins carry the extended range an
// RGBA8 ImageData cannot.  The same 0..6x HDR grey ramp is deposited three ways on
// a linear canvas -- RGBA8 putImageData (top, clamped: everything past linear 1.0
// saturates to flat white), float16 putImageData (middle, the ramp keeps
// brightening, PQ-encoded as HDR highlights), and a float16 getImageData ->
// putImageData round trip of the middle row (bottom, lossless: identical to it).
static void imagedataf16(void) {
    int const w = 480, h = 284;
    struct canvas2d_context *__single c = canvas2d(w, h, CANVAS2D_CS_LINEAR_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/imagedataf16.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_LEFT);
    canvas2d_set_text_baseline(c, CANVAS2D_BASELINE_ALPHABETIC);
    canvas2d_set_font_size(c, 13.0f);

    int const iw = 440, ih = 48, ix = 20;
    int const flen = iw * ih * 4;       // RGBA element count (u8 bytes == f16 elements)
    float const peak = 6.0f;            // the ramp runs 0 .. 6x linear (1.0 == ref white)

    _Float16 *__counted_by(flen) f16 = malloc((size_t)flen * sizeof *f16);
    uint8_t  *__counted_by(flen) u8  = malloc((size_t)flen);
    _Float16 *__counted_by(flen) rt  = malloc((size_t)flen * sizeof *rt);
    if (f16 && u8 && rt) {
        for (int x = 0; x < iw; x++) {
            float const v = peak * (float)x / (float)(iw - 1);             // 0 .. peak
            _Float16 const hv = (_Float16)v;
            uint8_t const bv = (uint8_t)(canvas2d_clamp01(v) * 255.0f + 0.5f);  // u8 clamps >1
            for (int y = 0; y < ih; y++) {
                int const o = (y * iw + x) * 4;
                f16[o + 0] = hv; f16[o + 1] = hv; f16[o + 2] = hv; f16[o + 3] = (_Float16)1.0f;
                u8[o + 0] = bv;  u8[o + 1] = bv;  u8[o + 2] = bv;  u8[o + 3] = 255;
            }
        }

        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.70f, 0.74f, 0.82f, 1.0f);
        canvas2d_fill_text(c, "RGBA8 putImageData: HDR clamps to flat white", (float)ix, 28.0f);
        canvas2d_put_image_data(c, CANVAS2D_CS_LINEAR_SRGB, u8, flen, iw, ih, ix, 40);

        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.70f, 0.74f, 0.82f, 1.0f);
        canvas2d_fill_text(c, "float16 putImageData: HDR preserved (0..6x)", (float)ix, 122.0f);
        canvas2d_put_image_data_f16(c, CANVAS2D_CS_LINEAR_SRGB, f16, flen, iw, ih, ix, 134);

        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.70f, 0.74f, 0.82f, 1.0f);
        canvas2d_fill_text(c, "float16 getImageData -> putImageData: lossless round trip",
                         (float)ix, 216.0f);
        canvas2d_get_image_data_f16(c, CANVAS2D_CS_LINEAR_SRGB, ix, 134, iw, ih, rt, flen);
        canvas2d_put_image_data_f16(c, CANVAS2D_CS_LINEAR_SRGB, rt, flen, iw, ih, ix, 228);
    }
    free(f16);
    free(u8);
    free(rt);
    save(c, "gallery/imagedataf16.png");
}

// Image colour-space conversion on a linear-working-space canvas, shown as a
// difference: a mid-tone-rich sRGB test card -- a grayscale ramp, per-channel
// ramps, and mid-tone colour swatches, where the sRGB transfer curves most --
// drawn twice.  LEFT is tagged sRGB, so the sampler converts sRGB->linear on
// deposit; RIGHT is tagged linear, matched, so the same bytes deposit unchanged.
// RIGHT is the old behaviour (bytes treated as already linear): its shadows lift
// and its colours wash out.  The program carries a `working_space linear` line
// and replays onto a linear canvas.
static void imagecolorspace(void) {
    enum { CW = 192, CH = 144 };
    int const pad = 16, gap = 28, top = 26;
    int const w = pad + CW + gap + CW + pad, h = top + CH + 30;
    struct canvas2d_context *__single out = canvas2d(w, h, CANVAS2D_CS_LINEAR_SRGB);
    if (!out) {
        return;
    }
    record_scene(out, "gallery/imagecolorspace.canvas");
    canvas2d_set_fill_rgba(out, CANVAS2D_CS_SRGB, 0.12f, 0.12f, 0.15f, 1.0f);
    canvas2d_fill_rect(out, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_set_text_align(out, CANVAS2D_ALIGN_LEFT);
    canvas2d_set_text_baseline(out, CANVAS2D_BASELINE_ALPHABETIC);

    int const clen = CW * CH * 4;
    uint8_t *__counted_by(clen) card = malloc((size_t)clen);
    if (!card) {
        canvas2d_free(out);
        return;
    }
    static uint8_t const swatch[8][3] = {
        { 180, 90, 90 },  { 90, 180, 90 },   { 90, 90, 180 },   { 180, 180, 90 },
        { 180, 90, 180 }, { 90, 180, 180 },  { 150, 150, 150 }, { 110, 140, 170 },
    };
    for (int y = 0; y < CH; y++) {
        for (int x = 0; x < CW; x++) {
            int const ramp = x * 255 / (CW - 1);
            uint8_t r, g, b;
            if (y < 48) {  // grayscale ramp
                r = g = b = (uint8_t)ramp;
            } else if (y < 96) {  // R / G / B channel ramps, 16px bands
                int const band = (y - 48) / 16;
                r = band == 0 ? (uint8_t)ramp : 0;
                g = band == 1 ? (uint8_t)ramp : 0;
                b = band == 2 ? (uint8_t)ramp : 0;
            } else {  // eight mid-tone swatches
                int const i = x * 8 / CW;
                r = swatch[i][0];
                g = swatch[i][1];
                b = swatch[i][2];
            }
            int const o = (y * CW + x) * 4;
            card[o + 0] = r;
            card[o + 1] = g;
            card[o + 2] = b;
            card[o + 3] = 255;
        }
    }

    // The SAME bytes, tagged two ways: sRGB (converts on deposit) and linear
    // (matched, deposits unchanged -- the old, wrong reading).
    struct canvas2d_image *__single honored =
        canvas2d_image_unorm8(CANVAS2D_CS_SRGB, card, CW, CH, CANVAS2D_ALPHA_UNPREMUL);
    struct canvas2d_image *__single ignored =
        canvas2d_image_unorm8(CANVAS2D_CS_LINEAR_SRGB, card, CW, CH, CANVAS2D_ALPHA_UNPREMUL);
    free(card);
    if (honored) {
        canvas2d_draw_image(out, honored, (float)pad, (float)top);
    }
    if (ignored) {
        canvas2d_draw_image(out, ignored, (float)(pad + CW + gap), (float)top);
    }
    canvas2d_image_free(honored);
    canvas2d_image_free(ignored);

    canvas2d_set_fill_rgba(out, CANVAS2D_CS_SRGB, 0.80f, 0.83f, 0.90f, 1.0f);
    canvas2d_set_font_size(out, 12.0f);
    canvas2d_fill_text(out, "tag honored (sRGB -> linear)", (float)pad, 18.0f);
    canvas2d_fill_text(out, "tag ignored (bytes as linear)",
                     (float)(pad + CW + gap), 18.0f);
    canvas2d_set_fill_rgba(out, CANVAS2D_CS_SRGB, 0.55f, 0.59f, 0.68f, 1.0f);
    canvas2d_fill_text(out, "same sRGB bytes on a linear canvas", (float)pad,
                     (float)(top + CH + 20));
    save(out, "gallery/imagecolorspace.png");
}

// imageSmoothingQuality made live -- the drawImage flavour of the emoji ruler,
// one row per quality tier.  The minify ramp draws the same 160px rocket
// bitmap (rendered once on a scratch canvas, read back as straight RGBA8:
// exactly the borrowed buffer the public drawImage takes), sweeping 80px down
// to 6px -- pure axis-aligned scaling, no rotation, and the geometric steps
// keep most factors away from clean powers of two: low (plain bilinear, the
// spec default) shimmers as its four taps undersample; medium's premultiplied
// mip chain + trilinear stays clean.
// The magnify cell (right) is a hard-edged test card instead -- emoji art is
// already antialiased, i.e. band-limited, so reconstruction kernels barely
// differ on it; single-pixel checker, dots, and diagonals are where they
// part.  At 7x, low/medium's bilinear turns the dots into a soft bead
// lattice while high's 4x4 Catmull-Rom keeps them round and contrasty.

static void imagescale(void) {
    struct canvas2d_context *__single c = canvas2d(700, 520, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/imagescale.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.94f, 0.94f, 0.96f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 700.0f, 520.0f);

    // The rocket source: rendered on a scratch canvas and SNAPSHOTTED --
    // canvas-as-source, the surface's premultiplied f16 pixels quantized once
    // into a reified image, no read_rgba round trip -- then its mip pyramid
    // built once, explicitly, where every minifying draw below gets it free.
    enum { SRC = 160 };
    struct canvas2d_context *__single s = canvas2d(SRC, SRC, CANVAS2D_CS_SRGB);
    if (!s) {
        canvas2d_free(c);
        return;
    }
    // Font size 160 is the canonical emoji capture size (CANVAS2D_CAPTURE_EM), so
    // this draw is the capture texel-for-texel -- scale 1, no mip blend -- and
    // the snapshot is maximally sharp.  The rocket's ink box at 160px spans
    // x in [0, 160] from the pen and y in [-20, 140] about the baseline, so a
    // left-aligned alphabetic draw at (0, 140) fills the canvas exactly.
    canvas2d_set_font_size(s, 160.0f);
    canvas2d_fill_text(s, "🚀", 0.0f, 140.0f);
    struct canvas2d_image *__single rocket = canvas2d_snapshot(s);
    canvas2d_free(s);
    if (!rocket || !canvas2d_image_build_mips(rocket)) {
        canvas2d_image_free(rocket);
        canvas2d_free(c);
        return;
    }

    // The magnification test card: dark ink on a white ground, every feature
    // one pixel wide -- a frame, a diagonal, a 1px checkerboard quadrant, and
    // isolated dots.  A straight-RGBA8 reified image, deliberately left
    // mip-LESS: it only ever magnifies, so the pyramid would be dead weight
    // (and its block records without an image_mips line).
    enum { CARD = 10 };
    uint8_t cardpx[CARD * CARD * 4];
    for (int y = 0; y < CARD; y++) {
        for (int x = 0; x < CARD; x++) {
            bool ink = x == 0 || y == 0 || x == CARD - 1 || y == CARD - 1;
            ink = ink || x == y;                                  // diagonal
            ink = ink || (x >= 6 && y >= 6 && (((unsigned)x ^ (unsigned)y) & 1u));  // checker
            ink = ink || (y == 2 && (x == 6 || x == 8));          // bead dots
            int const o = (y * CARD + x) * 4;
            uint8_t const v = ink ? 32 : 255;
            cardpx[o + 0] = v;
            cardpx[o + 1] = v;
            cardpx[o + 2] = v;
            cardpx[o + 3] = 255;
        }
    }
    struct canvas2d_image *__single card =
        canvas2d_image_unorm8(CANVAS2D_CS_SRGB, cardpx, CARD, CARD, CANVAS2D_ALPHA_UNPREMUL);
    if (!card) {
        canvas2d_image_free(rocket);
        canvas2d_free(c);
        return;
    }

    canvas2d_set_text_align(c, CANVAS2D_ALIGN_LEFT);
    canvas2d_set_text_baseline(c, CANVAS2D_BASELINE_ALPHABETIC);

    // Both sources at 1:1 for reference -- an integer-placed unscaled
    // drawImage is an exact passthrough at any quality tier.  The card's
    // reference sits centred atop its magnification column, so each column
    // reads source-then-tiers straight down.
    canvas2d_draw_image(c, rocket, 20.0f, 14.0f);
    canvas2d_draw_image(c, card, 593.0f, 166.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.40f, 0.43f, 0.50f, 1.0f);
    canvas2d_set_font_size(c, 12.0f);
    canvas2d_fill_text(c, "the rocket source, 1:1 (160px)", 20.0f, 192.0f);
    canvas2d_fill_text(c, "the test card, 1:1 (10px)", 528.0f, 192.0f);

    static enum canvas2d_image_smoothing_quality const tier[3] = {
        CANVAS2D_SMOOTHING_LOW, CANVAS2D_SMOOTHING_MEDIUM, CANVAS2D_SMOOTHING_HIGH,
    };
    static char const *const label[3] = {
        "low: bilinear",
        "medium: + premultiplied mips, trilinear minification",
        "high: + 4x4 Catmull-Rom magnification",
    };
    for (int row = 0; row < 3; row++) {
        float const top = 214.0f + 100.0f * (float)row;
        canvas2d_set_image_smoothing_quality(c, tier[row]);
        float x = 88.0f;
        int const n = 7;
        for (int i = 0; i < n; i++) {
            float const t = (float)i / (float)(n - 1);
            float const size = 80.0f * powf(6.0f / 80.0f, t);
            canvas2d_draw_image_scaled(c, rocket, x,
                                     top + 42.0f - size * 0.5f, size, size);
            x += size + 14.0f;
        }
        // The test card, 10 source px blown up to 84 (8.4x).
        canvas2d_draw_image_scaled(c, card, 556.0f, top, 84.0f, 84.0f);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.40f, 0.43f, 0.50f, 1.0f);
        canvas2d_set_font_size(c, 12.0f);
        canvas2d_fill_text(c, label[row], 12.0f, top + 96.0f);
    }
    canvas2d_set_image_smoothing_quality(c, CANVAS2D_SMOOTHING_LOW);
    canvas2d_image_free(rocket);
    canvas2d_image_free(card);
    save(c, "gallery/imagescale.png");
}

// Text shaping + font fallback: one fill_text per line, each a greeting in a
// different script.  Core Text picks the right fallback font per run, joins Arabic
// contextually, lays RTL out right-to-left, reorders Devanagari -- all rendered by
// the same coverage rasterizer (and color emoji as bitmaps).
static void shaping(void) {
    struct canvas2d_context *__single c = canvas2d(500, 348, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/shaping.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 500.0f, 348.0f);

    canvas2d_set_text_align(c, CANVAS2D_ALIGN_LEFT);
    canvas2d_set_text_baseline(c, CANVAS2D_BASELINE_ALPHABETIC);

    canvas2d_set_fill_linear_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, 36.0f, 0.0f, 360.0f, 0.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.0f, 0.99f, 0.80f, 0.26f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 1.0f, 0.35f, 0.55f, 0.98f, 1.0f);
    canvas2d_set_font_size(c, 21.0f);
    canvas2d_fill_text(c, "one fill_text, every script", 36.0f, 46.0f);

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
        float const y = 92.0f + (float)i * 31.0f;
        float const t = (float)i / 8.0f;
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.5f + 0.5f * cosf(TAU * t),
                             0.5f + 0.5f * cosf(TAU * (t + 0.33f)),
                             0.5f + 0.5f * cosf(TAU * (t + 0.66f)), 1.0f);
        canvas2d_set_font_size(c, 28.0f);
        canvas2d_fill_text(c, row[i].greeting, 40.0f, y);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.55f, 0.59f, 0.67f, 1.0f);
        canvas2d_set_font_size(c, 12.0f);
        canvas2d_fill_text(c, row[i].label, 320.0f, y - 4.0f);
    }

    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.62f, 0.66f, 0.74f, 1.0f);
    canvas2d_set_font_size(c, 13.0f);
    canvas2d_fill_text(c, "Core Text fallback + shaping, one coverage rasterizer",
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
    struct canvas2d_context *__single c = canvas2d(500, 330, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/rtl.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 500.0f, 330.0f);

    // Headline, ltr.
    canvas2d_set_fill_linear_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, 36.0f, 0.0f, 464.0f, 0.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.0f, 0.99f, 0.80f, 0.26f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 1.0f, 0.35f, 0.55f, 0.98f, 1.0f);
    canvas2d_set_font_size(c, 20.0f);
    canvas2d_fill_text(c, "direction: rtl", 36.0f, 40.0f);

    // RTL paragraphs from the right margin: start anchors right under rtl.
    float const right = 464.0f;
    canvas2d_set_direction(c, CANVAS2D_DIRECTION_RTL);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);
    canvas2d_set_font_size(c, 30.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.95f, 0.83f, 0.45f, 1.0f);
    canvas2d_fill_text(c, "שלום עולם", right, 88.0f);          // Hebrew
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.55f, 0.85f, 0.65f, 1.0f);
    canvas2d_fill_text(c, "مرحبا بالعالم", right, 130.0f);     // Arabic, joined
    canvas2d_set_font_size(c, 26.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.70f, 0.75f, 0.95f, 1.0f);
    canvas2d_fill_text(c, "טקסט עם canvas2d בפנים", right, 172.0f);  // mixed bidi

    // Captions, back to ltr at the left margin.
    canvas2d_set_direction(c, CANVAS2D_DIRECTION_LTR);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_LEFT);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.55f, 0.59f, 0.67f, 1.0f);
    canvas2d_set_font_size(c, 12.0f);
    canvas2d_fill_text(c, "Hebrew", 36.0f, 84.0f);
    canvas2d_fill_text(c, "Arabic joins", 36.0f, 126.0f);
    canvas2d_fill_text(c, "mixed bidi", 36.0f, 168.0f);

    // start/end resolve against direction: four rows off one anchor line.
    float const ax = 250.0f;
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.85f, 0.35f, 0.35f, 1.0f);
    canvas2d_fill_rect(c, ax - 0.5f, 196.0f, 1.0f, 122.0f);
    struct {
        enum canvas2d_direction dir;
        enum canvas2d_text_align align;
        char const *label;
    } const row[4] = {
        { CANVAS2D_DIRECTION_LTR, CANVAS2D_ALIGN_START, "ltr start" },
        { CANVAS2D_DIRECTION_LTR, CANVAS2D_ALIGN_END,   "ltr end" },
        { CANVAS2D_DIRECTION_RTL, CANVAS2D_ALIGN_START, "rtl start" },
        { CANVAS2D_DIRECTION_RTL, CANVAS2D_ALIGN_END,   "rtl end" },
    };
    for (int i = 0; i < 4; i++) {
        float const y = 222.0f + (float)i * 28.0f;
        canvas2d_set_direction(c, row[i].dir);
        canvas2d_set_text_align(c, row[i].align);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.90f, 0.92f, 0.96f, 1.0f);
        canvas2d_set_font_size(c, 18.0f);
        canvas2d_fill_text(c, "אב ab", ax, y);
        canvas2d_set_direction(c, CANVAS2D_DIRECTION_LTR);
        canvas2d_set_text_align(c, CANVAS2D_ALIGN_LEFT);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.55f, 0.59f, 0.67f, 1.0f);
        canvas2d_set_font_size(c, 12.0f);
        canvas2d_fill_text(c, row[i].label, 36.0f, y);
    }

    save(c, "gallery/rtl.png");
}

// canvas2d_shape_text takes counted (bytes, len) slices; a string literal carries
// its byte length at compile time, so S(lit) expands to exactly that pair (the
// test_shaping.c idiom).
#define S(lit) ("" lit), ((int)sizeof lit - 1)

// The pinned family the canvas itself shapes with (canvas.h: the typeface is
// fixed to Libian TC).  Shaping here with the same name, size, and direction
// reproduces fill_text's layout exactly, so hit-test geometry computed from
// these lines lands where the canvas draws the glyphs.
#define SELECTION_FONT "Libian TC"

// The selection scene's lines, each shared between a counted canvas2d_shape_text
// call (geometry) and a NUL-terminated canvas2d_fill_text call (pixels).
#define SEL_LATIN "The quick brown fox"
#define SEL_BIDI  "טקסט עם canvas2d בפנים"
#define SEL_CARET "abc 😀 مرحبا"
#define SEL_CLICK "click anywhere"

// A caret: a thin vertical stroke in the current fill, spanning `h` from `top`.
static void caret(struct canvas2d_context *__single cv, float x, float top, float h) {
    canvas2d_fill_rect(cv, x - 0.75f, top, 1.5f, h);
}

// Selection and carets: the shaped-line hit-testing API (canvas2d_text.h) made
// visible.  All geometry is computed at record time -- canvas2d_shape_text with the
// canvas's pinned family/size/direction shapes exactly the lines fill_text
// draws -- and lands in the program as plain rects, so the scene replays
// fontless like every other.  Row 1: a Latin selection is one highlight span
// under the glyphs.  Row 2: the bidi money shot -- ONE logical range over a
// mixed-direction line comes back from canvas2d_shaped_selection as TWO visual
// spans (the embedded Latin reorders; the gap is the Latin run's unselected
// middle).  Row 3: carets from canvas2d_shaped_x_at_index at cluster edges; index
// 5 sits INSIDE the emoji's surrogate pair, so its caret (pink, shorter) snaps
// onto index 4's leading edge.  Row 4: the round trip -- a click x maps through
// canvas2d_shaped_index_at_x to a logical index and back through
// canvas2d_shaped_x_at_index to the caret on that glyph's left edge.
static void selection(void) {
    struct canvas2d_context *__single c = canvas2d(520, 352, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/selection.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 520.0f, 352.0f);

    canvas2d_set_text_align(c, CANVAS2D_ALIGN_LEFT);
    canvas2d_set_text_baseline(c, CANVAS2D_BASELINE_ALPHABETIC);

    // Headline.
    canvas2d_set_fill_linear_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, 36.0f, 0.0f, 484.0f, 0.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.0f, 0.99f, 0.80f, 0.26f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 1.0f, 0.35f, 0.55f, 0.98f, 1.0f);
    canvas2d_set_font_size(c, 20.0f);
    canvas2d_fill_text(c, "selection & carets", 36.0f, 36.0f);

    // Every line is set at one size; the highlight and caret heights come from
    // the font's vertical metrics at that size (the classic editor line box).
    float const lx = 36.0f, size = 26.0f;
    canvas2d_set_font_size(c, size);
    canvas2d_text_metrics const m = canvas2d_measure_text_full(c, "Hg");
    float const asc = m.font_bounding_box_ascent, desc = m.font_bounding_box_descent;

    // Row 1: a Latin selection -- logical [4,15) "quick brown" is one visual
    // span, filled selection-blue under the glyphs.
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.55f, 0.59f, 0.67f, 1.0f);
    canvas2d_set_font_size(c, 12.0f);
    canvas2d_fill_text(c, "a Latin selection [4,15): one visual span", lx, 66.0f);
    float const y1 = 102.0f;
    struct canvas2d_shaped *__single latin = canvas2d_shape_text(S(SELECTION_FONT), size,
                                                         false, 400, false, 0, 0, 0, 4, S(""), S(SEL_LATIN));
    canvas2d_xspan sp[4];
    if (latin) {
        int const n = canvas2d_shaped_selection(latin, 4, 15, sp, 4);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.25f, 0.47f, 0.90f, 0.45f);
        for (int k = 0; k < n; k++) {
            canvas2d_fill_rect(c, lx + sp[k].x0, y1 - asc, sp[k].x1 - sp[k].x0, asc + desc);
        }
    }
    canvas2d_shaped_free(latin);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.93f, 0.94f, 0.98f, 1.0f);
    canvas2d_set_font_size(c, size);
    canvas2d_fill_text(c, SEL_LATIN, lx, y1);

    // Row 2: ONE logical range over a mixed-direction line.  Under the rtl
    // paragraph base the embedded Latin reorders, so logical [5,12) -- Hebrew
    // "עם", a space, and "canv" -- maps to TWO visual spans with the Latin
    // run's unselected middle between them.
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.55f, 0.59f, 0.67f, 1.0f);
    canvas2d_set_font_size(c, 12.0f);
    canvas2d_fill_text(c, "bidi: one logical range [5,12), two visual spans", lx, 142.0f);
    float const y2 = 178.0f;
    struct canvas2d_shaped *__single bidi = canvas2d_shape_text(S(SELECTION_FONT), size,
                                                        true, 400, false, 0, 0, 0, 4, S(""), S(SEL_BIDI));
    if (bidi) {
        float const bx = 484.0f - canvas2d_shaped_width(bidi);  // hang from the right margin
        int const n = canvas2d_shaped_selection(bidi, 5, 12, sp, 4);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.25f, 0.47f, 0.90f, 0.45f);
        for (int k = 0; k < n; k++) {
            canvas2d_fill_rect(c, bx + sp[k].x0, y2 - asc, sp[k].x1 - sp[k].x0, asc + desc);
        }
        canvas2d_set_direction(c, CANVAS2D_DIRECTION_RTL);  // the base the spans assume
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.93f, 0.94f, 0.98f, 1.0f);
        canvas2d_set_font_size(c, size);
        canvas2d_fill_text(c, SEL_BIDI, bx, y2);  // align left: the pen origin is bx
        canvas2d_set_direction(c, CANVAS2D_DIRECTION_LTR);
    }
    canvas2d_shaped_free(bidi);

    // Row 3: carets at cluster edges.  Indices 0/4/9/12 (amber) are the line
    // start, the emoji's leading edge, a boundary inside joined Arabic, and the
    // end-of-line caret; index 5 (pink, shorter) is the emoji's low surrogate
    // -- inside the cluster -- and snaps onto index 4's caret.
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.55f, 0.59f, 0.67f, 1.0f);
    canvas2d_set_font_size(c, 12.0f);
    canvas2d_fill_text(c, "carets at indices 0, 4, 9, 12; 5 is inside the emoji and snaps onto 4",
                     lx, 218.0f);
    float const y3 = 254.0f;
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.93f, 0.94f, 0.98f, 1.0f);
    canvas2d_set_font_size(c, size);
    canvas2d_fill_text(c, SEL_CARET, lx, y3);
    struct canvas2d_shaped *__single line = canvas2d_shape_text(S(SELECTION_FONT), size,
                                                        false, 400, false, 0, 0, 0, 4, S(""), S(SEL_CARET));
    if (line) {
        int const at[4] = { 0, 4, 9, 12 };
        char const *const lbl[4] = { "0", "4 = 5", "9", "12" };
        canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
        for (int i = 0; i < 4; i++) {
            float const x = lx + canvas2d_shaped_x_at_index(line, at[i]);
            canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.99f, 0.80f, 0.26f, 1.0f);
            caret(c, x, y3 - asc, asc + desc);
            canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.62f, 0.66f, 0.74f, 1.0f);
            canvas2d_set_font_size(c, 11.0f);
            canvas2d_fill_text(c, lbl[i], x, y3 + desc + 14.0f);
        }
        canvas2d_set_text_align(c, CANVAS2D_ALIGN_LEFT);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.96f, 0.35f, 0.45f, 1.0f);  // the snapped caret
        caret(c, lx + canvas2d_shaped_x_at_index(line, 5), y3 - asc * 0.5f, asc * 0.5f + desc);
    }
    canvas2d_shaped_free(line);

    // Row 4: the round trip.  A click (pink dot) hit-tests to a logical index,
    // and that index's caret (amber) lands on the clicked glyph's left edge.
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.55f, 0.59f, 0.67f, 1.0f);
    canvas2d_set_font_size(c, 12.0f);
    canvas2d_fill_text(c, "caret from a click: index_at_x, then x_at_index back to the glyph edge",
                     lx, 300.0f);
    float const y4 = 336.0f;
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.93f, 0.94f, 0.98f, 1.0f);
    canvas2d_set_font_size(c, size);
    canvas2d_fill_text(c, SEL_CLICK, lx, y4);
    struct canvas2d_shaped *__single click = canvas2d_shape_text(S(SELECTION_FONT), size,
                                                         false, 400, false, 0, 0, 0, 4, S(""), S(SEL_CLICK));
    if (click) {
        float const cx = canvas2d_shaped_width(click) * 0.5f;     // the click, mid-line
        int const idx = canvas2d_shaped_index_at_x(click, cx);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.99f, 0.80f, 0.26f, 1.0f);
        caret(c, lx + canvas2d_shaped_x_at_index(click, idx), y4 - asc, asc + desc);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.96f, 0.35f, 0.45f, 1.0f);
        canvas2d_begin_path(c);
        canvas2d_arc(c, lx + cx, y4 - asc * 0.35f, 3.0f, 0.0f, TAU, false);
        canvas2d_fill(c, CANVAS2D_NONZERO);
    }
    canvas2d_shaped_free(click);

    save(c, "gallery/selection.png");
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
        void (*add)(struct canvas2d_context *__single cv, float amount);
        float amt;
        void (*add2)(struct canvas2d_context *__single cv, float amount);  // chained second entry
        float amt2;
        bool shadow;                  // append a drop-shadow() after add/add2
        float sdx, sdy, sblur;        // its offset + blur
        float sr, sg, sb, sa;         // its colour
        char const *label;
    } const cell[15] = {
        { .label = "none" },
        { .add = canvas2d_add_filter_brightness, .amt = 1.5f,       .label = "brightness(1.5)" },
        { .add = canvas2d_add_filter_contrast,   .amt = 2.0f,       .label = "contrast(2)" },
        { .add = canvas2d_add_filter_grayscale,  .amt = 1.0f,       .label = "grayscale(1)" },
        { .add = canvas2d_add_filter_hue_rotate, .amt = TAU / 3.0f, .label = "hue-rotate(120deg)" },
        { .add = canvas2d_add_filter_invert,     .amt = 1.0f,       .label = "invert(1)" },
        { .add = canvas2d_add_filter_opacity,    .amt = 0.35f,      .label = "opacity(0.35)" },
        { .add = canvas2d_add_filter_saturate,   .amt = 3.0f,       .label = "saturate(3)" },
        { .add = canvas2d_add_filter_sepia,      .amt = 1.0f,       .label = "sepia(1)" },
        { .add = canvas2d_add_filter_blur,       .amt = 1.5f,       .label = "blur(1.5)" },
        { .add = canvas2d_add_filter_blur,       .amt = 3.0f,       .label = "blur(3)" },
        { .add = canvas2d_add_filter_blur,       .amt = 3.0f,
          .add2 = canvas2d_add_filter_saturate,  .amt2 = 3.0f,      .label = "blur(3) saturate(3)" },
        { .shadow = true, .sdx = 3.0f, .sdy = 3.0f, .sblur = 2.0f,
          .sr = 0.0f, .sg = 0.0f, .sb = 0.0f, .sa = 1.0f,
          .label = "drop-shadow(3 3 2)" },
        { .shadow = true, .sdx = 6.0f, .sdy = 6.0f, .sblur = 3.0f,
          .sr = 0.55f, .sg = 0.25f, .sb = 0.95f, .sa = 0.6f,
          .label = "drop-shadow(violet 60%)" },
        { .add = canvas2d_add_filter_grayscale,  .amt = 1.0f,
          .shadow = true, .sdx = 6.0f, .sdy = 6.0f, .sblur = 3.0f,
          .sr = 0.55f, .sg = 0.25f, .sb = 0.95f, .sa = 0.85f,
          .label = "grayscale(1) drop-shadow" },
    };
    float const M = 12.0f, cellW = 140.0f, cellH = 124.0f;
    struct canvas2d_context *__single c = canvas2d(444, 644, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/filters.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.13f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 444.0f, 644.0f);

    for (int i = 0; i < 15; i++) {
        int col = i % 3, row = i / 3;
        float ox = M + (float)col * cellW, oy = M + (float)row * cellH;

        // This cell's filter (one function, a two-entry chain, or a chain
        // ending in drop-shadow -- always in list order).
        canvas2d_set_filter_none(c);
        if (cell[i].add) {
            cell[i].add(c, cell[i].amt);
        }
        if (cell[i].add2) {
            cell[i].add2(c, cell[i].amt2);
        }
        if (cell[i].shadow) {
            canvas2d_add_filter_drop_shadow(c, CANVAS2D_CS_SRGB, cell[i].sdx, cell[i].sdy,
                                          cell[i].sblur, cell[i].sr, cell[i].sg,
                                          cell[i].sb, cell[i].sa);
        }

        // The motif, filtered: a gradient tile, then two translucent discs.
        canvas2d_set_fill_linear_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, ox + 18.0f, oy + 12.0f,
                                        ox + 122.0f, oy + 100.0f);
        canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.0f, 0.98f, 0.55f, 0.15f, 1.0f);
        canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.5f, 0.85f, 0.25f, 0.55f, 1.0f);
        canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 1.0f, 0.20f, 0.55f, 0.95f, 1.0f);
        canvas2d_begin_path(c);
        canvas2d_round_rect(c, ox + 18.0f, oy + 12.0f, 104.0f, 88.0f, 12.0f);
        canvas2d_fill(c, CANVAS2D_NONZERO);

        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.15f, 0.90f, 0.45f, 0.55f);
        canvas2d_begin_path(c);
        canvas2d_arc(c, ox + 52.0f, oy + 44.0f, 24.0f, 0.0f, TAU, false);
        canvas2d_fill(c, CANVAS2D_NONZERO);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.95f, 0.20f, 0.20f, 0.55f);
        canvas2d_begin_path(c);
        canvas2d_arc(c, ox + 84.0f, oy + 64.0f, 24.0f, 0.0f, TAU, false);
        canvas2d_fill(c, CANVAS2D_NONZERO);

        // Label, with the filter cleared so every caption reads uniformly.
        canvas2d_set_filter_none(c);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.90f, 0.92f, 0.96f, 1.0f);
        canvas2d_set_font_size(c, 13.0f);
        canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
        canvas2d_fill_text(c, cell[i].label, ox + cellW * 0.5f, oy + 116.0f);
    }
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);

    save(c, "gallery/filters.png");
}

// Three translucent primaries overlapping over a mid grey -- the canonical
// "same untagged sRGB colours, two working spaces" demonstrator.  In sRGB the
// overlaps darken and go muddy (averaging happens in gamma space); in extended
// linear sRGB the same translucent colours composite in light, so the overlaps
// stay bright.  Drawn into the half rect at (ox, oy).
static void linearlight_motif(struct canvas2d_context *__single c, float ox, float oy) {
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.5f, 0.5f, 0.5f, 1.0f);  // mid-grey ground
    canvas2d_fill_rect(c, ox, oy, 150.0f, 150.0f);

    float const r = 42.0f, cx = ox + 75.0f, cy = oy + 64.0f, off = 30.0f;
    float const a = 0.5f;
    struct { float r, g, b, dx, dy; } const disc[3] = {
        { 0.95f, 0.15f, 0.15f,  0.0f,    -off },          // red, up
        { 0.15f, 0.85f, 0.20f, -off * 0.87f, off * 0.5f },// green, lower-left
        { 0.20f, 0.35f, 0.95f,  off * 0.87f, off * 0.5f },// blue, lower-right
    };
    for (int i = 0; i < 3; i++) {
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, disc[i].r, disc[i].g, disc[i].b, a);
        canvas2d_begin_path(c);
        canvas2d_arc(c, cx + disc[i].dx, cy + disc[i].dy, r, 0.0f, TAU, false);
        canvas2d_fill(c, CANVAS2D_NONZERO);
    }
}

// The working-space demonstrator: ONE linear-working-space output canvas shows
// the same untagged sRGB scene composited two ways, side by side.  The LEFT
// half is rendered on a separate sRGB canvas (today's gamma-space compositing),
// read back to sRGB bytes, and put_image_data'd onto the linear output -- which
// decodes sRGB->linear on the way in, then re-encodes on readback, so the sRGB
// result displays faithfully.  The RIGHT half draws the identical scene natively
// on the linear canvas, so its translucent overlaps composite in light.  The
// committed program is the FIRST to carry a `working_space linear` line (its
// output canvas is linear); it replays onto a linear canvas and matches the PNG.
static void linearlight(void) {
    int const w = 316, h = 150;
    struct canvas2d_context *__single out = canvas2d(w, h, CANVAS2D_CS_LINEAR_SRGB);
    if (!out) {
        return;
    }
    record_scene(out, "gallery/linearlight.canvas");

    // The sRGB half, rendered off-canvas and copied in.  put_image_data on the
    // linear canvas decodes the incoming sRGB bytes to linear; readback encodes
    // back, so the sRGB-composited pixels survive the round trip exactly.
    struct canvas2d_context *__single srgb = canvas2d(150, 150, CANVAS2D_CS_SRGB);
    if (srgb) {
        linearlight_motif(srgb, 0.0f, 0.0f);
        int const blen = 150 * 150 * 4;
        uint8_t *__counted_by(blen) block = malloc((size_t)blen);
        if (block) {
            canvas2d_get_image_data(srgb, CANVAS2D_CS_SRGB, 0, 0, 150, 150, block, blen);
            canvas2d_put_image_data(out, CANVAS2D_CS_SRGB, block, blen, 150, 150, 0, 0);
            free(block);
        }
        canvas2d_free(srgb);
    }

    // The linear half, drawn natively on the linear output.
    linearlight_motif(out, 166.0f, 0.0f);

    save(out, "gallery/linearlight.png");
}

// The gradient-interpolation demonstrator: the two ORTHOGONAL interpolation
// knobs -- colour SPACE and alpha PREMULTIPLICATION -- isolated one variable at
// a time, so each comparison changes exactly one thing.
//
//   SPACE axis (top): the SAME opaque red->green->blue ramp interpolated in
//   each of the three spaces (sRGB, linear sRGB, Oklab).  Alpha is 1 throughout,
//   where premul == unpremul, so this is a PURE space comparison: sRGB's dark,
//   muddy midpoints; linear sRGB's brighter (light-correct) mixing; Oklab's
//   perceptually even hue sweep.
//
//   ALPHA axis (bottom): the space held FIXED at sRGB, a colour fading to
//   transparent over a checkerboard, interpolated UNPREMUL then PREMUL.  This is
//   a PURE premultiply comparison: unpremul drags the (otherwise invisible)
//   colour of the transparent end into the ramp as a muddy tint; premul fades
//   cleanly, the colour living only in the alpha.
//
// Each ramp's interpolation is chosen at the gradient's creation; the committed
// program carries it on the set_fill_linear_gradient line.  It replays on a
// plain sRGB working-space canvas and matches the PNG.
static void gradinterp_space_ramp(struct canvas2d_context *__single c, float x, float y, float w,
                                  float h, enum canvas2d_color_space space,
                                  char const *__null_terminated label) {
    canvas2d_set_fill_linear_gradient(c, space, CANVAS2D_ALPHA_UNPREMUL,  // alpha=1: premul==unpremul
                                    x, 0.0f, x + w, 0.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.0f, 0.90f, 0.12f, 0.12f, 1.0f);  // red
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.5f, 0.12f, 0.80f, 0.22f, 1.0f);  // green
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 1.0f, 0.15f, 0.28f, 0.92f, 1.0f);  // blue
    canvas2d_fill_rect(c, x, y, w, h);

    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.80f, 0.83f, 0.90f, 1.0f);
    canvas2d_set_font_size(c, 13.0f);
    canvas2d_fill_text(c, label, x, y - 6.0f);
}

static void gradinterp_alpha_ramp(struct canvas2d_context *__single c, float x, float y, float w,
                                  float h, enum canvas2d_alpha_type alpha,
                                  char const *__null_terminated label) {
    // A pale checker UNDER the ramp so where the gradient is transparent the
    // ground shows through -- the only way the premul difference reads.
    for (float gy = y; gy < y + h; gy += 12.0f) {
        for (float gx = x; gx < x + w; gx += 12.0f) {
            bool const lit = (((int)((gx - x) / 12.0f)) + ((int)((gy - y) / 12.0f))) % 2 == 0;
            canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, lit ? 0.78f : 0.56f, lit ? 0.78f : 0.56f,
                                 lit ? 0.80f : 0.59f, 1.0f);
            float const cw = gx + 12.0f > x + w ? x + w - gx : 12.0f;
            float const ch = gy + 12.0f > y + h ? y + h - gy : 12.0f;
            canvas2d_fill_rect(c, gx, gy, cw, ch);
        }
    }

    // Opaque blue -> transparent RED, sRGB space, the chosen alpha mode.  The
    // transparent end is invisible, but its colour still rides the coords: an
    // UNPREMUL lerp drags that red into the mid-ramp as a muddy tint; a PREMUL
    // lerp weights the colour by alpha first, so the vanishing red contributes
    // nothing and the fade stays clean blue.
    canvas2d_set_fill_linear_gradient(c, CANVAS2D_CS_SRGB, alpha,  // space fixed
                                    x, 0.0f, x + w, 0.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.0f, 0.15f, 0.28f, 0.92f, 1.0f);  // opaque blue
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 1.0f, 0.92f, 0.16f, 0.16f, 0.0f);  // transparent red
    canvas2d_fill_rect(c, x, y, w, h);

    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.80f, 0.83f, 0.90f, 1.0f);
    canvas2d_set_font_size(c, 13.0f);
    canvas2d_fill_text(c, label, x, y - 6.0f);
}

static void gradinterp(void) {
    int const w = 320, h = 344;
    struct canvas2d_context *__single c = canvas2d(w, h, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/gradinterp.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, (float)w, (float)h);

    canvas2d_set_text_baseline(c, CANVAS2D_BASELINE_ALPHABETIC);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_LEFT);

    float const rx = 16.0f, rw = 288.0f, rh = 36.0f, pitch = 50.0f;

    // Section caption: SPACE axis.
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.62f, 0.66f, 0.74f, 1.0f);
    canvas2d_set_font_size(c, 12.0f);
    canvas2d_fill_text(c, "interpolation SPACE  (opaque red->green->blue)", rx, 20.0f);

    // The same opaque ramp in each space, stacked (label above each).
    float const sy = 44.0f;
    gradinterp_space_ramp(c, rx, sy,             rw, rh, CANVAS2D_CS_SRGB,        "sRGB");
    gradinterp_space_ramp(c, rx, sy + pitch,     rw, rh, CANVAS2D_CS_LINEAR_SRGB, "linear sRGB");
    gradinterp_space_ramp(c, rx, sy + 2.0f * pitch, rw, rh, CANVAS2D_CS_OKLAB,    "Oklab");

    // Section caption: ALPHA axis.
    float const acap = sy + 3.0f * pitch + 8.0f;  // 202
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.62f, 0.66f, 0.74f, 1.0f);
    canvas2d_set_font_size(c, 12.0f);
    canvas2d_fill_text(c, "alpha PREMULTIPLICATION  (sRGB, blue -> transparent red)", rx, acap);

    // The same fade in each alpha mode, over the checker (label above each).
    float const ay = acap + 24.0f;  // 226
    gradinterp_alpha_ramp(c, rx, ay,         rw, rh, CANVAS2D_ALPHA_UNPREMUL, "unpremul (red bleeds in)");
    gradinterp_alpha_ramp(c, rx, ay + pitch, rw, rh, CANVAS2D_ALPHA_PREMUL,   "premul (clean fade)");

    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);
    save(c, "gallery/gradinterp.png");
}

// The per-COLOUR space-tag demonstrator: the same canvas2d_set_fill_rgba call
// means different colours under different tags.  This is the first gallery
// program to carry per-colour `space` tokens.  Three rows on a plain sRGB
// canvas:
//   sRGB    -- six numeric RGB triples tagged CANVAS2D_CS_SRGB: stored verbatim,
//              the gamma-encoded bytes (the legacy reading of these numbers).
//   linear  -- the SAME six triples tagged CANVAS2D_CS_LINEAR_SRGB: now read as
//              light, so each is encoded linear->sRGB on the way to the sRGB
//              surface and lands visibly brighter (0.5 -> ~0.74, not 0.5) --
//              the gamma gap made literal, swatch beside swatch.
//   Oklab   -- eight swatches at a FIXED Oklab lightness with the hue swept
//              around the a,b circle, tagged CANVAS2D_CS_OKLAB: a perceptual
//              specification, so the ring reads as one even brightness across
//              every hue (what equal L means), each converted Oklab->linear->
//              sRGB on store.
static void colorspaces(void) {
    int const w = 520, h = 232;
    struct canvas2d_context *__single c = canvas2d(w, h, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/colorspaces.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, (float)w, (float)h);

    // Six numeric RGB triples, drawn once tagged sRGB and once tagged linear --
    // the identical numbers, two readings.
    float const sw[6][3] = {
        { 0.25f, 0.25f, 0.25f },  // dark grey
        { 0.50f, 0.50f, 0.50f },  // mid grey
        { 0.75f, 0.20f, 0.20f },  // red
        { 0.20f, 0.65f, 0.30f },  // green
        { 0.25f, 0.40f, 0.85f },  // blue
        { 0.85f, 0.70f, 0.20f },  // gold
    };
    float const sx0 = 184.0f, sw_w = 48.0f, sw_gap = 4.0f;
    float const srgb_y = 36.0f, lin_y = 96.0f, ok_y = 156.0f, sw_h = 44.0f;

    canvas2d_set_text_baseline(c, CANVAS2D_BASELINE_ALPHABETIC);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_LEFT);

    // Row label + sRGB-tagged swatches: the bytes are these numbers.
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.80f, 0.83f, 0.90f, 1.0f);
    canvas2d_set_font_size(c, 13.0f);
    canvas2d_fill_text(c, "CANVAS_CS_SRGB", 18.0f, srgb_y + sw_h * 0.5f + 5.0f);
    for (int i = 0; i < 6; i++) {
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, sw[i][0], sw[i][1], sw[i][2], 1.0f);
        canvas2d_fill_rect(c, sx0 + (float)i * (sw_w + sw_gap), srgb_y, sw_w, sw_h);
    }

    // The SAME numbers tagged linear: encoded to sRGB on store -> brighter.
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.80f, 0.83f, 0.90f, 1.0f);
    canvas2d_set_font_size(c, 13.0f);
    canvas2d_fill_text(c, "CANVAS_CS_LINEAR_SRGB", 18.0f, lin_y + sw_h * 0.5f + 5.0f);
    for (int i = 0; i < 6; i++) {
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_LINEAR_SRGB, sw[i][0], sw[i][1], sw[i][2], 1.0f);
        canvas2d_fill_rect(c, sx0 + (float)i * (sw_w + sw_gap), lin_y, sw_w, sw_h);
    }

    // Oklab: fixed lightness, swept hue -- a perceptual hue ring.
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.80f, 0.83f, 0.90f, 1.0f);
    canvas2d_set_font_size(c, 13.0f);
    canvas2d_fill_text(c, "CANVAS_CS_OKLAB", 18.0f, ok_y + sw_h * 0.5f + 5.0f);
    int const nok = 8;
    float const L = 0.72f, C = 0.13f;  // fixed lightness, fixed chroma, hue swept
    float const ok_w = (6.0f * sw_w + 5.0f * sw_gap - (float)(nok - 1) * sw_gap)
                       / (float)nok;
    for (int i = 0; i < nok; i++) {
        float const hue = TAU * (float)i / (float)nok;
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_OKLAB, L, C * cosf(hue), C * sinf(hue), 1.0f);
        canvas2d_fill_rect(c, sx0 + (float)i * (ok_w + sw_gap), ok_y, ok_w, sw_h);
    }

    // A caption: the same numbers, three meanings.
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.55f, 0.59f, 0.68f, 1.0f);
    canvas2d_set_font_size(c, 13.0f);
    canvas2d_fill_text(c, "rows 1 & 2: identical numbers, sRGB vs linear tag", 18.0f, 216.0f);

    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);
    save(c, "gallery/colorspaces.png");
}

// ellipse with rotation: eight ellipses at evenly spaced rotation angles, filled
// (top row) and stroked (bottom row), showing the rotation parameter is live.
static void ellipserot(void) {
    struct canvas2d_context *__single c = canvas2d(480, 160, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/ellipserot.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 480.0f, 160.0f);

    int const n = 8;
    float const rx = 28.0f, ry = 12.0f, cx0 = 34.0f, step = 52.0f;

    // Filled row (top).
    for (int i = 0; i < n; i++) {
        float const rot = (float)i * (TAU * 0.5f / (float)n);
        float const t = (float)i / (float)(n - 1);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB,
                             0.5f + 0.5f * cosf(TAU * t),
                             0.5f + 0.5f * cosf(TAU * (t + 0.33f)),
                             0.5f + 0.5f * cosf(TAU * (t + 0.66f)), 1.0f);
        canvas2d_begin_path(c);
        canvas2d_ellipse(c, cx0 + (float)i * step, 46.0f,
                       rx, ry, rot, 0.0f, TAU, false);
        canvas2d_fill(c, CANVAS2D_NONZERO);
    }

    // Stroked row (bottom), same rotation sweep.
    canvas2d_set_line_width(c, 3.0f);
    for (int i = 0; i < n; i++) {
        float const rot = (float)i * (TAU * 0.5f / (float)n);
        float const t = (float)i / (float)(n - 1);
        canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB,
                               0.5f + 0.5f * cosf(TAU * t),
                               0.5f + 0.5f * cosf(TAU * (t + 0.33f)),
                               0.5f + 0.5f * cosf(TAU * (t + 0.66f)), 1.0f);
        canvas2d_begin_path(c);
        canvas2d_ellipse(c, cx0 + (float)i * step, 118.0f,
                       rx, ry, rot, 0.0f, TAU, false);
        canvas2d_stroke(c);
    }

    save(c, "gallery/ellipserot.png");
}

// Nested clips: three save/restore levels each narrowing the active region, then
// the same rainbow-stripe flood from the clip scene.  The outer clip is a circle;
// inside it the second clip intersects a rotated rectangle; inside both the third
// clip further intersects a star.  Flood stripes reveal the running intersection.
static void nestedclip(void) {
    struct canvas2d_context *__single c = canvas2d(300, 180, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/nestedclip.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 300.0f, 180.0f);

    // Show each clip boundary as a faint outline before nesting.
    float const cx = 150.0f, cy = 88.0f, r0 = 74.0f;
    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.45f, 0.48f, 0.55f, 0.45f);
    canvas2d_set_line_width(c, 1.0f);
    canvas2d_begin_path(c);
    canvas2d_arc(c, cx, cy, r0, 0.0f, TAU, false);
    canvas2d_stroke(c);
    canvas2d_save(c);
    canvas2d_translate(c, cx, cy);
    canvas2d_rotate(c, 0.45f);
    canvas2d_stroke_rect(c, -58.0f, -44.0f, 116.0f, 88.0f);
    canvas2d_restore(c);
    star(c, cx, cy, 58.0f);
    canvas2d_stroke(c);

    // Level 1: clip to the circle.
    canvas2d_save(c);
    canvas2d_begin_path(c);
    canvas2d_arc(c, cx, cy, r0, 0.0f, TAU, false);
    canvas2d_clip(c, CANVAS2D_NONZERO);

    // Level 2: clip to the rotated rectangle.
    canvas2d_save(c);
    canvas2d_translate(c, cx, cy);
    canvas2d_rotate(c, 0.45f);
    canvas2d_begin_path(c);
    canvas2d_rect(c, -58.0f, -44.0f, 116.0f, 88.0f);
    canvas2d_clip(c, CANVAS2D_NONZERO);
    canvas2d_reset_transform(c);

    // Level 3: clip to the star.
    star(c, cx, cy, 58.0f);
    canvas2d_clip(c, CANVAS2D_NONZERO);

    // Flood with rainbow stripes; only the triple intersection survives.
    clip_stripes(c, cx - r0, cy - r0, cx + r0, cy + r0);

    canvas2d_restore(c);  // pops level 2 (rect) and level 3 (star) via one restore here
    canvas2d_restore(c);  // pops level 1 (circle)

    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.80f, 0.83f, 0.90f, 1.0f);
    canvas2d_set_font_size(c, 13.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
    canvas2d_fill_text(c, "circle ∩ rect ∩ star", cx, 168.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);

    save(c, "gallery/nestedclip.png");
}

// Projective geometry (P1): a source rect mapped through set_perspective_quad onto
// a receding trapezoid (a "floor" seen in perspective).  Everything is authored in
// the flat SOURCE space -- a checkerboard floor (filled cells), a grid of stroked
// lines, a filled star motif, and a text label -- and the homography foreshortens
// it.  No images or gradients: sampling stays affine this phase, so the scene
// exercises only projective fills/strokes/text.
static void perspective_scene(void) {
    struct canvas2d_context *__single c = canvas2d(360, 240, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/perspective.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.07f, 0.08f, 0.11f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 360.0f, 240.0f);

    // The source rect is a 0..S unit floor; the destination quad recedes toward
    // the top (a far edge narrower than the near edge), corner order TL,TR,BR,BL.
    float const S = 8.0f;
    canvas2d_set_perspective_quad(c, 0.0f, 0.0f, S, S,
                                130.0f, 70.0f,    // TL (far-left)
                                230.0f, 70.0f,    // TR (far-right)
                                340.0f, 220.0f,   // BR (near-right)
                                20.0f,  220.0f);  // BL (near-left)

    // Checkerboard floor: fill alternating source cells.  Each fill is a flat
    // source-space rect the homography foreshortens.
    int const n = (int)S;
    for (int gy = 0; gy < n; gy++) {
        for (int gx = 0; gx < n; gx++) {
            bool const dark = ((gx + gy) & 1) != 0;
            canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB,
                                 dark ? 0.16f : 0.28f,
                                 dark ? 0.18f : 0.34f,
                                 dark ? 0.24f : 0.46f, 1.0f);
            canvas2d_fill_rect(c, (float)gx, (float)gy, 1.0f, 1.0f);
        }
    }

    // Grid lines over the floor (stroked in source space, width constant in
    // source units so it foreshortens with the plane).
    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.55f, 0.62f, 0.78f, 0.9f);
    canvas2d_set_line_width(c, 0.05f);
    canvas2d_begin_path(c);
    for (int k = 0; k <= n; k++) {
        canvas2d_move_to(c, (float)k, 0.0f);
        canvas2d_line_to(c, (float)k, S);
        canvas2d_move_to(c, 0.0f, (float)k);
        canvas2d_line_to(c, S, (float)k);
    }
    canvas2d_stroke(c);

    // A filled star motif standing on the floor, plus its outline.
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.95f, 0.78f, 0.30f, 1.0f);
    star(c, S * 0.5f, S * 0.5f, S * 0.28f);
    canvas2d_fill(c, CANVAS2D_NONZERO);
    canvas2d_set_stroke_rgba(c, CANVAS2D_CS_SRGB, 0.20f, 0.14f, 0.04f, 1.0f);
    canvas2d_set_line_width(c, 0.04f);
    star(c, S * 0.5f, S * 0.5f, S * 0.28f);
    canvas2d_stroke(c);

    // Text laid on the projected plane (small source-space size; foreshortened).
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.92f, 0.95f, 1.0f, 1.0f);
    canvas2d_set_font_size(c, 0.9f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
    canvas2d_fill_text(c, "perspective", S * 0.5f, S - 0.6f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);

    save(c, "gallery/perspective.png");
}

// Perspective-correct SAMPLING (P2): an image and a gradient mapped onto receding
// quads via set_perspective_quad.  The left quad textures an 8x8 checkerboard
// onto a floor that recedes toward the top -- the squares foreshorten (they get
// smaller toward the far edge), the giveaway of perspective-correct sampling: an
// affine warp would keep every row the same size.  The right quad fills a linear
// gradient running along the source's depth axis; the gradient bands likewise
// crowd toward the horizon (the parameter is solved in user space per pixel, not
// linearly across the device row).
static void perspectivetexture(void) {
    struct canvas2d_context *__single c = canvas2d(360, 240, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/perspectivetexture.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.07f, 0.08f, 0.11f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 360.0f, 240.0f);

    // An 8x8 checkerboard texture, one device-ish texel per source cell so the
    // foreshortening reads as the cells shrinking, not as filtering.
    int const T = 64;  // 8 cells * 8 px
    uint8_t tex[64 * 64 * 4];
    for (int y = 0; y < T; y++) {
        for (int x = 0; x < T; x++) {
            int const i = (y * T + x) * 4;
            bool const dark = (((x / 8) + (y / 8)) & 1) != 0;
            tex[i + 0] = dark ? 40  : 230;
            tex[i + 1] = dark ? 70  : 210;
            tex[i + 2] = dark ? 130 : 120;
            tex[i + 3] = 255;
        }
    }

    // Left half: the checkerboard on a floor receding toward the top.  The CTM
    // maps the texture's own 0..T source rect onto the destination quad, so a
    // plain 1:1 draw paints the whole texture across it, sampled perspective-
    // correctly.
    canvas2d_save(c);
    canvas2d_set_perspective_quad(c, 0.0f, 0.0f, (float)T, (float)T,
                                55.0f,  60.0f,    // TL (far-left)
                                135.0f, 60.0f,    // TR (far-right)
                                165.0f, 215.0f,   // BR (near-right)
                                20.0f,  215.0f);  // BL (near-left)
    canvas2d_draw_bitmap(c, CANVAS2D_CS_SRGB, tex, T, T, 0.0f, 0.0f);
    canvas2d_restore(c);

    // Right half: a linear gradient down the source depth axis on a matching
    // receding quad.  The gradient is defined in source space (0..T along y),
    // so its bands foreshorten with the plane.
    canvas2d_save(c);
    canvas2d_set_perspective_quad(c, 0.0f, 0.0f, (float)T, (float)T,
                                225.0f, 60.0f,    // TL (far-left)
                                305.0f, 60.0f,    // TR (far-right)
                                340.0f, 215.0f,   // BR (near-right)
                                195.0f, 215.0f);  // BL (near-left)
    canvas2d_set_fill_linear_gradient(c, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL,
                                    0.0f, 0.0f, 0.0f, (float)T);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.0f, 0.95f, 0.85f, 0.30f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 0.5f, 0.85f, 0.25f, 0.35f, 1.0f);
    canvas2d_add_fill_color_stop(c, CANVAS2D_CS_SRGB, 1.0f, 0.20f, 0.30f, 0.75f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, (float)T, (float)T);
    canvas2d_restore(c);

    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.92f, 0.95f, 1.0f, 1.0f);
    canvas2d_set_font_family(c, "Libian TC");
    canvas2d_set_font_size(c, 14.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_CENTER);
    canvas2d_fill_text(c, "perspectivetexture", 180.0f, 232.0f);
    canvas2d_set_text_align(c, CANVAS2D_ALIGN_START);

    save(c, "gallery/perspectivetexture.png");
}

// fontFamily: the same line drawn in the default Libian TC plus three macOS
// system fonts.  Each family records its own glyph curves into the program (the
// Libian model), so the scene replays on a fontless machine; an unavailable
// family would fall back through Core Text and record under the resolved name.
static void fontfamily(void) {
    struct canvas2d_context *__single c = canvas2d(420, 230, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/fontfamily.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 420.0f, 230.0f);

    char const *__null_terminated families[4] = {
        "Libian TC", "Helvetica", "Georgia", "Menlo",
    };
    char const *__null_terminated sample = "Typeface 字體 123";

    for (int i = 0; i < 4; i++) {
        float const y = 36.0f + (float)i * 50.0f;
        // The family label, in the default face at a small size.
        canvas2d_set_font_family(c, "Libian TC");
        canvas2d_set_font_size(c, 12.0f);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.55f, 0.58f, 0.66f, 1.0f);
        canvas2d_fill_text(c, families[i], 20.0f, y - 18.0f);
        // The sample line, in this family.
        canvas2d_set_font_family(c, families[i]);
        canvas2d_set_font_size(c, 30.0f);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.92f, 0.93f, 0.96f, 1.0f);
        canvas2d_fill_text(c, sample, 20.0f, y + 14.0f);
    }

    save(c, "gallery/fontfamily.png");
}

// Font WEIGHT and STYLE as canvas state.  Top rows: Helvetica (a family with
// real bold and italic faces) drawn regular / bold / italic / bold-italic, each
// labelled -- the weight trait matches Helvetica-Bold's heavier outlines and the
// italic trait Helvetica-Oblique's slanted ones.  Bottom row: Papyrus, a family
// with no bold or italic face, so its italic is SYNTHESIZED (Core Text has no
// real oblique; the boundary bakes a slant into the font matrix, which the
// recorded glyph curves capture) -- it reads as a slanted Papyrus, distinct from
// the upright regular.  Each face records its glyph curves (the Libian model), so
// the .canvas replays fontless byte-for-byte.
static void fontstyle(void) {
    struct canvas2d_context *__single c = canvas2d(440, 250, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/fontstyle.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 440.0f, 250.0f);

    char const *__null_terminated sample = "Regular bold italic";

    struct face { char const *__null_terminated label; int weight; enum canvas2d_font_style style; };
    struct face const faces[4] = {
        { "Helvetica regular",     400, CANVAS2D_FONT_STYLE_NORMAL },
        { "Helvetica bold",        700, CANVAS2D_FONT_STYLE_NORMAL },
        { "Helvetica italic",      400, CANVAS2D_FONT_STYLE_ITALIC },
        { "Helvetica bold italic", 700, CANVAS2D_FONT_STYLE_ITALIC },
    };
    for (int i = 0; i < 4; i++) {
        float const y = 34.0f + (float)i * 44.0f;
        // The label, in the default upright face at a small size.
        canvas2d_set_font_family(c, "Helvetica");
        canvas2d_set_font_weight(c, 400);
        canvas2d_set_font_style(c, CANVAS2D_FONT_STYLE_NORMAL);
        canvas2d_set_font_size(c, 11.0f);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.55f, 0.58f, 0.66f, 1.0f);
        canvas2d_fill_text(c, faces[i].label, 18.0f, y - 16.0f);
        // The sample, in this weight/style.
        canvas2d_set_font_weight(c, faces[i].weight);
        canvas2d_set_font_style(c, faces[i].style);
        canvas2d_set_font_size(c, 26.0f);
        canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.92f, 0.93f, 0.96f, 1.0f);
        canvas2d_fill_text(c, sample, 18.0f, y + 9.0f);
    }

    // The synthesis row: Papyrus has no real italic face, so italic is
    // synthesized (a slanted Papyrus) -- visibly distinct from the upright.
    float const y = 34.0f + 4.0f * 44.0f;
    canvas2d_set_font_family(c, "Helvetica");
    canvas2d_set_font_weight(c, 400);
    canvas2d_set_font_style(c, CANVAS2D_FONT_STYLE_NORMAL);
    canvas2d_set_font_size(c, 11.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.55f, 0.58f, 0.66f, 1.0f);
    canvas2d_fill_text(c, "Papyrus regular | synth italic", 18.0f, y - 16.0f);
    canvas2d_set_font_family(c, "Papyrus");
    canvas2d_set_font_size(c, 22.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.92f, 0.93f, 0.96f, 1.0f);
    canvas2d_set_font_style(c, CANVAS2D_FONT_STYLE_NORMAL);
    canvas2d_fill_text(c, "Papyrus", 18.0f, y + 7.0f);
    canvas2d_set_font_style(c, CANVAS2D_FONT_STYLE_ITALIC);
    canvas2d_fill_text(c, "Papyrus", 230.0f, y + 7.0f);

    save(c, "gallery/fontstyle.png");
}

// The shaping-attribute toggles (fontKerning / textRendering / lang) as canvas
// state.  Each row draws the same string twice -- once at the AUTO default, once
// with the toggle changed -- so the observable effect reads side by side:
//   - fontKerning: a kerning-sensitive Helvetica string with kerning AUTO
//     (Core Text's default pair kerning, advances tightened) vs NONE (kerning
//     off, the looser unkerned advances).  The "AVA To Wa" gaps visibly open.
//   - textRendering: a Hoefler Text string (a face with default kerning AND
//     ligatures) at AUTO vs optimizeSpeed, which disables both -- the advance
//     loosens and the ffi/fl ligatures break into separate glyphs.
//   - lang: PingFang SC shaping the Han characters 骨/誤 under zh-Hans vs
//     zh-Hant, whose locale-specific glyph forms differ (Han unification).
// The defaults (AUTO / AUTO / "") reproduce today's shaping, so each row's first
// draw is exactly what the canvas drew before this feature.  Every face records
// its glyph curves (the Libian model), so the .canvas replays fontless.
static void fonttoggles(void) {
    struct canvas2d_context *__single c = canvas2d(560, 240, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/fonttoggles.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 560.0f, 240.0f);

    float const lbl_r = 0.55f, lbl_g = 0.58f, lbl_b = 0.66f;
    float const ink_r = 0.92f, ink_g = 0.93f, ink_b = 0.96f;

    // Row 1: fontKerning AUTO vs NONE (Helvetica, a kerned face).
    canvas2d_set_font_family(c, "Helvetica");
    canvas2d_set_font_size(c, 11.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, lbl_r, lbl_g, lbl_b, 1.0f);
    canvas2d_fill_text(c, "fontKerning: auto (kerned)  vs  none (unkerned)", 18.0f, 28.0f);
    canvas2d_set_font_size(c, 30.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, ink_r, ink_g, ink_b, 1.0f);
    canvas2d_set_font_kerning(c, CANVAS2D_FONT_KERNING_AUTO);
    canvas2d_fill_text(c, "AVA To Wa", 18.0f, 62.0f);
    canvas2d_set_font_kerning(c, CANVAS2D_FONT_KERNING_NONE);
    canvas2d_fill_text(c, "AVA To Wa", 300.0f, 62.0f);
    canvas2d_set_font_kerning(c, CANVAS2D_FONT_KERNING_AUTO);  // back to default

    // Row 2: textRendering AUTO vs optimizeSpeed (Hoefler Text: kerning + ligatures).
    canvas2d_set_font_family(c, "Hoefler Text");
    canvas2d_set_font_size(c, 11.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, lbl_r, lbl_g, lbl_b, 1.0f);
    canvas2d_fill_text(c, "textRendering: auto  vs  optimizeSpeed (no kerning/ligatures)",
                     18.0f, 112.0f);
    canvas2d_set_font_size(c, 30.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, ink_r, ink_g, ink_b, 1.0f);
    canvas2d_set_text_rendering(c, CANVAS2D_TEXT_RENDERING_AUTO);
    canvas2d_fill_text(c, "office Wa", 18.0f, 146.0f);
    canvas2d_set_text_rendering(c, CANVAS2D_TEXT_RENDERING_OPTIMIZE_SPEED);
    canvas2d_fill_text(c, "office Wa", 300.0f, 146.0f);
    canvas2d_set_text_rendering(c, CANVAS2D_TEXT_RENDERING_AUTO);  // back to default

    // Row 3: lang zh-Hans vs zh-Hant (PingFang SC: locale-specific Han forms).
    canvas2d_set_font_family(c, "PingFang SC");
    canvas2d_set_font_size(c, 11.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, lbl_r, lbl_g, lbl_b, 1.0f);
    canvas2d_fill_text(c, "lang: zh-Hans  vs  zh-Hant (locale glyph forms)", 18.0f, 196.0f);
    canvas2d_set_font_size(c, 34.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, ink_r, ink_g, ink_b, 1.0f);
    canvas2d_set_lang(c, "zh-Hans");
    canvas2d_fill_text(c, "\xE9\xAA\xA8\xE8\xAA\xA4", 18.0f, 232.0f);   // 骨誤
    canvas2d_set_lang(c, "zh-Hant");
    canvas2d_fill_text(c, "\xE9\xAA\xA8\xE8\xAA\xA4", 300.0f, 232.0f);  // 骨誤
    canvas2d_set_lang(c, "");  // back to default

    save(c, "gallery/fonttoggles.png");
}

// fontVariantCaps and fontStretch as canvas state.  Each row draws the same
// string twice -- once at the NORMAL default, once with the attribute changed --
// so the observable effect reads side by side:
//   - fontVariantCaps: Baskerville (a face carrying the smcp small-cap feature)
//     drawn NORMAL vs SMALL_CAPS -- the lowercase letters become small capitals,
//     a different set of glyph ids at slightly different advances.
//   - fontStretch: Futura drawn NORMAL vs CONDENSED -- the width trait resolves
//     Futura-CondensedMedium, a real narrower face (a distinct resolved name, so
//     the glyph cache separates it), so the same string draws visibly tighter.
// The defaults (NORMAL / NORMAL) reproduce today's shaping, so each row's first
// draw is exactly what the canvas drew before this feature.  Both effects come
// from real faces/features the recorded glyph curves capture (the Libian model),
// so the .canvas replays fontless.
static void fontvariants(void) {
    struct canvas2d_context *__single c = canvas2d(560, 180, CANVAS2D_CS_SRGB);
    if (!c) {
        return;
    }
    record_scene(c, "gallery/fontvariants.canvas");
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas2d_fill_rect(c, 0.0f, 0.0f, 560.0f, 180.0f);

    float const lbl_r = 0.55f, lbl_g = 0.58f, lbl_b = 0.66f;
    float const ink_r = 0.92f, ink_g = 0.93f, ink_b = 0.96f;

    // Row 1: fontVariantCaps NORMAL vs SMALL_CAPS (Baskerville, an smcp face).
    canvas2d_set_font_family(c, "Baskerville");
    canvas2d_set_font_size(c, 11.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, lbl_r, lbl_g, lbl_b, 1.0f);
    canvas2d_fill_text(c, "fontVariantCaps: normal  vs  small-caps", 18.0f, 36.0f);
    canvas2d_set_font_size(c, 30.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, ink_r, ink_g, ink_b, 1.0f);
    canvas2d_set_font_variant_caps(c, CANVAS2D_FONT_VARIANT_CAPS_NORMAL);
    canvas2d_fill_text(c, "Chapter", 18.0f, 74.0f);
    canvas2d_set_font_variant_caps(c, CANVAS2D_FONT_VARIANT_CAPS_SMALL_CAPS);
    canvas2d_fill_text(c, "Chapter", 300.0f, 74.0f);
    canvas2d_set_font_variant_caps(c, CANVAS2D_FONT_VARIANT_CAPS_NORMAL);  // back to default

    // Row 2: fontStretch NORMAL vs CONDENSED (Futura, with a real condensed face).
    canvas2d_set_font_family(c, "Futura");
    canvas2d_set_font_size(c, 11.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, lbl_r, lbl_g, lbl_b, 1.0f);
    canvas2d_fill_text(c, "fontStretch: normal  vs  condensed", 18.0f, 122.0f);
    canvas2d_set_font_size(c, 30.0f);
    canvas2d_set_fill_rgba(c, CANVAS2D_CS_SRGB, ink_r, ink_g, ink_b, 1.0f);
    canvas2d_set_font_stretch(c, CANVAS2D_FONT_STRETCH_NORMAL);
    canvas2d_fill_text(c, "Magazine", 18.0f, 160.0f);
    canvas2d_set_font_stretch(c, CANVAS2D_FONT_STRETCH_CONDENSED);
    canvas2d_fill_text(c, "Magazine", 300.0f, 160.0f);
    canvas2d_set_font_stretch(c, CANVAS2D_FONT_STRETCH_NORMAL);  // back to default

    save(c, "gallery/fontvariants.png");
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
    textspacing();
    porterduff();
    hittest();
    blend();
    shadows();
    emoji();
    emojiscale();
    extendedrange();
    imagedataf16();
    imagecolorspace();
    imagescale();
    shaping();
    rtl();
    selection();
    filters();
    linearlight();
    gradinterp();
    colorspaces();
    ellipserot();
    nestedclip();
    perspective_scene();
    perspectivetexture();
    fontfamily();
    fontstyle();
    fonttoggles();
    fontvariants();
}

int main(void) {
    int const reps = gallery_reps();
    // GALLERY_NO_SAVE suppresses ALL writes: a profiling run (profile_scene.sh)
    // gets sampled then killed, and the renderer can finish
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
