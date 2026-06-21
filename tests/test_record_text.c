// The serialized half of the text lookup: a recorded program carries inline
// font/glyph/bitmap/shaping blocks (canonical font-unit curves, ink bounds,
// vmetrics, emoji captures, shaped runs), so replay pre-populates the text
// cache and never crosses the Core Text boundary.  Pinned here:
//   - round trip: record a text scene (emoji included), replay it onto a
//     fresh canvas -> byte-identical pixels AND zero shape/glyph boundary
//     misses (the stats surface proves the boundary was never consulted --
//     replayed emoji DRAW, from their serialized captures, fontless);
//   - measureText / measureTextFull after replay match the recording canvas
//     bit for bit (the serialized ink bounds + vmetrics earning their keep);
//   - dedup: a repeated string emits its blocks once per recording, the
//     emoji bitmap block very much included;
//   - size: the bitmap block rides DEFLATED under its base64 (canvas2d_zlib), so
//     a one-emoji program -- ~137 KB when the capture was raw base64 -- now
//     records at roughly half that;
//   - strict parsing: truncated/malformed blocks (bitmap chunk miscounts,
//     bad/mis-padded base64, deflated-length lies, streams that are not zlib
//     or inflate to the wrong size among them) stop replay false and leave
//     the canvas drawable;
//   - the float round-trip property: every %.9g-printed float32 (denormals,
//     -0, extremes included) reparses to the identical float32.
// See docs/text-boundary.md and canvas.h's record_to/replay_from contracts.

#include "test_util.h"

#include "canvas2d.h"
#include "canvas2d_replay.h"
#include "canvas2d_text.h"
#include "canvas2d_zlib.h"

#include <math.h>
#include <ptrcheck.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 128
#define H 96
#define NPX (W * H * 4)

// sizeof-1 keeps the literal an array (bounds known), avoiding the
// __null_terminated->__counted_by seam at the call.
#define REPLAY(cv, s) canvas2d_replay_text((cv), (s), sizeof(s) - 1)

// A text scene: Latin + CJK + emoji, two sizes, align/baseline variations,
// fill and stroke.  Coordinates are simple decimals; the text round-trips by
// design.  The emoji line proves the bitmap blocks earn their keep: replay
// draws it from the serialized capture, fontless and boundary-free.
static void draw_text_scene(struct canvas2d_context *__single cv) {
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.95f, 0.95f, 0.9f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);

    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.1f, 0.1f, 0.4f, 1.0f);
    canvas2d_set_font_size(cv, 17.5f);
    canvas2d_fill_text(cv, "Waffle 隸書", 4.0f, 24.0f);

    canvas2d_set_font_size(cv, 23.0f);
    canvas2d_set_text_align(cv, CANVAS2D_ALIGN_CENTER);
    canvas2d_set_text_baseline(cv, CANVAS2D_BASELINE_MIDDLE);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.5f, 0.15f, 0.1f, 1.0f);
    canvas2d_fill_text(cv, "Waffle 隸書", 64.0f, 48.0f);  // same bytes, other size

    canvas2d_set_text_align(cv, CANVAS2D_ALIGN_RIGHT);
    canvas2d_set_text_baseline(cv, CANVAS2D_BASELINE_TOP);
    canvas2d_set_stroke_rgba(cv, CANVAS2D_CS_SRGB, 0.1f, 0.3f, 0.1f, 1.0f);
    canvas2d_set_line_width(cv, 1.0f);
    canvas2d_stroke_text(cv, "kerning", 120.0f, 64.0f);

    canvas2d_set_text_align(cv, CANVAS2D_ALIGN_LEFT);
    canvas2d_set_text_baseline(cv, CANVAS2D_BASELINE_ALPHABETIC);
    canvas2d_set_font_size(cv, 21.0f);
    canvas2d_fill_text(cv, "a\xF0\x9F\x8C\x88z", 4.0f, 88.0f);  // a 🌈 z
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

// Count the lines of `buf` starting with `prefix` (which includes its trailing
// space, so "shaping " doesn't match a hypothetical "shapingx").
static int count_lines(char const *__counted_by(len) buf, int len,
                       char const *__null_terminated prefix) {
    int n = 0;
    int at_start = 1;
    for (int i = 0; i < len; i++) {
        if (at_start) {
            int k = 0;
            char const *__null_terminated p = prefix;
            bool match = true;
            while (*p != '\0') {
                if (i + k >= len || buf[i + k] != *p) {
                    match = false;
                    break;
                }
                p++;
                k++;
            }
            if (match) {
                n++;
            }
        }
        at_start = buf[i] == '\n';
    }
    return n;
}

// Bitwise float equality (NaN-free domain): the round-trip contract is the
// identical float32, not an epsilon.
static bool feq_bits(float a, float b) {
    uint32_t ua, ub;
    memcpy(&ua, &a, sizeof ua);
    memcpy(&ub, &b, sizeof ub);
    return ua == ub;
}

static bool metrics_eq(canvas2d_text_metrics a, canvas2d_text_metrics b) {
    return feq_bits(a.width, b.width) &&
           feq_bits(a.actual_bounding_box_left, b.actual_bounding_box_left) &&
           feq_bits(a.actual_bounding_box_right, b.actual_bounding_box_right) &&
           feq_bits(a.actual_bounding_box_ascent, b.actual_bounding_box_ascent) &&
           feq_bits(a.actual_bounding_box_descent, b.actual_bounding_box_descent) &&
           feq_bits(a.font_bounding_box_ascent, b.font_bounding_box_ascent) &&
           feq_bits(a.font_bounding_box_descent, b.font_bounding_box_descent) &&
           feq_bits(a.em_height_ascent, b.em_height_ascent) &&
           feq_bits(a.em_height_descent, b.em_height_descent) &&
           feq_bits(a.alphabetic_baseline, b.alphabetic_baseline) &&
           feq_bits(a.hanging_baseline, b.hanging_baseline) &&
           feq_bits(a.ideographic_baseline, b.ideographic_baseline);
}

