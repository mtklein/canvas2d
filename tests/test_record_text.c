// The serialized half of the text lookup: a recorded program carries inline
// font/glyph/bitmap/shape blocks (canonical font-unit curves, ink bounds,
// vmetrics, emoji captures, shaped runs), so replay pre-populates the text
// cache and never crosses the Core Text boundary.  Pinned here:
//   - round trip: record a text scene (emoji included), replay it onto a
//     fresh canvas -> byte-identical pixels AND zero shape/glyph boundary
//     misses (the stats surface proves the boundary was never consulted --
//     replayed emoji DRAW, from their serialized captures, fontless);
//   - measureText / measureTextFull after replay match the recording canvas
//     bit for bit (the serialized ink bounds + vmetrics earning their keep);
//   - dedup: a repeated string emits its blocks once per recording, the
//     ~137 KB emoji bitmap block very much included;
//   - strict parsing: truncated/malformed blocks (bitmap chunk miscounts,
//     bad/mis-padded base64, wrong decoded size among them) stop replay false
//     and leave the canvas drawable;
//   - the float round-trip property: every %.9g-printed float32 (denormals,
//     -0, extremes included) reparses to the identical float32.
// See docs/text-boundary.md and canvas.h's record_to/replay_from contracts.

#include "test_util.h"

#include "canvas.h"
#include "cnvs_replay.h"
#include "cnvs_text.h"

#include <math.h>
#include <ptrcheck.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 128
#define H 96
#define NPX (W * H * 4)

// sizeof-1 keeps the literal an array (bounds known), avoiding the
// __null_terminated->__counted_by seam at the call.
#define REPLAY(cv, s) cnvs_replay_text((cv), (s), sizeof(s) - 1)

// A text scene: Latin + CJK + emoji, two sizes, align/baseline variations,
// fill and stroke.  Coordinates are simple decimals; the text round-trips by
// design.  The emoji line proves the bitmap blocks earn their keep: replay
// draws it from the serialized capture, fontless and boundary-free.
static void draw_text_scene(canvas *__single cv) {
    canvas_set_fill_rgba(cv, 0.95f, 0.95f, 0.9f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);

    canvas_set_fill_rgba(cv, 0.1f, 0.1f, 0.4f, 1.0f);
    canvas_set_font_size(cv, 17.5f);
    canvas_fill_text(cv, "Waffle 隸書", 4.0f, 24.0f);

    canvas_set_font_size(cv, 23.0f);
    canvas_set_text_align(cv, CANVAS_ALIGN_CENTER);
    canvas_set_text_baseline(cv, CANVAS_BASELINE_MIDDLE);
    canvas_set_fill_rgba(cv, 0.5f, 0.15f, 0.1f, 1.0f);
    canvas_fill_text(cv, "Waffle 隸書", 64.0f, 48.0f);  // same bytes, other size

    canvas_set_text_align(cv, CANVAS_ALIGN_RIGHT);
    canvas_set_text_baseline(cv, CANVAS_BASELINE_TOP);
    canvas_set_stroke_rgba(cv, 0.1f, 0.3f, 0.1f, 1.0f);
    canvas_set_line_width(cv, 1.0f);
    canvas_stroke_text(cv, "kerning", 120.0f, 64.0f);

    canvas_set_text_align(cv, CANVAS_ALIGN_LEFT);
    canvas_set_text_baseline(cv, CANVAS_BASELINE_ALPHABETIC);
    canvas_set_font_size(cv, 21.0f);
    canvas_fill_text(cv, "a\xF0\x9F\x8C\x88z", 4.0f, 88.0f);  // a 🌈 z
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

// Count the lines of `buf` starting with `prefix` (which includes its trailing
// space, so "shape " doesn't match a hypothetical "shapex").
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

static bool metrics_eq(canvas_text_metrics a, canvas_text_metrics b) {
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
    canvas_text_metrics m17, m23, me;
    memset(&m17, 0, sizeof m17);
    memset(&m23, 0, sizeof m23);
    memset(&me, 0, sizeof me);
    {
        canvas *__single cv = canvas_create(W, H);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas_record_to(cv, path));
        draw_text_scene(cv);
        canvas_read_rgba(cv, recorded_px, (int)sizeof recorded_px);
        canvas_set_font_size(cv, 17.5f);
        w17 = canvas_measure_text(cv, "Waffle 隸書");
        m17 = canvas_measure_text_full(cv, "Waffle 隸書");
        canvas_set_font_size(cv, 23.0f);
        w23 = canvas_measure_text(cv, "Waffle 隸書");
        m23 = canvas_measure_text_full(cv, "Waffle 隸書");
        canvas_set_font_size(cv, 21.0f);
        we = canvas_measure_text(cv, "a\xF0\x9F\x8C\x88z");
        me = canvas_measure_text_full(cv, "a\xF0\x9F\x8C\x88z");
        canvas_destroy(cv);  // flush + close the file
    }
    CHECK(w17 > 0.0f && w23 > w17 && we > 0.0f);
    CHECK(me.actual_bounding_box_ascent > 0.0f);  // the emoji ink measured

    {
        canvas *__single cv = canvas_create(W, H);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas_replay_from(cv, path));

        // The proof of self-containment: every text lookup during replay hit
        // the cache the blocks pre-populated -- the boundary was never asked
        // to shape a line or fetch a glyph.
        cnvs_text_cache *__single c = cnvs_canvas_text_cache(cv);
        CHECK(c->shape_misses == 0);
        CHECK(c->glyph_misses == 0);
        CHECK(c->shape_hits > 0);
        CHECK(c->glyph_hits > 0);

        uint8_t replayed_px[NPX];
        canvas_read_rgba(cv, replayed_px, (int)sizeof replayed_px);
        CHECK(memcmp(recorded_px, replayed_px, sizeof recorded_px) == 0);

        // Measurement replays from the serialized ink bounds + vmetrics: the
        // values match the recording canvas's bit for bit, still boundary-free
        // -- the emoji line included (its ink box rides the bitmap block, and
        // both canvases scale it through the same capture-px math).
        canvas_set_font_size(cv, 17.5f);
        CHECK(feq_bits(canvas_measure_text(cv, "Waffle 隸書"), w17));
        CHECK(metrics_eq(canvas_measure_text_full(cv, "Waffle 隸書"), m17));
        canvas_set_font_size(cv, 23.0f);
        CHECK(feq_bits(canvas_measure_text(cv, "Waffle 隸書"), w23));
        CHECK(metrics_eq(canvas_measure_text_full(cv, "Waffle 隸書"), m23));
        canvas_set_font_size(cv, 21.0f);
        CHECK(feq_bits(canvas_measure_text(cv, "a\xF0\x9F\x8C\x88z"), we));
        CHECK(metrics_eq(canvas_measure_text_full(cv, "a\xF0\x9F\x8C\x88z"), me));
        CHECK(c->shape_misses == 0);
        CHECK(c->glyph_misses == 0);

        canvas_destroy(cv);
    }
}

