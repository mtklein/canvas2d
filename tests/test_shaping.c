#include "cnvs_text.h"
#include "test_util.h"

#include <stdlib.h>
#include <string.h>

// The shaping seam is counted -- cnvs_shape_text takes (bytes, len) pairs, never a NUL
// contract -- and a string literal carries its byte length at compile time, so
// S(lit) expands to exactly the pair the seam wants: no strlen, no bridge.
#define S(lit) ("" lit), ((int)sizeof lit - 1)

// Shaping output is font/OS-dependent, so assert structural invariants, not exact
// metrics: runs exist, every cluster index is within the source string, the width is
// positive, and hit-tests round-trip to valid source indices.  expect_rtl requires
// at least one run to report right-to-left.
static void check_shape(char const *__counted_by(len) text, int len, bool expect_rtl) {
    struct cnvs_shaped *s = cnvs_shape_text(S("Helvetica"), 20.0f, false, 400, false, text, len);
    CHECK(s != NULL);
    if (!s) {
        return;
    }
    CHECK(s->nruns >= 1);
    CHECK(s->utf16s > 0);

    bool clusters_ok = true, any_rtl = false;
    for (int r = 0; r < s->nruns; r++) {
        struct cnvs_glyph_run const run = s->run[r];
        CHECK(run.count > 0);
        any_rtl = any_rtl || run.rtl;
        for (int i = 0; i < run.count; i++) {
            if (run.cluster[i] < 0 || run.cluster[i] >= s->utf16s) {
                clusters_ok = false;
            }
        }
    }
    CHECK(clusters_ok);
    if (expect_rtl) {
        CHECK(any_rtl);
    }

    float const w = cnvs_shaped_width(s);
    CHECK(w > 0.0f);
    int const i0 = cnvs_shaped_index_at_x(s, 0.0f);
    int const i1 = cnvs_shaped_index_at_x(s, w * 0.99f);
    CHECK(i0 >= 0 && i0 < s->utf16s);
    CHECK(i1 >= 0 && i1 < s->utf16s);
    CHECK(cnvs_shaped_index_at_x(s, w + 100.0f) == -1);  // past the end

    cnvs_shaped_free(s);
}

// Font fallback: a mixed Latin+emoji string must use >= 2 distinct fonts across its
// runs, and the boundary must fill the name buffer within the caller's cap.
static void check_fallback(void) {
    struct cnvs_shaped *s = cnvs_shape_text(S("Helvetica"), 20.0f, false, 400, false, S("A\xF0\x9F\x98\x80Z"));  // A 😀 Z
    CHECK(s != NULL);
    if (!s) {
        return;
    }
    char name0[128] = { 0 };
    int const n0 = cnvs_run_font_name(s->run[0].font, name0, (int)sizeof name0);
    CHECK(n0 > 0);
    bool distinct = false;
    for (int r = 1; r < s->nruns; r++) {
        char nm[128] = { 0 };
        int const n = cnvs_run_font_name(s->run[r].font, nm, (int)sizeof nm);
        CHECK(n > 0);
        // Compare via the returned lengths + memcmp (the sized model), not strcmp
        // (the null-terminated model, which needs an unsafe bridge from an indexable
        // buffer -- see docs/text-boundary.md).
        if (n != n0 || memcmp(nm, name0, (size_t)n) != 0) {
            distinct = true;
        }
    }
    CHECK(distinct);  // the emoji forced a fallback to a different font

    // The boundary must respect a tiny cap: only the first 4 bytes may be written.
    char guard[8] = "ZZZZZZZ";
    cnvs_run_font_name(s->run[0].font, guard, 4);
    CHECK(guard[4] == 'Z' && guard[5] == 'Z' && guard[6] == 'Z');

    cnvs_shaped_free(s);
}