// Round trip: record the scene, replay onto a fresh canvas.  Pixels match byte
// for byte; the replay performed ZERO shape/glyph boundary calls (every lookup
// hit the block-built cache); and both measure paths agree bit for bit.
static void check_round_trip(void) {
    char const *__null_terminated path = "build/test_record_text_a.canvas";

    uint8_t recorded_px[NPX];
    float w17 = 0.0f, w23 = 0.0f, we = 0.0f;
    canvas2d_text_metrics m17, m23, me;
    memset(&m17, 0, sizeof m17);
    memset(&m23, 0, sizeof m23);
    memset(&me, 0, sizeof me);
    {
        struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas2d_record_to(cv, path));
        draw_text_scene(cv);
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, recorded_px, (int)sizeof recorded_px);
        canvas2d_set_font_size(cv, 17.5f);
        w17 = canvas2d_measure_text(cv, "Waffle 隸書");
        m17 = canvas2d_measure_text_full(cv, "Waffle 隸書");
        canvas2d_set_font_size(cv, 23.0f);
        w23 = canvas2d_measure_text(cv, "Waffle 隸書");
        m23 = canvas2d_measure_text_full(cv, "Waffle 隸書");
        canvas2d_set_font_size(cv, 21.0f);
        we = canvas2d_measure_text(cv, "a\xF0\x9F\x8C\x88z");
        me = canvas2d_measure_text_full(cv, "a\xF0\x9F\x8C\x88z");
        canvas2d_free(cv);  // flush + close the file
    }
    CHECK(w17 > 0.0f && w23 > w17 && we > 0.0f);
    CHECK(me.actual_bounding_box_ascent > 0.0f);  // the emoji ink measured

    {
        struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas2d_replay_from(cv, path));

        // The proof of self-containment: every text lookup during replay hit
        // the cache the blocks pre-populated -- the boundary was never asked
        // to shape a line or fetch a glyph.
        struct canvas2d_text_cache *__single c = canvas2d_canvas_text_cache(cv);
        CHECK(c->shaping_misses == 0);
        CHECK(c->glyph_misses == 0);
        CHECK(c->shaping_hits > 0);
        CHECK(c->glyph_hits > 0);

        uint8_t replayed_px[NPX];
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, replayed_px, (int)sizeof replayed_px);
        CHECK(memcmp(recorded_px, replayed_px, sizeof recorded_px) == 0);

        // Measurement replays from the serialized ink bounds + vmetrics: the
        // values match the recording canvas's bit for bit, still boundary-free
        // -- the emoji line included (its ink box rides the bitmap block, and
        // both canvases scale it through the same capture-px math).
        canvas2d_set_font_size(cv, 17.5f);
        CHECK(feq_bits(canvas2d_measure_text(cv, "Waffle 隸書"), w17));
        CHECK(metrics_eq(canvas2d_measure_text_full(cv, "Waffle 隸書"), m17));
        canvas2d_set_font_size(cv, 23.0f);
        CHECK(feq_bits(canvas2d_measure_text(cv, "Waffle 隸書"), w23));
        CHECK(metrics_eq(canvas2d_measure_text_full(cv, "Waffle 隸書"), m23));
        canvas2d_set_font_size(cv, 21.0f);
        CHECK(feq_bits(canvas2d_measure_text(cv, "a\xF0\x9F\x8C\x88z"), we));
        CHECK(metrics_eq(canvas2d_measure_text_full(cv, "a\xF0\x9F\x8C\x88z"), me));
        CHECK(c->shaping_misses == 0);
        CHECK(c->glyph_misses == 0);

        canvas2d_free(cv);
    }
}

// Dedup: drawing the same string twice (and measuring it) emits its font,
// glyph, and bitmap blocks exactly once; the second op is just its op line.
static void check_dedup(void) {
    char const *__null_terminated once = "build/test_record_text_d1.canvas";
    char const *__null_terminated twice = "build/test_record_text_d2.canvas";

    {
        struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas2d_record_to(cv, once));
        canvas2d_set_font_size(cv, 16.0f);
        canvas2d_fill_text(cv, "echo", 4.0f, 24.0f);
        canvas2d_fill_text(cv, "\xF0\x9F\x8D\x95", 4.0f, 48.0f);  // 🍕
        canvas2d_free(cv);
    }
    {
        struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas2d_record_to(cv, twice));
        canvas2d_set_font_size(cv, 16.0f);
        canvas2d_fill_text(cv, "echo", 4.0f, 24.0f);
        canvas2d_fill_text(cv, "echo", 4.0f, 48.0f);
        canvas2d_fill_text(cv, "\xF0\x9F\x8D\x95", 4.0f, 48.0f);
        canvas2d_fill_text(cv, "\xF0\x9F\x8D\x95", 40.0f, 48.0f);
        canvas2d_free(cv);
    }

    // Static buffers: roomy enough even if a capture ever compressed badly.
    static char a[1 << 19];
    static char b[1 << 19];
    int const na = slurp(once, a, (int)sizeof a);
    int const nb = slurp(twice, b, (int)sizeof b);
    CHECK(na > 0 && na < (int)sizeof a);
    CHECK(nb > 0 && nb < (int)sizeof b);
    if (na <= 0 || nb <= 0) {
        return;
    }
    // Same block lines whether each string draws once or twice...
    CHECK(count_lines(a, na, "font ") == count_lines(b, nb, "font "));
    CHECK(count_lines(a, na, "glyph ") == count_lines(b, nb, "glyph "));
    CHECK(count_lines(a, na, "bitmap ") == count_lines(b, nb, "bitmap "));
    CHECK(count_lines(a, na, "bits ") == count_lines(b, nb, "bits "));
    CHECK(count_lines(a, na, "shaping ") == 2);
    CHECK(count_lines(b, nb, "shaping ") == 2);
    // ...and the blocks really exist (three distinct outline glyphs, one
    // capture whose deflated stream takes at least one bits line -- the exact
    // count is the compressor's business, not the format's).
    CHECK(count_lines(b, nb, "glyph ") >= 3);  // e, c, h, o -> >= 3 distinct
    CHECK(count_lines(b, nb, "bitmap ") == 1);
    CHECK(count_lines(b, nb, "bits ") >= 1);
    CHECK(count_lines(b, nb, "run ") == 2);
    CHECK(count_lines(b, nb, "fill_text ") == 4);
}