// Dedup: drawing the same string twice (and measuring it) emits its font,
// glyph, and bitmap blocks exactly once; the second op is just its op line.
static void check_dedup(void) {
    char const *__null_terminated once = "build/test_record_text_d1.canvas";
    char const *__null_terminated twice = "build/test_record_text_d2.canvas";

    {
        canvas *__single cv = canvas_create(W, H);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas_record_to(cv, once));
        canvas_set_font_size(cv, 16.0f);
        canvas_fill_text(cv, "echo", 4.0f, 24.0f);
        canvas_fill_text(cv, "\xF0\x9F\x8D\x95", 4.0f, 48.0f);  // 🍕
        canvas_destroy(cv);
    }
    {
        canvas *__single cv = canvas_create(W, H);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas_record_to(cv, twice));
        canvas_set_font_size(cv, 16.0f);
        canvas_fill_text(cv, "echo", 4.0f, 24.0f);
        canvas_fill_text(cv, "echo", 4.0f, 48.0f);
        canvas_fill_text(cv, "\xF0\x9F\x8D\x95", 4.0f, 48.0f);
        canvas_fill_text(cv, "\xF0\x9F\x8D\x95", 40.0f, 48.0f);
        canvas_destroy(cv);
    }

    // The buffers are static: a bitmap block alone is ~137 KB of base64.
    static char a[1 << 19];
    static char b[1 << 19];
    int na = slurp(once, a, (int)sizeof a);
    int nb = slurp(twice, b, (int)sizeof b);
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
    CHECK(count_lines(a, na, "shape ") == 2);
    CHECK(count_lines(b, nb, "shape ") == 2);
    // ...and the blocks really exist (three distinct outline glyphs, one
    // capture: 160x160x4 bytes at 12288 per bits line = 9 lines).
    CHECK(count_lines(b, nb, "glyph ") >= 3);  // e, c, h, o -> >= 3 distinct
    CHECK(count_lines(b, nb, "bitmap ") == 1);
    CHECK(count_lines(b, nb, "bits ") == 9);
    CHECK(count_lines(b, nb, "run ") == 2);
    CHECK(count_lines(b, nb, "fill_text ") == 4);
}