// Positioned outlines: shaping + layout (checked) + per-glyph outline (boundary).
// Latin text produces geometry; a color-emoji glyph has an advance but NO outline
// path -- the gap that the bitmap boundary will fill.
static void check_outline(void) {
    struct cnvs_shaped *s = cnvs_shape_text(S("Helvetica"), 40.0f, false, 400, false, S("ffi"));
    CHECK(s != NULL);
    if (s) {
        struct cnvs_path p;
        cnvs_path_init(&p);
        float w = cnvs_shaped_outline(NULL, s, 0.0f, 0.0f, cnvs_mat_identity(),
                                      0.25f, &p, NULL, NULL);
        CHECK(w > 0.0f);
        CHECK(p.npts > 0 && p.nsubs > 0);   // "ffi" produced outline geometry
        cnvs_path_free(&p);
        cnvs_shaped_free(s);
    }

    struct cnvs_shaped *e = cnvs_shape_text(S("Helvetica"), 40.0f, false, 400, false, S("\xF0\x9F\x98\x80"));  // lone emoji
    CHECK(e != NULL);
    if (e) {
        struct cnvs_path p;
        cnvs_path_init(&p);
        float w = cnvs_shaped_outline(NULL, e, 0.0f, 0.0f, cnvs_mat_identity(),
                                      0.25f, &p, NULL, NULL);
        CHECK(w > 0.0f);          // it has an advance (occupies space)
        CHECK(p.npts == 0);     // but a color glyph has no outline path
        cnvs_path_free(&p);
        cnvs_shaped_free(e);
    }
}

// Color emoji: no outline, so it must be drawn into a pixel buffer.  The checked
// core owns the __counted_by(w*h*4) buffer; the boundary fills it via CGBitmapContext.
static void check_emoji_draw(void) {
    struct cnvs_shaped *s = cnvs_shape_text(S("Helvetica"), 40.0f, false, 400, false, S("\xF0\x9F\x98\x80"));  // 😀
    CHECK(s != NULL);
    if (!s) {
        return;
    }
    CHECK(s->nruns >= 1 && s->run[0].count >= 1);
    if (s->nruns >= 1 && s->run[0].count >= 1) {
        int const W = 64, H = 64, LEN = W * H * 4;
        uint8_t *__counted_by(LEN) px = calloc((size_t)LEN, 1);
        CHECK(px != NULL);
        if (px) {
            cnvs_glyph_draw(s->run[0].font, s->run[0].glyph[0], 12.0f, 12.0f, px, W, H);
            int nonzero = 0, maxa = 0;
            bool colored = false;
            for (int i = 0; i < LEN; i += 4) {
                if (px[i] | px[i + 1] | px[i + 2] | px[i + 3]) {
                    nonzero++;
                }
                if (px[i] != px[i + 1] || px[i + 1] != px[i + 2]) {
                    colored = true;  // a channel differs -> actual colour, not coverage
                }
                if (px[i + 3] > maxa) {
                    maxa = px[i + 3];
                }
            }
            CHECK(nonzero > 0);  // the color glyph rendered into the buffer
            CHECK(colored);      // and in colour (not just grayscale coverage)
            CHECK(maxa > 0);     // with real opacity
        }
        free(px);
    }
    cnvs_shaped_free(s);
}

static void check_bidi(void) {
    struct cnvs_shaped *s = cnvs_shape_text(S("Helvetica"), 20.0f, false, 400, false, S("Hi \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D!"));
    CHECK(s != NULL);
    if (!s) {
        return;
    }
    float const w = cnvs_shaped_width(s);
    cnvs_xspan sp[8];

    // Full selection collapses to one span covering the whole line.
    int const nfull = cnvs_shaped_selection(s, 0, s->utf16s, sp, 8);
    CHECK(nfull == 1);
    CHECK(sp[0].x0 == 0.0f && sp[0].x1 > w - 0.5f && sp[0].x1 < w + 0.5f);

    // A logical range crossing the LTR<->RTL boundary maps to non-contiguous visual
    // positions, so it splits into >= 2 spans (the point of bidi selection).
    int const nsplit = cnvs_shaped_selection(s, 1, 5, sp, 8);
    CHECK(nsplit >= 2);
    for (int k = 0; k < nsplit; k++) {
        CHECK(sp[k].x0 <= sp[k].x1 && sp[k].x0 >= 0.0f && sp[k].x1 <= w + 0.5f);
    }

    // The output cap is respected even when more spans exist.
    CHECK(cnvs_shaped_selection(s, 1, 5, sp, 1) <= 1);

    // Caret at the line start is 0; an index past the end is the line width.
    CHECK(cnvs_shaped_x_at_index(s, 0) == 0.0f);
    float const xe = cnvs_shaped_x_at_index(s, s->utf16s);
    CHECK(xe > w - 0.5f && xe < w + 0.5f);

    cnvs_shaped_free(s);
}