// Size: the capture rides deflated under the base64, so a one-emoji program
// -- ~137 KB when the 102400 capture bytes were raw base64 (nine 16 KiB
// lines) -- now records at roughly half that (the 🍕 measures ~57 KB; emoji
// across the art spectrum land 40-70 KB, anti-aliased gradients being noisy
// input for the greedy fixed-Huffman deflate).  The ceiling is deliberately
// loose -- emoji art and the compressor may both drift -- the point pinned is
// that the compression is real and the whole program stays well under the
// raw capture alone (102400), let alone its base64.
static void check_size(void) {
    char const *__null_terminated path = "build/test_record_text_z.canvas";
    {
        struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas2d_record_to(cv, path));
        canvas2d_set_font_size(cv, 32.0f);
        canvas2d_fill_text(cv, "\xF0\x9F\x8D\x95", 4.0f, 48.0f);  // 🍕
        canvas2d_free(cv);
    }
    static char buf[1 << 19];
    int const n = slurp(path, buf, (int)sizeof buf);
    CHECK(n > 0);
    CHECK(n < 80 * 1024);
    CHECK(count_lines(buf, n > 0 ? n : 0, "bitmap ") == 1);  // the block IS there
}

// Base64-encode `n` bytes into out (standard alphabet, '=' padding only in
// the final group), NUL-terminated -- mirrors the recorder's emission so the
// strict tests can wrap real deflate streams.  Returns the encoded length, or
// -1 if cap is too small.
static int b64enc(char *__counted_by(cap) out, int cap,
                  uint8_t const *__counted_by(n) src, int n) {
    static char const k[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                              "abcdefghijklmnopqrstuvwxyz0123456789+/";
    int at = 0;
    for (int i = 0; i < n; i += 3) {
        if (at + 5 > cap) {
            return -1;
        }
        int const m = n - i;
        uint32_t v = (uint32_t)src[i] << 16;
        if (m > 1) { v |= (uint32_t)src[i + 1] << 8; }
        if (m > 2) { v |= (uint32_t)src[i + 2]; }
        out[at++] = k[(v >> 18) & 63u];
        out[at++] = k[(v >> 12) & 63u];
        out[at++] = m > 1 ? k[(v >> 6) & 63u] : '=';
        out[at++] = m > 2 ? k[v & 63u] : '=';
    }
    if (at >= cap) {
        return -1;
    }
    out[at] = '\0';
    return at;
}

// Format one program into an in-memory FILE (fprintf, the same libc formatter
// the recorder writes with; snprintf's -fbounds-safety macro form trips
// -Wgnu-statement-expression) and replay it, returning the parse verdict.
__attribute__((format(printf, 2, 3)))
static bool replay_fmt(struct canvas2d_context *__single cv, char const *__null_terminated fmt,
                       ...) {
    static char prog[1024];
    FILE *__single f = fmemopen(prog, sizeof prog, "w");
    if (!f) {
        return false;
    }
    va_list ap;
    va_start(ap, fmt);
    int const r = vfprintf(f, fmt, ap);
    va_end(ap);
    bool const ok = r > 0 && fflush(f) == 0;
    long const n = ftell(f);
    (void)fclose(f);
    if (!ok || n <= 0 || (size_t)n >= sizeof prog) {
        return false;
    }
    return canvas2d_replay_text(cv, prog, (size_t)n);
}

// Strict parsing: malformed blocks stop replay (false) without corrupting the
// canvas -- it draws normally afterwards.
static void check_strict(void) {
    struct canvas2d_context *__single cv = canvas2d(64, 48, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }

    // Well-formed blocks parse (a tiny square glyph, a blank glyph, a shape).
    // The font block carries weight/style (400 0 here); the shaping block too.
    CHECK(REPLAY(cv,
        "font 0 1.05 0.33 400 0 Libian TC\n"
        "font 1 1.05 0.33 400 0 STLibianTC-Regular\n"
        "glyph 1 43 1000 10 11 592 601 m 10 11 l 592 11 l 592 601 l 10 601 z\n"
        "glyph 1 3 0 0 0 0 0\n"
        "shaping 12 0 0 0 400 0 0 0 0 4 0  9 Libian TC 2 1 2 H \n"
        "run 1 0 0 2 43 7.224 0 3 2.8 1\n"
        "fill_text 4 20 H \n"));

    // An rtl-shaped block parses too, and its op draws under the direction the
    // line was shaped at.
    CHECK(REPLAY(cv,
        "shaping 12 1 0 0 400 0 0 0 0 4 0  9 Libian TC 1 1 1 W\n"
        "run -1 1 0 1 7 5.5 0\n"
        "set_direction rtl\n"
        "fill_text 4 40 W\n"));

    // The shaping toggles ride the block: a non-default kerning (2 = none),
    // rendering (1 = optimizeSpeed), variant-caps (2 = all-small-caps), stretch
    // (2 = condensed), and a lang tag ("zh-Hant", 7 bytes) all
    // parse, length-prefixed lang like the family.
    CHECK(REPLAY(cv,
        "shaping 12 0 0 0 400 0 2 1 2 2 7 zh-Hant 9 Libian TC 1 1 1 A\n"
        "run -1 0 0 1 7 5.5 0\n"
        "fill_text 4 60 A\n"));

    // Malformed blocks are rejected.
    CHECK(!REPLAY(cv, "font 16 0.5 0.2 400 0 Over\n"));            // id out of range
    CHECK(!REPLAY(cv, "font 0 0.5 0.2 400 0 A\nfont 0 0.5 0.2 400 0 B\n"));  // redeclared id
    CHECK(!REPLAY(cv, "font 0 0.5 0.2 400 0\n"));            // empty name
    CHECK(!REPLAY(cv, "font 0 1e999 0.2 400 0 A\n"));        // overflowed float
    CHECK(!REPLAY(cv, "font 0 1 0.2 50 0 A\n"));             // weight below 100
    CHECK(!REPLAY(cv, "font 0 1 0.2 400 2 A\n"));            // "2" is no style bit
    CHECK(!REPLAY(cv, "glyph 0 5 1000 0 0 1 1 m 1 2\n"));    // undeclared font
    CHECK(!REPLAY(cv, "font 0 1 0.2 400 0 A\nglyph 0 5 1000 0 0 1 1 m 1\n"));   // short point
    CHECK(!REPLAY(cv, "font 0 1 0.2 400 0 A\nglyph 0 5 1000 0 0 1 1 x 1 2\n")); // bad verb
    CHECK(!REPLAY(cv, "font 0 1 0.2 400 0 A\nglyph 0 70000 1000 0 0 1 1 z\n")); // gid > 0xFFFF
    CHECK(!REPLAY(cv, "font 0 1 0.2 400 0 A\nglyph 0 5 1e999 0 0 1 1 z\n"));    // inf upem
    CHECK(!REPLAY(cv, "font 0 1 0.2 400 0 A\nglyph 0 5 0 0 0 0 0 m 1 2\n"));    // blank w/ curves
    CHECK(!REPLAY(cv, "font 0 1 0.2 400 0 A\nglyph 0 5 -1 0 0 1 1 z\n"));       // negative upem
    CHECK(!REPLAY(cv, "shaping 12 2 0 0 400 0 1 1 2 H \n"));     // "2" is no
                                                               // direction bit
    CHECK(!REPLAY(cv, "shaping 12 x 0 0 400 0 1 1 1 A\nrun -1 0 0 0\n"));  // nor is "x"
    CHECK(!REPLAY(cv, "shaping 12 0 x 0 400 0 1 1 1 A\n"));      // non-finite ls
    CHECK(!REPLAY(cv, "shaping 12 0 0 1e999 400 0 1 1 1 A\n"));  // non-finite ws
    CHECK(!REPLAY(cv, "shaping 12 0 0 0 50 0 9 Libian TC 1 1 1 A\n"));  // weight below 100
    CHECK(!REPLAY(cv, "shaping 12 0 0 0 400 2 9 Libian TC 1 1 1 A\n")); // "2" is no style bit
    CHECK(!REPLAY(cv, "shaping 12 0 0 0 400 0 0 0 0 4 0  999 Lib\n"));  // family bytes
                                                               // overrun the line
    CHECK(!REPLAY(cv, "shaping 12 0 0 0 400 0 0 0 0 4 0  9 Libian TC 1 1 3 A\n"));  // byte-len mismatch
    CHECK(!REPLAY(cv, "shaping 12 0 0 0 400 0 0 0 0 4 0  9 Libian TC 4 1 2 Hi\n")); // utf16 len > bytes
    CHECK(!REPLAY(cv, "shaping 12 0 0 0 400 0 0 0 0 4 0  9 Libian TC 1 1 1 A\n"));  // truncated: no run line
    CHECK(!REPLAY(cv, "shaping 12 0 0 0 400 0 0 0 0 4 0  9 Libian TC 1 1 1 A\nfill_rect 0 0 4 4\n"));  // non-run inside
    CHECK(!REPLAY(cv, "shaping 12 0 0 0 400 0 0 0 0 4 0  9 Libian TC 1 1 1 A\n# comment\nrun 0 0 0 0\n"));  // ditto
    // The shaping toggles parse strictly: kerning in [0, 2], rendering in [0, 3],
    // variant-caps in [0, 2], stretch in [0, 8].
    CHECK(!REPLAY(cv, "shaping 12 0 0 0 400 0 3 0 0 4 0  9 Libian TC 1 1 1 A\n"));  // kerning out of range
    CHECK(!REPLAY(cv, "shaping 12 0 0 0 400 0 0 4 0 4 0  9 Libian TC 1 1 1 A\n"));  // rendering out of range
    CHECK(!REPLAY(cv, "shaping 12 0 0 0 400 0 0 0 3 4 0  9 Libian TC 1 1 1 A\n"));  // variant-caps out of range
    CHECK(!REPLAY(cv, "shaping 12 0 0 0 400 0 0 0 0 9 0  9 Libian TC 1 1 1 A\n"));  // stretch out of range
    CHECK(!REPLAY(cv, "shaping 12 0 0 0 400 0 0 0 0 4 99 ab\n"));  // lang bytes overrun the line
    CHECK(!REPLAY(cv, "run 0 0 0 0\n"));                     // run with no shape
    CHECK(!REPLAY(cv,
        "font 0 1 0.2 400 0 A\n"
        "shaping 12 0 0 0 400 0 0 0 0 4 0  9 Libian TC 1 1 1 A\n"
        "run 0 0 0 1 10 5 1\n"));                            // cluster >= utf16 len
    CHECK(!REPLAY(cv,
        "shaping 12 0 0 0 400 0 0 0 0 4 0  9 Libian TC 1 1 1 A\n"
        "run 3 0 0 1 10 5 0\n"));                            // undeclared run font
    CHECK(!REPLAY(cv,
        "shaping 12 0 0 0 400 0 0 0 0 4 0  9 Libian TC 1 1 1 A\n"
        "run -1 0 0 1 10 1e999 0\n"));                       // overflowed advance

    // Bitmap blocks carry the capture DEFLATED under the base64, so the
    // well-formed cases wrap real canvas2d_zlib_deflate streams.  A 2x2 premul
    // RGBA capture (16 bytes: three transparent pixels, one opaque white)...
    uint8_t raw16[16];
    for (int i = 0; i < 16; i++) {
        raw16[i] = i < 12 ? 0 : 0xFF;
    }
    uint8_t z16[64];
    int const zn16 = canvas2d_zlib_deflate(z16, (int)sizeof z16, raw16, 16);
    CHECK(zn16 > 3);  // header + adler alone are 6 bytes; > 3 lets us chunk
    char zb16[128] = { 0 };
    CHECK(b64enc(zb16, (int)sizeof zb16, z16, zn16 > 0 ? zn16 : 0) > 0);
    #define BM_FONT "font 0 1 0.25 400 0 AppleColorEmoji\n"

    // ...parses as one bits line...
    CHECK(replay_fmt(cv, BM_FONT "bitmap 0 64 2 2 0 -0.5 2 1.5 %d 1\nbits %s\n",
                     zn16, zb16));

    // ...and chunked across two (the first carrying three decoded bytes).
    CHECK(replay_fmt(cv,
                     BM_FONT "bitmap 0 64 2 2 0 -0.5 2 1.5 %d 2\n"
                             "bits %.4s\nbits %s\n",
                     zn16, zb16, zb16 + 4));

    // Lies about the deflated stream are rejected: each case below keeps the
    // base64 structurally valid so the failure isolates the named check.
    {
        // Declared deflated length one longer than what the lines land...
        CHECK(!replay_fmt(cv,
                          BM_FONT "bitmap 0 64 2 2 0 -0.5 2 1.5 %d 1\nbits %s\n",
                          zn16 + 1, zb16));
        // ...and one shorter: the line decodes past the declaration.
        CHECK(!replay_fmt(cv,
                          BM_FONT "bitmap 0 64 2 2 0 -0.5 2 1.5 %d 1\nbits %s\n",
                          zn16 - 1, zb16));
        // Truncated stream, length re-declared to match: base64 and count
        // agree, but the zlib stream lost its tail -- inflate rejects.
        char ztrunc[128] = { 0 };
        CHECK(b64enc(ztrunc, (int)sizeof ztrunc, z16, zn16 - 3) > 0);
        CHECK(!replay_fmt(cv,
                          BM_FONT "bitmap 0 64 2 2 0 -0.5 2 1.5 %d 1\nbits %s\n",
                          zn16 - 3, ztrunc));
        // A perfectly valid zlib stream of the WRONG decoded size, both ways:
        // 12 bytes against the header's 2x2 (16)...
        uint8_t zwrong[64];
        char zwb[128] = { 0 };
        int wn = canvas2d_zlib_deflate(zwrong, (int)sizeof zwrong, raw16, 12);
        CHECK(wn > 0);
        CHECK(b64enc(zwb, (int)sizeof zwb, zwrong, wn > 0 ? wn : 0) > 0);
        CHECK(!replay_fmt(cv,
                          BM_FONT "bitmap 0 64 2 2 0 -0.5 2 1.5 %d 1\nbits %s\n",
                          wn, zwb));
        // ...and 20 bytes against it (inflate overflows the w*h*4 buffer).
        uint8_t raw20[20];
        for (int i = 0; i < 20; i++) {
            raw20[i] = (uint8_t)(i * 7);
        }
        wn = canvas2d_zlib_deflate(zwrong, (int)sizeof zwrong, raw20, 20);
        CHECK(wn > 0);
        CHECK(b64enc(zwb, (int)sizeof zwb, zwrong, wn > 0 ? wn : 0) > 0);
        CHECK(!replay_fmt(cv,
                          BM_FONT "bitmap 0 64 2 2 0 -0.5 2 1.5 %d 1\nbits %s\n",
                          wn, zwb));
    }

    // Bytes that are not a zlib stream at all (four zero bytes: bad header).
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 4 1\nbits AAAAAA==\n"));

    // Header and base64 structure stay as strict as ever (zlen is 4 -- the
    // structural failure fires before any inflate could).
    CHECK(!REPLAY(cv, "bitmap 0 64 1 1 0 0 1 1 4 1\nbits AAAAAA==\n"));  // undeclared font
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 70000 1 1 0 0 1 1 4 1\nbits AAAAAA==\n"));  // gid > 0xFFFF
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 0 1 0 0 1 1 4 1\nbits AAAAAA==\n"));     // zero width
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 513 1 0 0 1 1 4 1\nbits AAAAAA==\n"));   // width > cap
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 1e999 0 1 1 4 1\nbits AAAAAA==\n")); // inf ink
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 1 0 1 1 4 1\nbits AAAAAA==\n"));     // empty ink (x1 <= x0)
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 0 1\nbits AAAAAA==\n"));     // zero deflated length
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 21 1\nbits AAAAAA==\n"));    // zlen > zlib_bound(4) = 20
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 4 0\nbits AAAAAA==\n"));     // zero lines
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 4 3\n"));                    // nlines > ceil(zlen/3)
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 4 1 junk\nbits AAAAAA==\n"));// trailing junk
    CHECK(!REPLAY(cv, "bits AAAAAA==\n"));                                          // bits with no bitmap
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 4 1\nbits AA!AAA==\n"));     // bad base64 char
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 4 1\nbits AAAAA\n"));        // length % 4 != 0
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 4 1\nbits =AAAAAAA\n"));     // '=' up front
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 4 1\nbits AA==AAAA\n"));     // padding mid-line
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 2 2 0 0 1 1 6 2\nbits AA==\nbits AAAAAAAA\n"));  // padding before the final line
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 4 1\nbits AAAA\n"));         // short: 3 of 4 bytes
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 4 1\nbits AAAAAAAA\n"));     // long: 6 of 4 bytes
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 4 1\n"));                    // truncated: no bits line
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 4 1\nfill_rect 0 0 4 4\n")); // non-bits inside
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 4 1\n# comment\nbits AAAAAA==\n"));  // ditto
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 4 1\nshape 12 0 1 0 1 A\n"));  // shape inside
    #undef BM_FONT

    // Not corrupted: the canvas still draws.
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, 64.0f, 48.0f);
    uint8_t px[64 * 48 * 4];
    canvas2d_get_image_data(cv, CANVAS2D_CS_SRGB, 0, 0, 64, 48, px, (int)sizeof px);
    CHECK(px[0] == 255);

    canvas2d_free(cv);
}