// Strict parsing: malformed blocks stop replay (false) without corrupting the
// canvas -- it draws normally afterwards.
static void check_strict(void) {
    canvas *__single cv = canvas_create(64, 48);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }

    // Well-formed blocks parse (a tiny square glyph, a blank glyph, a shape).
    CHECK(REPLAY(cv,
        "font 0 1.05 0.33 Libian TC\n"
        "font 1 1.05 0.33 STLibianTC-Regular\n"
        "glyph 1 43 1000 10 11 592 601 m 10 11 l 592 11 l 592 601 l 10 601 z\n"
        "glyph 1 3 0 0 0 0 0\n"
        "shape 12 2 1 2 H \n"
        "run 1 0 0 2 43 7.224 0 3 2.8 1\n"
        "fill_text 4 20 H \n"));

    // Malformed blocks are rejected.
    CHECK(!REPLAY(cv, "font 16 0.5 0.2 Over\n"));            // id out of range
    CHECK(!REPLAY(cv, "font 0 0.5 0.2 A\nfont 0 0.5 0.2 B\n"));  // redeclared id
    CHECK(!REPLAY(cv, "font 0 0.5 0.2\n"));                  // empty name
    CHECK(!REPLAY(cv, "font 0 1e999 0.2 A\n"));              // overflowed float
    CHECK(!REPLAY(cv, "glyph 0 5 1000 0 0 1 1 m 1 2\n"));    // undeclared font
    CHECK(!REPLAY(cv, "font 0 1 0.2 A\nglyph 0 5 1000 0 0 1 1 m 1\n"));   // short point
    CHECK(!REPLAY(cv, "font 0 1 0.2 A\nglyph 0 5 1000 0 0 1 1 x 1 2\n")); // bad verb
    CHECK(!REPLAY(cv, "font 0 1 0.2 A\nglyph 0 70000 1000 0 0 1 1 z\n")); // gid > 0xFFFF
    CHECK(!REPLAY(cv, "font 0 1 0.2 A\nglyph 0 5 1e999 0 0 1 1 z\n"));    // inf upem
    CHECK(!REPLAY(cv, "font 0 1 0.2 A\nglyph 0 5 0 0 0 0 0 m 1 2\n"));    // blank w/ curves
    CHECK(!REPLAY(cv, "font 0 1 0.2 A\nglyph 0 5 -1 0 0 1 1 z\n"));       // negative upem
    CHECK(!REPLAY(cv, "shape 12 1 1 3 A\n"));                // byte-len mismatch
    CHECK(!REPLAY(cv, "shape 12 4 1 2 Hi\n"));               // utf16 len > bytes
    CHECK(!REPLAY(cv, "shape 12 1 1 1 A\n"));                // truncated: no run line
    CHECK(!REPLAY(cv, "shape 12 1 1 1 A\nfill_rect 0 0 4 4\n"));  // non-run inside
    CHECK(!REPLAY(cv, "shape 12 1 1 1 A\n# comment\nrun 0 0 0 0\n"));  // ditto
    CHECK(!REPLAY(cv, "run 0 0 0 0\n"));                     // run with no shape
    CHECK(!REPLAY(cv,
        "font 0 1 0.2 A\n"
        "shape 12 1 1 1 A\n"
        "run 0 0 0 1 10 5 1\n"));                            // cluster >= utf16 len
    CHECK(!REPLAY(cv,
        "shape 12 1 1 1 A\n"
        "run 3 0 0 1 10 5 0\n"));                            // undeclared run font
    CHECK(!REPLAY(cv,
        "shape 12 1 1 1 A\n"
        "run -1 0 0 1 10 1e999 0\n"));                       // overflowed advance

    // Bitmap blocks: a well-formed capture parses (2x2, two bits lines, the
    // second padded)...
    CHECK(REPLAY(cv,
        "font 0 1 0.25 AppleColorEmoji\n"
        "bitmap 0 64 2 2 0 -0.5 2 1.5 2\n"
        "bits AAAAAAAAAAAAAAAA\n"
        "bits /////w==\n"));

    // ...and malformed ones are rejected, the canvas drawable throughout.
    CHECK(!REPLAY(cv, "bitmap 0 64 1 1 0 0 1 1 1\nbits AAAAAA==\n"));  // undeclared font
    #define BM_FONT "font 0 1 0.25 AppleColorEmoji\n"
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 70000 1 1 0 0 1 1 1\nbits AAAAAA==\n"));  // gid > 0xFFFF
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 0 1 0 0 1 1 1\nbits AAAAAA==\n"));     // zero width
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 513 1 0 0 1 1 1\nbits AAAAAA==\n"));   // width > cap
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 1e999 0 1 1 1\nbits AAAAAA==\n")); // inf ink
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 1 0 1 1 1\nbits AAAAAA==\n"));     // empty ink (x1 <= x0)
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 0\nbits AAAAAA==\n"));     // zero lines
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 3\n"));                    // nlines > ceil(bytes/3)
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 1 junk\nbits AAAAAA==\n"));// trailing junk
    CHECK(!REPLAY(cv, "bits AAAAAA==\n"));                                        // bits with no bitmap
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 1\nbits AA!AAA==\n"));     // bad base64 char
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 1\nbits AAAAA\n"));        // length % 4 != 0
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 1\nbits =AAAAAAA\n"));     // '=' up front
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 1\nbits AA==AAAA\n"));     // padding mid-line
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 2 2 0 0 1 1 2\nbits AA==\nbits AAAAAAAAAAAAAAAAAAAA==\n"));  // padding before the final line
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 1\nbits AAAA\n"));         // short: 3 of 4 bytes
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 1\nbits AAAAAAAA\n"));     // long: 6 of 4 bytes
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 1\n"));                    // truncated: no bits line
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 1\nfill_rect 0 0 4 4\n")); // non-bits inside
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 1\n# comment\nbits AAAAAA==\n"));  // ditto
    CHECK(!REPLAY(cv, BM_FONT "bitmap 0 64 1 1 0 0 1 1 1\nshape 12 1 0 1 A\n"));  // shape inside
    #undef BM_FONT

    // Not corrupted: the canvas still draws.
    canvas_set_fill_rgba(cv, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, 64.0f, 48.0f);
    uint8_t px[64 * 48 * 4];
    canvas_get_image_data(cv, 0, 0, 64, 48, px, (int)sizeof px);
    CHECK(px[0] == 255);

    canvas_destroy(cv);
}

