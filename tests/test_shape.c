#include "cnvs_shape.h"
#include "test_util.h"

#include <stdlib.h>
#include <string.h>

// Shaping output is font/OS-dependent, so assert structural invariants, not exact
// metrics: runs exist, every cluster index is within the source string, the width is
// positive, and hit-tests round-trip to valid source indices.  expect_rtl requires
// at least one run to report right-to-left.
static void check_shape(char const *text, bool expect_rtl) {
    cnvs_shaped *s = cnvs_shape("Helvetica", 20.0f, text);
    CHECK(s != NULL);
    if (!s) {
        return;
    }
    CHECK(s->nruns >= 1);
    CHECK(s->text_len > 0);

    bool clusters_ok = true, any_rtl = false;
    for (int r = 0; r < s->nruns; r++) {
        cnvs_glyph_run run = s->run[r];
        CHECK(run.count > 0);
        any_rtl = any_rtl || run.rtl;
        for (int i = 0; i < run.count; i++) {
            if (run.cluster[i] < 0 || run.cluster[i] >= s->text_len) {
                clusters_ok = false;
            }
        }
    }
    CHECK(clusters_ok);
    if (expect_rtl) {
        CHECK(any_rtl);
    }

    float w = cnvs_shaped_width(s);
    CHECK(w > 0.0f);
    int i0 = cnvs_shaped_index_at_x(s, 0.0f);
    int i1 = cnvs_shaped_index_at_x(s, w * 0.99f);
    CHECK(i0 >= 0 && i0 < s->text_len);
    CHECK(i1 >= 0 && i1 < s->text_len);
    CHECK(cnvs_shaped_index_at_x(s, w + 100.0f) == -1);  // past the end

    cnvs_shaped_free(s);
}

// Font fallback: a mixed Latin+emoji string must use >= 2 distinct fonts across its
// runs, and the boundary must fill the name buffer within the caller's cap.
static void check_fallback(void) {
    cnvs_shaped *s = cnvs_shape("Helvetica", 20.0f, "A\xF0\x9F\x98\x80Z");  // A 😀 Z
    CHECK(s != NULL);
    if (!s) {
        return;
    }
    char name0[128] = { 0 };
    int n0 = cnvs_run_font_name(s->run[0].font, name0, (int)sizeof name0);
    CHECK(n0 > 0);
    bool distinct = false;
    for (int r = 1; r < s->nruns; r++) {
        char nm[128] = { 0 };
        int n = cnvs_run_font_name(s->run[r].font, nm, (int)sizeof nm);
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
    cnvs_shaped *s = cnvs_shape("Helvetica", 40.0f, "ffi");
    CHECK(s != NULL);
    if (s) {
        cnvs_path p;
        cnvs_path_init(&p);
        float w = cnvs_shaped_outline(s, 0.0f, 0.0f, cnvs_mat_identity(), 0.25f, &p);
        CHECK(w > 0.0f);
        CHECK(p.pt_len > 0 && p.sp_len > 0);   // "ffi" produced outline geometry
        cnvs_path_free(&p);
        cnvs_shaped_free(s);
    }

    cnvs_shaped *e = cnvs_shape("Helvetica", 40.0f, "\xF0\x9F\x98\x80");  // lone emoji
    CHECK(e != NULL);
    if (e) {
        cnvs_path p;
        cnvs_path_init(&p);
        float w = cnvs_shaped_outline(e, 0.0f, 0.0f, cnvs_mat_identity(), 0.25f, &p);
        CHECK(w > 0.0f);          // it has an advance (occupies space)
        CHECK(p.pt_len == 0);     // but a color glyph has no outline path
        cnvs_path_free(&p);
        cnvs_shaped_free(e);
    }
}

// Color emoji: no outline, so it must be drawn into a pixel buffer.  The checked
// core owns the __counted_by(w*h*4) buffer; the boundary fills it via CGBitmapContext.
static void check_emoji_draw(void) {
    cnvs_shaped *s = cnvs_shape("Helvetica", 40.0f, "\xF0\x9F\x98\x80");  // 😀
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
    cnvs_shaped *s = cnvs_shape("Helvetica", 20.0f, "Hi \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D!");
    CHECK(s != NULL);
    if (!s) {
        return;
    }
    float w = cnvs_shaped_width(s);
    cnvs_xspan sp[8];

    // Full selection collapses to one span covering the whole line.
    int nfull = cnvs_shaped_selection(s, 0, s->text_len, sp, 8);
    CHECK(nfull == 1);
    CHECK(sp[0].x0 == 0.0f && sp[0].x1 > w - 0.5f && sp[0].x1 < w + 0.5f);

    // A logical range crossing the LTR<->RTL boundary maps to non-contiguous visual
    // positions, so it splits into >= 2 spans (the point of bidi selection).
    int nsplit = cnvs_shaped_selection(s, 1, 5, sp, 8);
    CHECK(nsplit >= 2);
    for (int k = 0; k < nsplit; k++) {
        CHECK(sp[k].x0 <= sp[k].x1 && sp[k].x0 >= 0.0f && sp[k].x1 <= w + 0.5f);
    }

    // The output cap is respected even when more spans exist.
    CHECK(cnvs_shaped_selection(s, 1, 5, sp, 1) <= 1);

    // Caret at the line start is 0; an index past the end is the line width.
    CHECK(cnvs_shaped_x_at_index(s, 0) == 0.0f);
    float xe = cnvs_shaped_x_at_index(s, s->text_len);
    CHECK(xe > w - 0.5f && xe < w + 0.5f);

    cnvs_shaped_free(s);
}

int main(void) {
    check_shape("ffi waffle", false);             // Latin with ligatures (cluster gaps)
    check_shape("a\xF0\x9F\x98\x80""b", false);   // a + U+1F600 emoji + b (multi-run)
    check_shape("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D", true);  // Hebrew "shalom" (RTL)
    check_shape("Hi \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D!", true);  // mixed bidi
    check_fallback();
    check_outline();
    check_emoji_draw();
    check_bidi();
    return TEST_REPORT();
}