// xorshift32, run in 64-bit and masked back: the wrap is by design, so keep
// it out of -fsanitize=integer's unsigned-shift check (the mix32 pattern in
// canvas2d_text.c).
static uint32_t xorshift32(uint32_t *__single s) {
    uint64_t x = *s;
    x = (x ^ (x << 13)) & 0xFFFFFFFFu;
    x = x ^ (x >> 17);
    x = (x ^ (x << 5)) & 0xFFFFFFFFu;
    *s = (uint32_t)x;
    return (uint32_t)x;
}

// One float through the recorder's emission (fprintf %.9g -- the identical
// libc formatter canvas2d_record.c writes with, via an in-memory FILE) and the
// replay parser's number reader (a set_transform line, read back via
// get_transform): the reparsed float32 must be bit-identical.  set_transform
// now carries the full 3x3 (the affine subset's trailing 0 0 1).
static bool roundtrips(struct canvas2d_context *__single cv, FILE *__single f,
                       char const *__counted_by(cap) line, int cap, float v) {
    rewind(f);
    fprintf(f, "set_transform %.9g 0 0 1 0 0 0 0 1\n", (double)v);
    if (fflush(f) != 0) {
        return false;
    }
    long const n = ftell(f);
    if (n <= 0 || n > cap) {
        return false;
    }
    if (!canvas2d_replay_text(cv, line, (size_t)n)) {
        return false;
    }
    return feq_bits(canvas2d_get_transform(cv).a, v);
}

