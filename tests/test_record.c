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
#include "cnvs_replay.h"

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
static void draw_program(struct canvas *__single cv) {
    canvas_save(cv);

    // reset + resize first: both record as themselves (resize swallows the
    // reset it expands to), both clear the bitmap, and the rest of the
    // program then proves drawing state and pixels rebuild identically after
    // them.  Same dimensions, so the pixel-identity buffers stay W x H.
    canvas_fill_rect(cv, 0.0f, 0.0f, 8.0f, 8.0f);  // something for reset to wipe
    canvas_reset(cv);
    canvas_resize(cv, W, H);

    // An opaque source-over fill up front, so the bitmap is non-trivial before
    // any later clip or blend mode narrows things.
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.5f, 0.25f, 0.75f, 1.0f);
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

    canvas_clear_rect(cv, 1.0f, 1.0f, 2.0f, 2.0f);

    // Fill gradients (linear + radial) with stops.
    canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, 16.0f, 16.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_set_fill_radial_gradient(cv, 8.0f, 8.0f, 0.0f, 8.0f, 8.0f, 16.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas_set_fill_conic_gradient(cv, 0.5f, 16.0f, 12.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.5f, 0.0f, 1.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.5f, 1.0f, 1.0f);

    // Stroke paints (solid, then both gradient forms) + line styles.
    canvas_set_stroke_rgba(cv, CANVAS_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas_set_stroke_linear_gradient(cv, 0.0f, 0.0f, 32.0f, 0.0f);
    canvas_add_stroke_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_set_stroke_radial_gradient(cv, 4.0f, 4.0f, 1.0f, 4.0f, 4.0f, 8.0f);
    canvas_add_stroke_color_stop(cv, CANVAS_CS_SRGB, 1.0f, 0.5f, 0.5f, 0.5f, 1.0f);
    canvas_set_stroke_conic_gradient(cv, 0.25f, 16.0f, 12.0f);
    canvas_add_stroke_color_stop(cv, CANVAS_CS_SRGB, 0.5f, 1.0f, 1.0f, 0.0f, 1.0f);
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
    canvas_round_rect_radii(cv, 34.0f, 1.0f, 12.0f, 10.0f, 2.0f, 3.0f,
                            4.0f, 2.0f, 0.0f, 0.0f, 5.0f, 5.0f);
    canvas_close_path(cv);
    canvas_fill(cv, CANVAS_NONZERO);
    canvas_stroke(cv);

    // The filter list: every function once (the chain applies to the fill),
    // then cleared.
    canvas_set_filter_none(cv);
    canvas_add_filter_brightness(cv, 1.25f);
    canvas_add_filter_contrast(cv, 1.5f);
    canvas_add_filter_grayscale(cv, 0.5f);
    canvas_add_filter_hue_rotate(cv, 0.5f);
    canvas_add_filter_invert(cv, 0.25f);
    canvas_add_filter_opacity(cv, 0.75f);
    canvas_add_filter_saturate(cv, 2.0f);
    canvas_add_filter_sepia(cv, 0.5f);
    canvas_add_filter_blur(cv, 1.5f);
    canvas_add_filter_drop_shadow(cv, CANVAS_CS_SRGB, 2.0f, 2.0f, 1.0f, 0.25f, 0.5f, 0.75f, 0.5f);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.9f, 0.6f, 0.2f, 0.8f);
    canvas_fill_rect(cv, 36.0f, 14.0f, 8.0f, 8.0f);
    canvas_set_filter_none(cv);

    // Text (Latin + CJK, no leading whitespace / newline so it round-trips).
    // The text ops also emit their font/glyph/shape blocks (see canvas.h), so
    // this pins the blocks' spellings against the parser too.
    canvas_set_font_size(cv, 12.0f);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_text(cv, "Hi 隸", 4.0f, 20.0f);
    canvas_set_text_align(cv, CANVAS_ALIGN_CENTER);
    canvas_set_text_baseline(cv, CANVAS_BASELINE_MIDDLE);
    canvas_stroke_text(cv, "yo", 4.0f, 30.0f);
    canvas_fill_text_max(cv, "squeeze", 24.0f, 20.0f, 12.0f);
    canvas_stroke_text_max(cv, "squeeze", 24.0f, 30.0f, 12.0f);

    // Image ops: one 4x3 source through every form -- the three draw_image
    // overloads, both putImageData forms, and both pattern paints.  The same
    // buffer every time, so the file carries ONE content-deduped image block
    // (the idempotence compare below pins the dedupe and the id assignment).
    uint8_t img[4 * 3 * 4];
    for (int i = 0; i < (int)sizeof img; i++) {
        img[i] = (uint8_t)(i * 5);
    }
    canvas_set_image_smoothing_enabled(cv, false);
    canvas_set_image_smoothing_quality(cv, CANVAS_SMOOTHING_HIGH);
    canvas_draw_bitmap(cv, CANVAS_CS_SRGB, img, 4, 3, 2.0f, 2.0f);
    canvas_set_image_smoothing_enabled(cv, true);
    canvas_draw_bitmap_scaled(cv, CANVAS_CS_SRGB, img, 4, 3, 8.0f, 2.0f, 8.0f, 6.0f);
    canvas_draw_bitmap_subrect(cv, CANVAS_CS_SRGB, img, 4, 3, 1.0f, 1.0f, 2.0f, 2.0f,
                              18.0f, 2.0f, 6.0f, 6.0f);
    canvas_put_image_data(cv, CANVAS_CS_SRGB, img, (int)sizeof img, 4, 3, 26, 2);
    canvas_put_image_data_dirty(cv, CANVAS_CS_SRGB, img, (int)sizeof img, 4, 3, 32, 2,
                                1, 1, 2, 2);
    canvas_set_fill_pattern(cv, CANVAS_CS_SRGB, img, 4, 3, CANVAS_REPEAT);
    canvas_fill_rect(cv, 2.0f, 24.0f, 10.0f, 6.0f);
    canvas_set_stroke_pattern(cv, CANVAS_CS_SRGB, img, 4, 3, CANVAS_REPEAT_X);
    canvas_stroke_rect(cv, 16.0f, 24.0f, 10.0f, 6.0f);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.2f, 0.6f, 0.4f, 1.0f);
    canvas_set_stroke_rgba(cv, CANVAS_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);

    // Path2D: every builder command in one path, drawn by all three ops --
    // twice each for fill/stroke, so the content dedupe pins ONE `path` block
    // (the idempotence compare again) -- plus the two hit-test overloads,
    // which are queries and must record nothing.
    struct canvas_path2d *__single p2 = canvas_path2d();
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
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.9f, 0.4f, 0.1f, 1.0f);
        canvas_fill_rect(cv, 18.0f, 2.0f, 16.0f, 14.0f);
        canvas_restore(cv);
        canvas_path2d_free(p2);
    }

    // A clip last, so a non-empty path region is exercised by the clip command.
    canvas_begin_path(cv);
    canvas_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_clip(cv, CANVAS_NONZERO);

    canvas_restore(cv);
}

