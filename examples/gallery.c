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

// Batching: a dense field of translucent discs.  Every disc is its own
// canvas_fill -- hundreds of draws that batch into a single GPU command buffer,
// flushed once at write_png.  The alpha overlap also shows source-over ordering
// survives the batch.
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

// Text: Libian TC (clerical-script 隸書) glyph outlines from Core Text, rasterized
// by the same analytic-coverage fill as everything else -- so they take gradients,
// strokes, transforms, and alpha, and one fill_text mixes Latin and Chinese (UTF-8).
// The glyphs arrive as ordinary device-space paths from the unchecked C shim
// (cnvs_font_ct.c); nothing else gives up checking.
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

int main(void) {
    shapes();
    winding();
    dashes();
    imagedata();
    joinscaps();
    paths();
    clipping();
    gradients();
    batching();
    drawimage();
    text();
    return 0;
}