// The float round-trip property: emit/parse identity over denormals, negative
// zero, extremes, powers of two, and a large random sweep of bit patterns.
static void check_float_round_trip(void) {
    struct canvas2d_context *__single cv = canvas2d(8, 8, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    char line[64];
    FILE *__single f = fmemopen(line, sizeof line, "w");
    CHECK(f != NULL);
    if (!f) {
        canvas2d_free(cv);
        return;
    }

    uint32_t const fixed[] = {
        0x00000000u,  // +0
        0x80000000u,  // -0
        0x00000001u,  // min subnormal
        0x80000001u,  // -min subnormal
        0x007FFFFFu,  // max subnormal
        0x00800000u,  // FLT_MIN
        0x7F7FFFFFu,  // FLT_MAX (huge advance)
        0xFF7FFFFFu,  // -FLT_MAX
        0x3F800000u,  // 1
        0x3F800001u,  // nextafter(1)
        0x4B7FFFFFu,  // 16777215 (last odd exact int)
        0x4B800000u,  // 16777216
        0x33D6BF95u,  // 1e-7
        0x501502F9u,  // 1e10
    };
    for (int i = 0; i < (int)(sizeof fixed / sizeof fixed[0]); i++) {
        float v;
        memcpy(&v, &fixed[i], sizeof v);
        CHECK(roundtrips(cv, f, line, (int)sizeof line, v));
    }

    // Every exponent with a few mantissa patterns (covers the whole denormal
    // band and both sides of every power of two)...
    int bad = 0;
    for (uint32_t exp = 0; exp <= 0xFE; exp++) {
        uint32_t const mants[] = { 0u, 1u, 0x400000u, 0x7FFFFFu, 0x357319u };
        for (int mi = 0; mi < (int)(sizeof mants / sizeof mants[0]); mi++) {
            for (uint32_t sign = 0; sign <= 1; sign++) {
                uint32_t bits = (sign << 31) | (exp << 23) | mants[mi];
                float v;
                memcpy(&v, &bits, sizeof v);
                if (!roundtrips(cv, f, line, (int)sizeof line, v)) {
                    bad++;
                }
            }
        }
    }
    CHECK(bad == 0);

    // ...plus random finite bit patterns.
    uint32_t seed = 0xC0FFEEu;
    for (int i = 0; i < 20000; i++) {
        uint32_t bits = xorshift32(&seed);
        if (((bits >> 23) & 0xFFu) == 0xFFu) {
            continue;  // inf/NaN: not emittable as a parseable number
        }
        float v;
        memcpy(&v, &bits, sizeof v);
        if (!roundtrips(cv, f, line, (int)sizeof line, v)) {
            bad++;
        }
    }
    CHECK(bad == 0);

    (void)fclose(f);
    canvas2d_free(cv);
}

// stroke_rect and fill_text_max -- the last two ops the gallery text scenes
// need -- round-trip byte for byte: recording a scene that uses both and
// replaying it onto a fresh canvas reproduces identical pixels, with no text
// boundary call (the fill_text_max line carries the same font/glyph/shape
// blocks fill_text would).  stroke_rect strokes its own rect through the CTM;
// fill_text_max condenses the phrase about its left anchor to fit max_width.
static void check_new_ops(void) {
    char const *__null_terminated path = "build/test_record_text_n.canvas";

    uint8_t recorded_px[NPX];
    {
        struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas2d_record_to(cv, path));
        canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.1f, 0.1f, 0.12f, 1.0f);
        canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);

        // stroke_rect with a thick join, and a rotated-CTM quad (corners go
        // through the transform), and the degenerate hairline.
        canvas2d_set_stroke_rgba(cv, CANVAS2D_CS_SRGB, 0.9f, 0.5f, 0.3f, 1.0f);
        canvas2d_set_line_width(cv, 6.0f);
        canvas2d_set_line_join(cv, CANVAS2D_JOIN_ROUND);
        canvas2d_stroke_rect(cv, 8.0f, 8.0f, 40.0f, 30.0f);
        canvas2d_save(cv);
        canvas2d_translate(cv, 90.0f, 30.0f);
        canvas2d_rotate(cv, 0.3f);
        canvas2d_stroke_rect(cv, -20.0f, -14.0f, 40.0f, 28.0f);
        canvas2d_restore(cv);
        canvas2d_set_line_cap(cv, CANVAS2D_CAP_ROUND);
        canvas2d_stroke_rect(cv, 8.0f, 60.0f, 40.0f, 0.0f);  // degenerate hairline

        // fill_text_max: an overflowing phrase condensed to a finite width, and
        // an unconstrained one (max_width <= 0 imposes no limit).
        canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.9f, 0.92f, 0.95f, 1.0f);
        canvas2d_set_font_size(cv, 22.0f);
        canvas2d_fill_text_max(cv, "Condense me to fit", 4.0f, 88.0f, 60.0f);
        canvas2d_fill_text_max(cv, "free", 70.0f, 88.0f, -1.0f);
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, recorded_px, (int)sizeof recorded_px);
        canvas2d_free(cv);  // flush + close
    }

    {
        struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas2d_replay_from(cv, path));
        struct canvas2d_text_cache *__single c = canvas2d_canvas_text_cache(cv);
        CHECK(c->shaping_misses == 0);  // fill_text_max replayed boundary-free
        CHECK(c->glyph_misses == 0);
        CHECK(c->shaping_hits > 0);
        CHECK(c->glyph_hits > 0);

        uint8_t replayed_px[NPX];
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, replayed_px, (int)sizeof replayed_px);
        CHECK(memcmp(recorded_px, replayed_px, sizeof recorded_px) == 0);
        canvas2d_free(cv);
    }

    // Strict parsing of the new op lines (the canvas still draws after each).
    struct canvas2d_context *__single cv = canvas2d(32, 32, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    CHECK(REPLAY(cv, "stroke_rect 1 2 10 10\n"));      // well-formed
    CHECK(!REPLAY(cv, "stroke_rect 1 2 10\n"));         // too few floats
    CHECK(!REPLAY(cv, "stroke_rect 1 2 10 10 x\n"));    // trailing junk
    CHECK(!REPLAY(cv, "stroke_rect 1 2 1.5.2 10\n"));   // malformed float token
    // fill_text_max needs three floats before the text; the text is the tail.
    CHECK(REPLAY(cv,
        "font 0 1.05 0.33 400 0 Libian TC\n"
        "glyph 0 43 1000 10 11 592 601 m 10 11 l 592 11 l 592 601 l 10 601 z\n"
        "shaping 12 0 0 0 400 0 0 0 0 4 0  9 Libian TC 1 1 1 H\n"
        "run 0 0 0 1 43 7.224 0\n"
        "fill_text_max 4 20 50 H\n"));
    CHECK(!REPLAY(cv, "fill_text_max 4 20 H\n"));       // missing max_width
    canvas2d_free(cv);
}