// Read up to cap bytes of `path` into buf; byte count, or -1 if it won't open.
static int slurp(char const *__null_terminated path, char *__counted_by(cap) buf,
                 int cap) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    size_t const got = fread(buf, 1, (size_t)cap, f);
    (void)fclose(f);
    return (int)got;
}

// Whether `hay[i, i+m)` equals the m bytes of `needle` (a counted-buffer
// memcmp that stays in the indexable world, no __null_terminated seam).
static bool eq_at(char const *__counted_by(n) hay, int n, int i,
                  char const *__counted_by(m) needle, int m) {
    if (i < 0 || i + m > n) {
        return false;
    }
    for (int k = 0; k < m; k++) {
        if (hay[i + k] != needle[k]) {
            return false;
        }
    }
    return true;
}

// Whether `hay` (a slurped recording) contains `needle` (a "<line>\n" literal,
// its trailing NUL excluded by sizeof-1) as a whole line -- anchored at start-
// of-file or a newline.  Used to assert a specific op line was (or was NOT)
// emitted.
#define HAS_LINE(hay, n, lit) has_line_n((hay), (n), (lit), (int)sizeof(lit) - 1)
static bool has_line_n(char const *__counted_by(n) hay, int n,
                       char const *__counted_by(m) needle, int m) {
    for (int i = 0; i + m <= n; i++) {
        if ((i == 0 || hay[i - 1] == '\n') && eq_at(hay, n, i, needle, m)) {
            return true;
        }
    }
    return false;
}

