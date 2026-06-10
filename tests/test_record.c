// canvas_record_to: a drawing session recorded to text and replayed back is
// identical, two ways.  (1) Pixel identity: record a program, replay the file
// onto a fresh canvas, the bitmaps match byte-for-byte.  (2) Text idempotence:
// replaying the file while recording reproduces the file byte-for-byte -- so the
// recorder and the replay parser agree on every command's spelling and argument
// order (a drift guard).  Built under -fbounds-safety like the rest of the suite;
// the recorder is the write-side counterpart to cnvs_replay.c.  See
// docs/decisions/security-review.md.

#include "test_util.h"

#include "canvas.h"

#include <ptrcheck.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 48
#define H 32
#define NPX (W * H * 4)

// Every recordable command, and each enum value, at least once.  Coordinates and
// colours are integers / simple decimals that round-trip exactly through %.9g and
// the parser's number reader, so record -> replay is bit-identical.
static void draw_program(canvas *__single cv) {
    canvas_save(cv);

    // An opaque source-over fill up front, so the bitmap is non-trivial before
    // any later clip or blend mode narrows things.
    canvas_set_fill_rgba(cv, 0.5f, 0.25f, 0.75f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);

    // Transforms.
    canvas_translate(cv, 4.0f, 8.0f);
    canvas_scale(cv, 2.0f, 2.0f);
    canvas_rotate(cv, 0.5f);
    canvas_transform(cv, 1.0f, 0.0f, 0.0f, 1.0f, 3.0f, 5.0f);
    canvas_set_transform(cv, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    canvas_reset_transform(cv);

    // Global state + blend mode + fill rule.
    canvas_set_global_alpha(cv, 0.75f);
    canvas_set_global_composite_operation(cv, CANVAS_OP_MULTIPLY);
    canvas_set_fill_rule(cv, CANVAS_EVENODD);

    canvas_clear_rect(cv, 1.0f, 1.0f, 2.0f, 2.0f);

    // Fill gradients (linear + radial) with stops.
    canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, 16.0f, 16.0f);
    canvas_add_fill_color_stop(cv, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_add_fill_color_stop(cv, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_set_fill_radial_gradient(cv, 8.0f, 8.0f, 0.0f, 8.0f, 8.0f, 16.0f);
    canvas_add_fill_color_stop(cv, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f);

    // Stroke paints (solid, then both gradient forms) + line styles.
    canvas_set_stroke_rgba(cv, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas_set_stroke_linear_gradient(cv, 0.0f, 0.0f, 32.0f, 0.0f);
    canvas_add_stroke_color_stop(cv, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_set_stroke_radial_gradient(cv, 4.0f, 4.0f, 1.0f, 4.0f, 4.0f, 8.0f);
    canvas_add_stroke_color_stop(cv, 1.0f, 0.5f, 0.5f, 0.5f, 1.0f);
    canvas_set_line_width(cv, 2.0f);
    canvas_set_line_join(cv, CANVAS_JOIN_ROUND);
    canvas_set_line_cap(cv, CANVAS_CAP_SQUARE);
    canvas_set_miter_limit(cv, 4.0f);
    canvas_set_line_dash(cv, (float[]){ 4.0f, 2.0f, 1.0f, 3.0f }, 4);
    canvas_set_line_dash_offset(cv, 1.0f);

    // Path building: every segment kind plus the compound helpers (arc,
    // round_rect, arc_to) that record as themselves and swallow their expansion.
    canvas_begin_path(cv);
    canvas_move_to(cv, 2.0f, 2.0f);
    canvas_line_to(cv, 30.0f, 2.0f);
    canvas_quadratic_curve_to(cv, 31.0f, 12.0f, 30.0f, 22.0f);
    canvas_bezier_curve_to(cv, 20.0f, 24.0f, 10.0f, 24.0f, 2.0f, 22.0f);
    canvas_rect(cv, 5.0f, 5.0f, 4.0f, 4.0f);
    canvas_arc(cv, 16.0f, 12.0f, 6.0f, 0.0f, 3.0f, false);
    canvas_ellipse(cv, 16.0f, 12.0f, 8.0f, 4.0f, 0.25f, 0.0f, 3.0f, true);
    canvas_arc_to(cv, 2.0f, 2.0f, 8.0f, 2.0f, 3.0f);
    canvas_round_rect(cv, 1.0f, 1.0f, 10.0f, 10.0f, 2.0f);
    canvas_close_path(cv);
    canvas_fill(cv);
    canvas_stroke(cv);

    // Text (Latin + CJK, no leading whitespace / newline so it round-trips).
    // The text ops also emit their font/glyph/shape blocks (see canvas.h), so
    // this pins the blocks' spellings against the parser too.
    canvas_set_font_size(cv, 12.0f);
    canvas_set_fill_rgba(cv, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_text(cv, "Hi 隸", 4.0f, 20.0f);
    canvas_set_text_align(cv, CANVAS_ALIGN_CENTER);
    canvas_set_text_baseline(cv, CANVAS_BASELINE_MIDDLE);
    canvas_stroke_text(cv, "yo", 4.0f, 30.0f);

    // Image ops: one 4x3 source through every form -- the three draw_image
    // overloads, both putImageData forms, and both pattern paints.  The same
    // buffer every time, so the file carries ONE content-deduped image block
    // (the idempotence compare below pins the dedupe and the id assignment).
    uint8_t img[4 * 3 * 4];
    for (int i = 0; i < (int)sizeof img; i++) {
        img[i] = (uint8_t)(i * 5);
    }
    canvas_draw_image(cv, img, 4, 3, 2.0f, 2.0f);
    canvas_draw_image_scaled(cv, img, 4, 3, 8.0f, 2.0f, 8.0f, 6.0f);
    canvas_draw_image_subrect(cv, img, 4, 3, 1.0f, 1.0f, 2.0f, 2.0f,
                              18.0f, 2.0f, 6.0f, 6.0f);
    canvas_put_image_data(cv, img, (int)sizeof img, 4, 3, 26, 2);
    canvas_put_image_data_dirty(cv, img, (int)sizeof img, 4, 3, 32, 2,
                                1, 1, 2, 2);
    canvas_set_fill_pattern(cv, img, 4, 3, CANVAS_REPEAT);
    canvas_fill_rect(cv, 2.0f, 24.0f, 10.0f, 6.0f);
    canvas_set_stroke_pattern(cv, img, 4, 3, CANVAS_REPEAT_X);
    canvas_stroke_rect(cv, 16.0f, 24.0f, 10.0f, 6.0f);
    canvas_set_fill_rgba(cv, 0.2f, 0.6f, 0.4f, 1.0f);
    canvas_set_stroke_rgba(cv, 0.0f, 0.0f, 0.0f, 1.0f);

    // Path2D: every builder command in one path, drawn by all three ops --
    // twice each for fill/stroke, so the content dedupe pins ONE `path` block
    // (the idempotence compare again) -- plus the two hit-test overloads,
    // which are queries and must record nothing.
    canvas_path2d *__single p2 = canvas_path2d_create();
    if (p2) {
        canvas_path2d_move_to(p2, 20.0f, 4.0f);
        canvas_path2d_line_to(p2, 30.0f, 4.0f);
        canvas_path2d_quadratic_curve_to(p2, 32.0f, 8.0f, 30.0f, 12.0f);
        canvas_path2d_bezier_curve_to(p2, 28.0f, 14.0f, 24.0f, 14.0f,
                                      22.0f, 12.0f);
        canvas_path2d_arc_to(p2, 20.0f, 12.0f, 20.0f, 8.0f, 2.0f);
        canvas_path2d_close_path(p2);
        canvas_path2d_arc(p2, 25.0f, 8.0f, 2.0f, 0.0f, 3.0f, true);
        canvas_path2d_ellipse(p2, 25.0f, 8.0f, 4.0f, 2.0f, 0.25f, 0.0f, 3.0f,
                              false);
        canvas_path2d_rect(p2, 21.0f, 5.0f, 3.0f, 3.0f);
        canvas_path2d_round_rect(p2, 26.0f, 5.0f, 4.0f, 4.0f, 1.0f);
        canvas_fill_path(cv, p2, CANVAS_EVENODD);
        canvas_fill_path(cv, p2, CANVAS_NONZERO);
        canvas_set_line_width(cv, 1.0f);
        canvas_stroke_path(cv, p2);
        canvas_stroke_path(cv, p2);
        (void)canvas_is_point_in_path2d(cv, p2, 25.0f, 8.0f, CANVAS_NONZERO);
        (void)canvas_is_point_in_stroke_path(cv, p2, 20.0f, 4.0f);
        canvas_save(cv);
        canvas_clip_path(cv, p2, CANVAS_NONZERO);
        canvas_set_fill_rgba(cv, 0.9f, 0.4f, 0.1f, 1.0f);
        canvas_fill_rect(cv, 18.0f, 2.0f, 16.0f, 14.0f);
        canvas_restore(cv);
        canvas_path2d_destroy(p2);
    }

    // A clip last, so a non-empty path region is exercised by the clip command.
    canvas_begin_path(cv);
    canvas_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_clip(cv);

    canvas_restore(cv);
}

// Read up to cap bytes of `path` into buf; byte count, or -1 if it won't open.
static int slurp(char const *__null_terminated path, char *__counted_by(cap) buf,
                 int cap) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    size_t got = fread(buf, 1, (size_t)cap, f);
    (void)fclose(f);
    return (int)got;
}

int main(void) {
    // Fixed paths under build/ (the test_png convention); the recorded text is
    // deterministic and variant-independent.
    char const *__null_terminated p1 = "build/test_record_a.canvas";
    char const *__null_terminated p2 = "build/test_record_b.canvas";

    // Opening an unwritable path records nothing and reports failure.
    {
        canvas *__single bad = canvas_create(W, H);
        CHECK(bad != NULL);
        CHECK(!canvas_record_to(bad, "build/no_such_dir_xyzzy/out.canvas"));
        canvas_destroy(bad);
    }

    // 1. Record a program to p1, capturing its pixels before destroy closes p1.
    uint8_t recorded_px[NPX];
    {
        canvas *__single cv = canvas_create(W, H);
        CHECK(cv != NULL);
        CHECK(canvas_record_to(cv, p1));
        draw_program(cv);
        canvas_read_rgba(cv, recorded_px, (int)sizeof recorded_px);
        canvas_destroy(cv);  // flush + close p1
    }

    // The program actually drew something (not a blank bitmap).
    {
        bool any = false;
        for (int i = 0; i < NPX; i++) {
            if (recorded_px[i] != 0) { any = true; break; }
        }
        CHECK(any);
    }

    // 2. Pixel identity: replay p1 onto a fresh canvas; bitmaps match exactly.
    {
        canvas *__single cv = canvas_create(W, H);
        CHECK(cv != NULL);
        CHECK(canvas_replay_from(cv, p1));
        uint8_t replayed_px[NPX];
        canvas_read_rgba(cv, replayed_px, (int)sizeof replayed_px);
        CHECK(memcmp(recorded_px, replayed_px, sizeof recorded_px) == 0);
        canvas_destroy(cv);
    }

    // 3. Text idempotence: replay p1 while recording into p2; p1 == p2 byte-for-
    // byte, proving record/replay are inverse on the canonical text form.
    {
        canvas *__single cv = canvas_create(W, H);
        CHECK(cv != NULL);
        CHECK(canvas_record_to(cv, p2));
        CHECK(canvas_replay_from(cv, p1));
        canvas_destroy(cv);  // flush + close p2

        char a[1 << 14];
        char b[1 << 14];
        int na = slurp(p1, a, (int)sizeof a);
        int nb = slurp(p2, b, (int)sizeof b);
        CHECK(na > 0);
        CHECK(na < (int)sizeof a);  // fit, so we compared the whole file
        CHECK(na == nb);
        if (na == nb && na > 0) {
            CHECK(memcmp(a, b, (size_t)na) == 0);
        }
    }

    return TEST_REPORT();
}