// The shadow setters round-trip: a drop-shadowed fill_text (the gallery's emoji
// scene uses exactly this on its first emoji) reproduces byte for byte and
// boundary-free.  Without recording the shadow state, the replay would draw the
// glyph with no shadow and diverge -- so this is the proof the four setters
// (color_rgba, blur, offset_x, offset_y) are captured.
static void check_shadow_ops(void) {
    char const *__null_terminated path = "build/test_record_text_s.canvas";

    uint8_t recorded_px[NPX];
    {
        struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas2d_record_to(cv, path));
        canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.95f, 0.95f, 0.95f, 1.0f);
        canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
        canvas2d_set_shadow_color_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 0.55f);
        canvas2d_set_shadow_blur(cv, 6.0f);
        canvas2d_set_shadow_offset_x(cv, 3.0f);
        canvas2d_set_shadow_offset_y(cv, 4.0f);
        canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.1f, 0.2f, 0.6f, 1.0f);
        canvas2d_set_font_size(cv, 28.0f);
        canvas2d_fill_text(cv, "shadow", 6.0f, 50.0f);
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, recorded_px, (int)sizeof recorded_px);
        canvas2d_free(cv);
    }
    {
        struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas2d_replay_from(cv, path));
        struct canvas2d_text_cache *__single c = canvas2d_canvas_text_cache(cv);
        CHECK(c->shaping_misses == 0);
        CHECK(c->glyph_misses == 0);
        uint8_t replayed_px[NPX];
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, replayed_px, (int)sizeof replayed_px);
        CHECK(memcmp(recorded_px, replayed_px, sizeof recorded_px) == 0);
        canvas2d_free(cv);
    }

    // Strict parse of the shadow op lines.
    struct canvas2d_context *__single cv = canvas2d(32, 32, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    CHECK(REPLAY(cv, "set_shadow_color_rgba 0 0 0 0.5 srgb\n"));
    CHECK(REPLAY(cv, "set_shadow_blur 4\n"));
    CHECK(REPLAY(cv, "set_shadow_offset_x 3\nset_shadow_offset_y -2\n"));
    CHECK(!REPLAY(cv, "set_shadow_color_rgba 0 0 0 srgb\n"));  // too few floats
    CHECK(!REPLAY(cv, "set_shadow_blur\n"));                // missing float
    CHECK(!REPLAY(cv, "set_shadow_offset_x 3 4\n"));        // trailing junk
    canvas2d_free(cv);
}