// Whether the recording contains NO occurrence of the bytes of `needle`
// anywhere -- used to prove an sRGB recording names no colour space at all.
#define HAS_NO_SUBSTR(hay, n, lit) has_no_substr_n((hay), (n), (lit), (int)sizeof(lit) - 1)
static bool has_no_substr_n(char const *__counted_by(n) hay, int n,
                            char const *__counted_by(m) needle, int m) {
    for (int i = 0; i + m <= n; i++) {
        if (eq_at(hay, n, i, needle, m)) {
            return false;
        }
    }
    return true;
}

// Record one color program (each tagged-color op once, in `space`) to `path`,
// then close.  Kept tiny and free of any other op so the file is easy to assert
// against line by line.
static void record_colors(char const *__null_terminated path,
                          enum canvas_color_space space) {
    struct canvas *__single cv = canvas(16, 16, CANVAS_CS_SRGB);
    CHECK(cv != NULL);
    CHECK(canvas_record_to(cv, path));
    canvas_set_fill_rgba(cv, space, 0.25f, 0.5f, 0.75f, 1.0f);
    canvas_set_stroke_rgba(cv, space, 0.125f, 0.25f, 0.375f, 0.5f);
    canvas_set_shadow_color_rgba(cv, space, 0.5f, 0.5f, 0.5f, 0.25f);
    canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, 16.0f, 16.0f);
    canvas_add_fill_color_stop(cv, space, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_set_stroke_linear_gradient(cv, 0.0f, 0.0f, 16.0f, 0.0f);
    canvas_add_stroke_color_stop(cv, space, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas_add_filter_drop_shadow(cv, space, 2.0f, 2.0f, 1.0f, 0.25f, 0.5f, 0.75f, 0.5f);
    // put_image_data: its colour space rides the BLOCK's optional tag.
    uint8_t img[2 * 2 * 4];
    for (int i = 0; i < (int)sizeof img; i++) { img[i] = (uint8_t)(i * 9); }
    canvas_put_image_data(cv, space, img, (int)sizeof img, 2, 2, 1, 1);
    canvas_free(cv);  // flush + close
}

int main(void) {
    // Fixed paths under build/ (the test_png convention); the recorded text is
    // deterministic and variant-independent.
    char const *__null_terminated p1 = "build/test_record_a.canvas";
    char const *__null_terminated p2 = "build/test_record_b.canvas";

    // Opening an unwritable path records nothing and reports failure.
    {
        struct canvas *__single bad = canvas(W, H, CANVAS_CS_SRGB);
        CHECK(bad != NULL);
        CHECK(!canvas_record_to(bad, "build/no_such_dir_xyzzy/out.canvas"));
        canvas_free(bad);
    }

    // 1. Record a program to p1, capturing its pixels before destroy closes p1.
    uint8_t recorded_px[NPX];
    {
        struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
        CHECK(cv != NULL);
        CHECK(canvas_record_to(cv, p1));
        draw_program(cv);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, recorded_px, (int)sizeof recorded_px);
        canvas_free(cv);  // flush + close p1
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
        struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
        CHECK(cv != NULL);
        CHECK(canvas_replay_from(cv, p1));
        uint8_t replayed_px[NPX];
        canvas_read_rgba(cv, CANVAS_CS_SRGB, replayed_px, (int)sizeof replayed_px);
        CHECK(memcmp(recorded_px, replayed_px, sizeof recorded_px) == 0);
        canvas_free(cv);
    }

    // 3. Text idempotence: replay p1 while recording into p2; p1 == p2 byte-for-
    // byte, proving record/replay are inverse on the canonical text form.
    {
        struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
        CHECK(cv != NULL);
        CHECK(canvas_record_to(cv, p2));
        CHECK(canvas_replay_from(cv, p1));
        canvas_free(cv);  // flush + close p2

        char a[1 << 14];
        char b[1 << 14];
        int const na = slurp(p1, a, (int)sizeof a);
        int const nb = slurp(p2, b, (int)sizeof b);
        CHECK(na > 0);
        CHECK(na < (int)sizeof a);  // fit, so we compared the whole file
        CHECK(na == nb);
        if (na == nb && na > 0) {
            CHECK(memcmp(a, b, (size_t)na) == 0);
        }
    }

    // 4. The working-space line: the same program on a LINEAR canvas records a
    // leading `working_space linear` line (an sRGB program leads with
    // `working_space srgb`, asserted in chunk 5a -- the line is written
    // unconditionally), replays to a linear canvas with pixel identity, and
    // round-trips byte-for-byte through replay-while-recording -- including
    // across the program's own reset/resize, which leave the immutable space
    // untouched.
    char const *__null_terminated lp1 = "build/test_record_lin_a.canvas";
    char const *__null_terminated lp2 = "build/test_record_lin_b.canvas";
    uint8_t lin_px[NPX];
    {
        struct canvas *__single cv = canvas(W, H, CANVAS_CS_LINEAR_SRGB);
        CHECK(cv != NULL);
        CHECK(canvas_record_to(cv, lp1));
        draw_program(cv);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, lin_px, (int)sizeof lin_px);
        canvas_free(cv);
    }
    {
        // The leading line is present and names linear, and the linear render
        // differs from the sRGB render of the identical program (p1's pixels).
        char buf[64];
        int const nb = slurp(lp1, buf, (int)sizeof buf);
        CHECK(nb >= 21);
        CHECK(memcmp(buf, "working_space linear\n", 21) == 0);
        CHECK(memcmp(recorded_px, lin_px, sizeof recorded_px) != 0);
    }
    {
        // Pixel identity: a fresh sRGB canvas, replay flips it to linear via the
        // leading line, and the bitmap matches the recorded linear render.
        struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
        CHECK(cv != NULL);
        CHECK(canvas_replay_from(cv, lp1));
        uint8_t replayed[NPX];
        canvas_read_rgba(cv, CANVAS_CS_SRGB, replayed, (int)sizeof replayed);
        CHECK(memcmp(lin_px, replayed, sizeof lin_px) == 0);
        canvas_free(cv);
    }
    {
        // Byte idempotence: replay lp1 while recording lp2; lp1 == lp2.
        struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
        CHECK(cv != NULL);
        CHECK(canvas_record_to(cv, lp2));
        CHECK(canvas_replay_from(cv, lp1));
        canvas_free(cv);

        char a[1 << 14];
        char b[1 << 14];
        int const na = slurp(lp1, a, (int)sizeof a);
        int const nb = slurp(lp2, b, (int)sizeof b);
        CHECK(na > 0 && na < (int)sizeof a && na == nb);
        if (na == nb && na > 0) {
            CHECK(memcmp(a, b, (size_t)na) == 0);
        }
    }

    // 5. The optional per-color space token.  Per color op, BOTH ways:
    //   (a) untagged: an sRGB colour records with NO trailing token (byte-
    //       identical to before this chunk) and replays back to sRGB; and
    //   (b) tagged: a non-sRGB colour emits the token, round-trips, and is
    //       byte-idempotent under record -> replay -> re-record.
    char const *__null_terminated cp_srgb = "build/test_record_cs_srgb.canvas";
    char const *__null_terminated cp_lin  = "build/test_record_cs_lin.canvas";
    char const *__null_terminated cp_okl  = "build/test_record_cs_okl.canvas";
    char const *__null_terminated cp_re   = "build/test_record_cs_re.canvas";

    {
        // (a) Untagged sRGB: every colour op line is exactly the floats, no
        // trailing token -- so a colour op stays byte-identical whether or not
        // the working space is sRGB.  put_image_data carries its space on the
        // image BLOCK, so an sRGB block emits no token either (the block line
        // stays `... <zlen> <nlines>`).  The file leads with the unconditional
        // working-space line, naming srgb here.
        record_colors(cp_srgb, CANVAS_CS_SRGB);
        char buf[1 << 13];
        int const n = slurp(cp_srgb, buf, (int)sizeof buf);
        CHECK(n > 0 && n < (int)sizeof buf);
        CHECK(n >= 19 && memcmp(buf, "working_space srgb\n", 19) == 0);
        CHECK(HAS_LINE(buf, n, "set_fill_rgba 0.25 0.5 0.75 1\n"));
        CHECK(HAS_LINE(buf, n, "set_stroke_rgba 0.125 0.25 0.375 0.5\n"));
        CHECK(HAS_LINE(buf, n, "set_shadow_color_rgba 0.5 0.5 0.5 0.25\n"));
        CHECK(HAS_LINE(buf, n, "add_fill_color_stop 0 1 0 0 1\n"));
        CHECK(HAS_LINE(buf, n, "add_stroke_color_stop 1 0 1 0 1\n"));
        CHECK(HAS_LINE(buf, n, "add_filter_drop_shadow 2 2 1 0.25 0.5 0.75 0.5\n"));
        // No colour-op line carries a trailing space token: a tagged form of any
        // of them must be absent.  (The bare names "srgb"/"linear"/"oklab" can't
        // be probed directly here -- set_fill_linear_gradient legitimately spells
        // "linear", and the leading working_space line legitimately ends ` srgb` --
        // so assert the absence of the actual tagged op lines.)
        CHECK(HAS_NO_SUBSTR(buf, n, "set_fill_rgba 0.25 0.5 0.75 1 "));
        CHECK(HAS_NO_SUBSTR(buf, n, "set_stroke_rgba 0.125 0.25 0.375 0.5 "));
        CHECK(HAS_NO_SUBSTR(buf, n, "add_filter_drop_shadow 2 2 1 0.25 0.5 0.75 0.5 "));
        // No colour op ends with a non-sRGB token (the working_space line names
        // srgb, the put_image_data block line ends after its <nlines> count, the
        // op lines after their floats).
        CHECK(HAS_NO_SUBSTR(buf, n, " linear\n"));
        CHECK(HAS_NO_SUBSTR(buf, n, " oklab\n"));
        // It replays without error onto a fresh canvas.
        struct canvas *__single cv = canvas(16, 16, CANVAS_CS_SRGB);
        CHECK(cv != NULL);
        CHECK(canvas_replay_from(cv, cp_srgb));
        canvas_free(cv);
    }

    {
        // (b) Tagged linear: every colour op line carries the ` linear` token,
        // and the put_image_data image block carries it too.
        record_colors(cp_lin, CANVAS_CS_LINEAR_SRGB);
        char buf[1 << 13];
        int const n = slurp(cp_lin, buf, (int)sizeof buf);
        CHECK(n > 0 && n < (int)sizeof buf);
        CHECK(HAS_LINE(buf, n, "set_fill_rgba 0.25 0.5 0.75 1 linear\n"));
        CHECK(HAS_LINE(buf, n, "set_stroke_rgba 0.125 0.25 0.375 0.5 linear\n"));
        CHECK(HAS_LINE(buf, n, "set_shadow_color_rgba 0.5 0.5 0.5 0.25 linear\n"));
        CHECK(HAS_LINE(buf, n, "add_fill_color_stop 0 1 0 0 1 linear\n"));
        CHECK(HAS_LINE(buf, n, "add_stroke_color_stop 1 0 1 0 1 linear\n"));
        CHECK(HAS_LINE(buf, n, "add_filter_drop_shadow 2 2 1 0.25 0.5 0.75 0.5 linear\n"));
        // The put_image_data block names its space (the optional trailing block
        // token), proving the tag round-trips through pixel I/O too.  The
        // deflated byte count is content-dependent, so match the block prefix,
        // walk to end of line, and assert it ends with the ` linear` tag.
        {
            char const pfx[] = "image 0 unorm8 unpremul 2 2 ";
            char const tag[] = " linear";
            int const pm = (int)sizeof pfx - 1, tm = (int)sizeof tag - 1;
            bool found = false;
            for (int i = 0; i + pm <= n; i++) {
                if ((i == 0 || buf[i - 1] == '\n') && eq_at(buf, n, i, pfx, pm)) {
                    int e = i;
                    while (e < n && buf[e] != '\n') { e++; }
                    found = eq_at(buf, n, e - tm, tag, tm);
                    break;
                }
            }
            CHECK(found);
        }

        // Byte idempotence: replay while recording reproduces the file exactly,
        // so record and replay agree on the token's spelling and placement.
        struct canvas *__single cv = canvas(16, 16, CANVAS_CS_SRGB);
        CHECK(cv != NULL);
        CHECK(canvas_record_to(cv, cp_re));
        CHECK(canvas_replay_from(cv, cp_lin));
        canvas_free(cv);
        char buf2[1 << 13];
        int const n2 = slurp(cp_re, buf2, (int)sizeof buf2);
        CHECK(n2 == n);
        if (n2 == n) {
            CHECK(memcmp(buf, buf2, (size_t)n) == 0);
        }
    }

    {
        // (b') Tagged Oklab on the op-line colour ops (Oklab is a valid input
        // space; put_image_data, which only carries the working-space-like block
        // tag, is exercised by the linear case above).
        struct canvas *__single cv = canvas(16, 16, CANVAS_CS_SRGB);
        CHECK(cv != NULL);
        CHECK(canvas_record_to(cv, cp_okl));
        canvas_set_fill_rgba(cv, CANVAS_CS_OKLAB, 0.5f, 0.25f, 0.75f, 1.0f);
        canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, 16.0f, 16.0f);
        canvas_add_fill_color_stop(cv, CANVAS_CS_OKLAB, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_free(cv);
        char buf[1 << 13];
        int const n = slurp(cp_okl, buf, (int)sizeof buf);
        CHECK(n > 0 && n < (int)sizeof buf);
        CHECK(HAS_LINE(buf, n, "set_fill_rgba 0.5 0.25 0.75 1 oklab\n"));
        CHECK(HAS_LINE(buf, n, "add_fill_color_stop 0 1 0 0 1 oklab\n"));
        // Round-trips byte-identically.
        struct canvas *__single cv2 = canvas(16, 16, CANVAS_CS_SRGB);
        CHECK(cv2 != NULL);
        CHECK(canvas_record_to(cv2, cp_re));
        CHECK(canvas_replay_from(cv2, cp_okl));
        canvas_free(cv2);
        char buf2[1 << 13];
        int const n2 = slurp(cp_re, buf2, (int)sizeof buf2);
        CHECK(n2 == n);
        if (n2 == n) {
            CHECK(memcmp(buf, buf2, (size_t)n) == 0);
        }
    }

    // 7. Recording SUSPEND: each compound op records its own line, then brackets
    // the public sub-calls it expands into with cnvs_rec_enter/leave so those
    // sub-calls emit NOTHING while suspend != 0.  Record each compound op in
    // isolation and assert: its own line is present, and the expansion's lines
    // are absent (proving the suspend arms swallowed them).  This drives the
    // suspend != 0 branch in every cnvs_rec_* emitter.
    {
        char const *__null_terminated sp = "build/test_record_suspend.canvas";
        // Helper: record one drawing closure to sp via a tiny inline scope.
        // arc -> records `arc`, swallows the canvas_ellipse it expands to.
        {
            struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
            CHECK(cv != NULL);
            CHECK(canvas_record_to(cv, sp));
            canvas_begin_path(cv);
            canvas_arc(cv, 16.0f, 12.0f, 6.0f, 0.0f, 3.0f, false);
            canvas_free(cv);
            char b[1 << 13];
            int const n = slurp(sp, b, (int)sizeof b);
            CHECK(n > 0 && n < (int)sizeof b);
            CHECK(HAS_LINE(b, n, "arc 16 12 6 0 3 0\n"));   // its own line
            CHECK(HAS_NO_SUBSTR(b, n, "ellipse "));         // expansion swallowed
        }
        // round_rect -> records `round_rect`, swallows move_to/arc/close_path.
        {
            struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
            CHECK(cv != NULL);
            CHECK(canvas_record_to(cv, sp));
            canvas_begin_path(cv);
            canvas_round_rect(cv, 1.0f, 1.0f, 10.0f, 10.0f, 2.0f);
            canvas_free(cv);
            char b[1 << 13];
            int const n = slurp(sp, b, (int)sizeof b);
            CHECK(n > 0 && n < (int)sizeof b);
            CHECK(HAS_LINE(b, n, "round_rect 1 1 10 10 2\n"));
            CHECK(HAS_NO_SUBSTR(b, n, "move_to "));
            CHECK(HAS_NO_SUBSTR(b, n, "arc "));
            CHECK(HAS_NO_SUBSTR(b, n, "close_path"));
        }
        // round_rect_radii -> records itself, swallows its arc_to expansion.
        {
            struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
            CHECK(cv != NULL);
            CHECK(canvas_record_to(cv, sp));
            canvas_begin_path(cv);
            canvas_round_rect_radii(cv, 1.0f, 1.0f, 12.0f, 10.0f, 2.0f, 3.0f,
                                    4.0f, 2.0f, 0.0f, 0.0f, 5.0f, 5.0f);
            canvas_free(cv);
            char b[1 << 13];
            int const n = slurp(sp, b, (int)sizeof b);
            CHECK(n > 0 && n < (int)sizeof b);
            CHECK(HAS_LINE(b, n, "round_rect_radii 1 1 12 10 2 3 4 2 0 0 5 5\n"));
            CHECK(HAS_NO_SUBSTR(b, n, "move_to "));
            CHECK(HAS_NO_SUBSTR(b, n, "arc_to "));
            CHECK(HAS_NO_SUBSTR(b, n, "line_to "));
        }
        // arc_to -> records `arc_to`, swallows the line_to/arc its impl issues.
        {
            struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
            CHECK(cv != NULL);
            CHECK(canvas_record_to(cv, sp));
            canvas_begin_path(cv);
            canvas_move_to(cv, 2.0f, 2.0f);   // the only move_to expected
            canvas_arc_to(cv, 2.0f, 2.0f, 8.0f, 2.0f, 3.0f);
            canvas_free(cv);
            char b[1 << 13];
            int const n = slurp(sp, b, (int)sizeof b);
            CHECK(n > 0 && n < (int)sizeof b);
            CHECK(HAS_LINE(b, n, "arc_to 2 2 8 2 3\n"));
            CHECK(HAS_LINE(b, n, "move_to 2 2\n"));         // the caller's, not swallowed
            CHECK(HAS_NO_SUBSTR(b, n, "line_to "));         // arc_to's impl swallowed
            CHECK(HAS_NO_SUBSTR(b, n, "arc "));
        }
        // fill_path -> records `path` block + `fill_path`, swallows p2d_replay's
        // path methods AND the nested current-path build.
        {
            struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
            CHECK(cv != NULL);
            CHECK(canvas_record_to(cv, sp));
            struct canvas_path2d *__single p = canvas_path2d();
            CHECK(p != NULL);
            if (p) {
                canvas_path2d_move_to(p, 4.0f, 4.0f);
                canvas_path2d_line_to(p, 20.0f, 4.0f);
                canvas_path2d_line_to(p, 12.0f, 18.0f);
                canvas_path2d_close_path(p);
                canvas_fill_path(cv, p, CANVAS_NONZERO);
                canvas_path2d_free(p);
            }
            canvas_free(cv);
            char b[1 << 13];
            int const n = slurp(sp, b, (int)sizeof b);
            CHECK(n > 0 && n < (int)sizeof b);
            CHECK(HAS_LINE(b, n, "fill_path 0 nonzero\n"));
            // The path's commands live in the `path` block's m/l/z lines (a
            // distinct grammar); the suspend guard means NO current-path op
            // lines (move_to/line_to/begin_path) leaked from p2d_replay.
            CHECK(HAS_NO_SUBSTR(b, n, "move_to "));
            CHECK(HAS_NO_SUBSTR(b, n, "line_to "));
            CHECK(HAS_NO_SUBSTR(b, n, "begin_path"));
        }
        // resize -> records `resize`, swallows the reset it expands to.
        {
            struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
            CHECK(cv != NULL);
            CHECK(canvas_record_to(cv, sp));
            canvas_resize(cv, W, H);
            canvas_free(cv);
            char b[1 << 13];
            int const n = slurp(sp, b, (int)sizeof b);
            CHECK(n > 0 && n < (int)sizeof b);
            CHECK(HAS_LINE(b, n, "resize 48 32\n"));
            CHECK(HAS_NO_SUBSTR(b, n, "reset"));            // the swallowed expansion
        }
    }

    // 6. Strict parsing: an unknown trailing colour-space token on a colour op
    // line is rejected (replay returns false), like every other bad enum token.
    // A well-formed leading subset already applied; the canvas stays valid.