// Caret snapping: an index INSIDE a cluster -- here the low half of an emoji's
// surrogate pair -- has no glyph of its own, so the caret snaps to the enclosing
// cluster's leading edge rather than falling off the end of the line.
static void check_caret_snap(void) {
    struct cnvs_shaped *s = cnvs_shape_text(S("Helvetica"), 20.0f, false, 400, false, S("a\xF0\x9F\x98\x80z"));  // a 😀 z
    CHECK(s != NULL);
    if (!s) {
        return;
    }
    CHECK(s->utf16s == 4);  // 'a' + the surrogate pair + 'z'
    float const w = cnvs_shaped_width(s);
    float const at_emoji = cnvs_shaped_x_at_index(s, 1);  // the pair's cluster start
    float const inside = cnvs_shaped_x_at_index(s, 2);    // its low surrogate
    CHECK(inside - at_emoji == 0.0f);                // snapped to the cluster's edge
    CHECK(inside < w);                               // and NOT off the end of the line
    CHECK(cnvs_shaped_x_at_index(s, 3) > at_emoji);  // 'z' sits past the emoji
    CHECK(cnvs_shaped_x_at_index(s, 4) > w - 0.5f);  // end-of-line caret
    for (int i = 0; i <= s->utf16s; i++) {           // every caret lands on the line
        float const x = cnvs_shaped_x_at_index(s, i);
        CHECK(x >= 0.0f && x <= w + 0.5f);
    }
    cnvs_shaped_free(s);
}

// The paragraph base direction (the canvas direction attribute) changes how a
// mixed-direction line lays out and how its neutrals resolve -- same glyphs,
// same advances, different visual order.  Both shapings must measure alike.
static void check_paragraph_direction(void) {
    // "Hi שלום!": under an ltr paragraph the Latin leads (the 'H' is the
    // leftmost glyph); under rtl the whole line reorders and the 'H' moves
    // right of the Hebrew.
    struct cnvs_shaped *ltr = cnvs_shape_text(S("Helvetica"), 20.0f, false, 400, false,
                                  S("Hi \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D!"));
    struct cnvs_shaped *rtl = cnvs_shape_text(S("Helvetica"), 20.0f, true, 400, false,
                                  S("Hi \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D!"));
    CHECK(ltr != NULL && rtl != NULL);
    if (ltr && rtl) {
        CHECK(cnvs_shaped_x_at_index(ltr, 0) == 0.0f);
        CHECK(cnvs_shaped_x_at_index(rtl, 0) > 0.0f);
        // Same glyphs in a different order: the advance widths agree (the sum
        // reorders, so allow the last-ulp wobble of float addition).
        float wl = cnvs_shaped_width(ltr), wr = cnvs_shaped_width(rtl);
        CHECK(wl > 0.0f);
        float const d = wl - wr;
        CHECK(d < 0.01f && d > -0.01f);
    }
    cnvs_shaped_free(ltr);
    cnvs_shaped_free(rtl);

    // Neutral resolution: the trailing "!" of an all-Hebrew line sits on the
    // base-direction side -- visual right (x > 0) under ltr, visual left
    // (x == 0) under rtl.  This is exactly the case first-strong would get
    // wrong for direction=ltr, so it also pins that the base is explicit.
    struct cnvs_shaped *nl = cnvs_shape_text(S("Helvetica"), 20.0f, false, 400, false,
                                 S("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D!"));
    struct cnvs_shaped *nr = cnvs_shape_text(S("Helvetica"), 20.0f, true, 400, false,
                                 S("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D!"));
    CHECK(nl != NULL && nr != NULL);
    if (nl && nr) {
        CHECK(cnvs_shaped_x_at_index(nl, 4) > 0.0f);   // "!" at the right
        CHECK(cnvs_shaped_x_at_index(nr, 4) == 0.0f);  // "!" at the left
    }
    cnvs_shaped_free(nl);
    cnvs_shaped_free(nr);
}

int main(void) {
    check_shape(S("ffi waffle"), false);             // Latin with ligatures (cluster gaps)
    check_shape(S("a\xF0\x9F\x98\x80""b"), false);   // a + U+1F600 emoji + b (multi-run)
    check_shape(S("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D"), true);  // Hebrew "shalom" (RTL)
    check_shape(S("Hi \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D!"), true);  // mixed bidi
    check_fallback();
    check_outline();
    check_emoji_draw();
    check_bidi();
    check_caret_snap();
    check_paragraph_direction();
    return TEST_REPORT();
}