// Direction rides the shape block: the same bytes drawn under ltr and then rtl
// are two distinct shaped lines (the bidi reordering differs), so the recording
// carries TWO shape blocks -- distinguished by the direction bit -- and replay
// rebuilds each under its own key.  Were direction missing from the key or the
// block, the second draw would alias the first line and replay would draw both
// rows in one order; the byte compare (plus the zero-miss assertion) proves the
// aliasing cannot happen.
static void check_direction_blocks(void) {
    char const *__null_terminated path = "build/test_record_text_r.canvas";
    char const *__null_terminated mixed = "\xD7\x90\xD7\x91 ab!";  // "אב ab!"

    uint8_t recorded_px[NPX];
    float w_ltr = 0.0f, w_rtl = 0.0f;
    {
        struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas2d_record_to(cv, path));
        canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.95f, 0.95f, 0.9f, 1.0f);
        canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
        canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.1f, 0.1f, 0.4f, 1.0f);
        canvas2d_set_font_size(cv, 20.0f);
        canvas2d_fill_text(cv, mixed, 4.0f, 32.0f);
        w_ltr = canvas2d_measure_text(cv, mixed);
        canvas2d_set_direction(cv, CANVAS2D_DIRECTION_RTL);
        canvas2d_fill_text(cv, mixed, 4.0f, 70.0f);   // same bytes, other paragraph
        w_rtl = canvas2d_measure_text(cv, mixed);
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, recorded_px, (int)sizeof recorded_px);
        canvas2d_free(cv);
    }
    CHECK(w_ltr > 0.0f && w_rtl > 0.0f);  // draw-measure agreement holds per
                                          // direction (same key, same line)

    // One shape block per direction, the bit telling them apart.
    static char buf[1 << 19];
    int const n = slurp(path, buf, (int)sizeof buf);
    CHECK(n > 0 && n < (int)sizeof buf);
    if (n <= 0) {
        return;
    }
    CHECK(count_lines(buf, n, "shaping 20 0 ") == 1);
    CHECK(count_lines(buf, n, "shaping 20 1 ") == 1);
    CHECK(count_lines(buf, n, "set_direction ") == 1);

    {
        struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas2d_replay_from(cv, path));
        struct canvas2d_text_cache *__single c = canvas2d_canvas_text_cache(cv);
        CHECK(c->shaping_misses == 0);  // both lines came from their blocks
        CHECK(c->glyph_misses == 0);
        CHECK(c->shaping_hits > 0);

        uint8_t replayed_px[NPX];
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, replayed_px, (int)sizeof replayed_px);
        CHECK(memcmp(recorded_px, replayed_px, sizeof recorded_px) == 0);

        // Measurement after replay reads the same per-direction lines.  (The
        // replayed program left the canvas rtl; measure each side explicitly.)
        canvas2d_set_font_size(cv, 20.0f);
        canvas2d_set_direction(cv, CANVAS2D_DIRECTION_LTR);
        CHECK(feq_bits(canvas2d_measure_text(cv, mixed), w_ltr));
        canvas2d_set_direction(cv, CANVAS2D_DIRECTION_RTL);
        CHECK(feq_bits(canvas2d_measure_text(cv, mixed), w_rtl));
        CHECK(c->shaping_misses == 0);
        canvas2d_free(cv);
    }
}

int main(void) {
    check_round_trip();
    check_dedup();
    check_size();
    check_strict();
    check_new_ops();
    check_shadow_ops();
    check_direction_blocks();
    check_float_round_trip();
    return TEST_REPORT();
}