#define REPLAY(cv, s) cnvs_replay_text((cv), (s), sizeof(s) - 1)
    {
        struct canvas *__single cv = canvas(16, 16, CANVAS_CS_SRGB);
        CHECK(cv != NULL);
        // The valid tokens parse.
        CHECK(REPLAY(cv, "set_fill_rgba 0.25 0.5 0.75 1\n"));          // untagged sRGB
        CHECK(REPLAY(cv, "set_fill_rgba 0.25 0.5 0.75 1 linear\n"));   // tagged linear
        CHECK(REPLAY(cv, "set_fill_rgba 0.25 0.5 0.75 1 oklab\n"));    // tagged oklab
        CHECK(REPLAY(cv, "add_stroke_color_stop 1 0 1 0 1 linear\n"));
        CHECK(REPLAY(cv, "add_filter_drop_shadow 2 2 1 0.25 0.5 0.75 0.5 oklab\n"));
        // An unknown trailing token is malformed.
        CHECK(!REPLAY(cv, "set_fill_rgba 0.25 0.5 0.75 1 rec709\n"));  // bad space name
        CHECK(!REPLAY(cv, "set_stroke_rgba 0 0 0 1 sideways\n"));
        CHECK(!REPLAY(cv, "set_shadow_color_rgba 0 0 0 1 LINEAR\n"));  // case-sensitive
        CHECK(!REPLAY(cv, "add_fill_color_stop 0 1 0 0 1 bogus\n"));
        CHECK(!REPLAY(cv, "add_filter_drop_shadow 2 2 1 0.25 0.5 0.75 0.5 xyz\n"));
        // A valid token followed by junk is malformed too.
        CHECK(!REPLAY(cv, "set_fill_rgba 0.25 0.5 0.75 1 linear extra\n"));
        canvas_free(cv);
    }
#undef REPLAY

    return TEST_REPORT();
}