// xorshift32, run in 64-bit and masked back: the wrap is by design, so keep
// it out of -fsanitize=integer's unsigned-shift check (the mix32 pattern in
// cnvs_text.c).
static uint32_t xorshift32(uint32_t *__single s) {
    uint64_t x = *s;
    x = (x ^ (x << 13)) & 0xFFFFFFFFu;
    x = x ^ (x >> 17);
    x = (x ^ (x << 5)) & 0xFFFFFFFFu;
    *s = (uint32_t)x;
    return (uint32_t)x;
}

// One float through the recorder's emission (fprintf %.9g -- the identical
// libc formatter cnvs_record.c writes with, via an in-memory FILE) and the
// replay parser's number reader (a set_transform line, read back via
// get_transform): the reparsed float32 must be bit-identical.
static bool roundtrips(canvas *__single cv, FILE *__single f,
                       char const *__counted_by(cap) line, int cap, float v) {
    rewind(f);
    fprintf(f, "set_transform %.9g 0 0 1 0 0\n", (double)v);
    if (fflush(f) != 0) {
        return false;
    }
    long n = ftell(f);
    if (n <= 0 || n > cap) {
        return false;
    }
    if (!cnvs_replay_text(cv, line, (size_t)n)) {
        return false;
    }
    return feq_bits(canvas_get_transform(cv).a, v);
}

// The float round-trip property: emit/parse identity over denormals, negative
// zero, extremes, powers of two, and a large random sweep of bit patterns.
static void check_float_round_trip(void) {
    canvas *__single cv = canvas_create(8, 8);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    char line[64];
    FILE *__single f = fmemopen(line, sizeof line, "w");
    CHECK(f != NULL);
    if (!f) {
        canvas_destroy(cv);
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
    canvas_destroy(cv);
}

int main(void) {
    check_round_trip();
    check_dedup();
    check_strict();
    check_float_round_trip();
    return TEST_REPORT();
}
