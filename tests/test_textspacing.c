// CSS canvas letterSpacing / wordSpacing: the spacing is baked into the shaped
// advances (canvas2d_shaped_apply_spacing), keyed in the text cache, and ridden by
// both measureText and the draw walk -- so this test drives measureText for the
// width math and a record->replay round trip for the determinism posture.

#include "canvas2d.h"
#include "test_util.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

// Exact (bit-for-bit) float equality, the spacing no-op assertions' tool: the
// suite forbids a bare == on floats (-Wfloat-equal), so spell it as a zero
// absolute difference.
static bool exact(float a, float b) {
    return fabsf(a - b) <= 0.0f;
}

int main(void) {
    struct canvas2d_context *__single cv = canvas2d(96, 64, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return TEST_REPORT();
    }
    canvas2d_set_font_size(cv, 20.0f);

    // "ab cd ef" -- eight clusters (plain ASCII, no ligatures in Libian TC) and
    // two U+0020 spaces.  The width math below derives the cluster count from
    // the measured delta, so it does not hard-code the font's per-glyph advances.
    char const *__null_terminated line = "ab cd ef";
    int const spaces = 2;  // count of U+0020 in `line`

    // Baseline width with no spacing.
    float const w0 = canvas2d_measure_text(cv, line);
    CHECK(w0 > 0.0f);

    // Zero spacing is an exact no-op: equal to the no-spacing width to the bit.
    canvas2d_set_letter_spacing(cv, 0.0f);
    canvas2d_set_word_spacing(cv, 0.0f);
    CHECK(exact(canvas2d_measure_text(cv, line), w0));

    // letterSpacing adds (cluster count) * ls.  Derive the cluster count from
    // the delta at ls = 1; it must be a positive integer and the relationship
    // must hold linearly at a second, different ls.
    canvas2d_set_letter_spacing(cv, 1.0f);
    float const w_ls1 = canvas2d_measure_text(cv, line);
    float const clusters_f = w_ls1 - w0;             // == cluster count * 1
    CHECK(clusters_f > 0.0f);
    int const clusters = (int)lroundf(clusters_f);
    CHECK(fabsf(clusters_f - (float)clusters) < 1e-3f);
    CHECK(clusters == 8);  // "ab cd ef" is eight ASCII clusters

    canvas2d_set_letter_spacing(cv, 3.0f);
    float const w_ls3 = canvas2d_measure_text(cv, line);
    CHECK(fabsf((w_ls3 - w0) - 3.0f * (float)clusters) < 1e-3f);
    canvas2d_set_letter_spacing(cv, 0.0f);

    // wordSpacing adds (space count) * ws, and nothing for the non-space
    // clusters.
    canvas2d_set_word_spacing(cv, 5.0f);
    float const w_ws5 = canvas2d_measure_text(cv, line);
    CHECK(fabsf((w_ws5 - w0) - 5.0f * (float)spaces) < 1e-3f);
    canvas2d_set_word_spacing(cv, 0.0f);

    // Combined: the two contributions add.
    canvas2d_set_letter_spacing(cv, 2.0f);
    canvas2d_set_word_spacing(cv, 4.0f);
    float const w_both = canvas2d_measure_text(cv, line);
    CHECK(fabsf((w_both - w0)
                - (2.0f * (float)clusters + 4.0f * (float)spaces)) < 1e-3f);
    canvas2d_set_letter_spacing(cv, 0.0f);
    canvas2d_set_word_spacing(cv, 0.0f);

    // Negative spacing shrinks the line (and stays linear).
    canvas2d_set_letter_spacing(cv, -1.0f);
    float const w_neg = canvas2d_measure_text(cv, line);
    CHECK(w_neg < w0);
    CHECK(fabsf((w0 - w_neg) - (float)clusters) < 1e-3f);
    canvas2d_set_letter_spacing(cv, 0.0f);

    // A word with no spaces is unaffected by wordSpacing.
    float const ns0 = canvas2d_measure_text(cv, "abcdef");
    canvas2d_set_word_spacing(cv, 100.0f);
    CHECK(exact(canvas2d_measure_text(cv, "abcdef"), ns0));
    canvas2d_set_word_spacing(cv, 0.0f);

    // Empty text is a no-op under any spacing.
    canvas2d_set_letter_spacing(cv, 7.0f);
    canvas2d_set_word_spacing(cv, 7.0f);
    CHECK(exact(canvas2d_measure_text(cv, ""), 0.0f));
    canvas2d_set_letter_spacing(cv, 0.0f);
    canvas2d_set_word_spacing(cv, 0.0f);

    // Guards: NaN / inf spacing is treated as 0 (the width is unchanged).
    canvas2d_set_letter_spacing(cv, (float)NAN);
    canvas2d_set_word_spacing(cv, (float)INFINITY);
    CHECK(exact(canvas2d_measure_text(cv, line), w0));
    canvas2d_set_letter_spacing(cv, -(float)INFINITY);
    CHECK(exact(canvas2d_measure_text(cv, line), w0));
    canvas2d_set_letter_spacing(cv, 0.0f);
    canvas2d_set_word_spacing(cv, 0.0f);

    // reset() restores the 0 defaults: a non-zero spacing does not survive it.
    canvas2d_set_letter_spacing(cv, 9.0f);
    canvas2d_set_word_spacing(cv, 9.0f);
    CHECK(canvas2d_measure_text(cv, line) > w0);
    canvas2d_reset(cv);
    canvas2d_set_font_size(cv, 20.0f);  // reset also restored the 10px default size
    CHECK(exact(canvas2d_measure_text(cv, line), w0));

    canvas2d_free(cv);

    // Record -> replay round trip WITH non-zero spacing reproduces the surface
    // byte-for-byte: letterSpacing/wordSpacing record as ordinary state ops
    // (set_letter_spacing/set_word_spacing, like set_font_size), and the shaped
    // line's baked advances ride its shape block keyed by (size, rtl, ls, ws,
    // text) -- so a fontless replay restores the spacing state from the ops and
    // reconstructs the spaced line from its block.
    enum { W = 160, H = 48, NPX = W * H * 4 };
    char const *__null_terminated path = "build/test_textspacing.canvas";
    uint8_t recorded_px[NPX];
    {
        struct canvas2d_context *__single rc = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(rc != NULL);
        if (rc) {
            CHECK(canvas2d_record_to(rc, path));
            canvas2d_set_font_size(rc, 18.0f);
            canvas2d_set_fill_rgba(rc, CANVAS2D_CS_SRGB, 0.9f, 0.9f, 0.95f, 1.0f);
            canvas2d_set_letter_spacing(rc, 3.0f);
            canvas2d_set_word_spacing(rc, 8.0f);
            canvas2d_fill_text(rc, "a b c", 4.0f, 30.0f);
            canvas2d_read_rgba(rc, CANVAS2D_CS_SRGB, recorded_px, (int)sizeof recorded_px);
            canvas2d_free(rc);  // flush + close
        }
    }
    // The spaced draw actually put ink down.
    {
        bool any = false;
        for (int i = 0; i < NPX; i++) {
            if (recorded_px[i] != 0) { any = true; break; }
        }
        CHECK(any);
    }
    {
        struct canvas2d_context *__single pc = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(pc != NULL);
        if (pc) {
            CHECK(canvas2d_replay_from(pc, path));
            uint8_t replayed_px[NPX];
            canvas2d_read_rgba(pc, CANVAS2D_CS_SRGB, replayed_px, (int)sizeof replayed_px);
            CHECK(memcmp(recorded_px, replayed_px, sizeof recorded_px) == 0);
            canvas2d_free(pc);
        }
    }

    // Spacing set, CHANGED, then RE-USED: the same text drawn under spacing A,
    // then under spacing B, then under A again.  The third draw's shape block is
    // already in the file (keyed under A), so the recorder re-emits nothing for
    // it -- the set_letter_spacing op recorded before it is what restores the
    // canvas's spacing state so the op keys the right block.  This is the
    // ordering the removed re-emit hack existed for; it must still round-trip
    // byte-for-byte.
    char const *__null_terminated path2 = "build/test_textspacing_reuse.canvas";
    uint8_t reuse_px[NPX];
    {
        struct canvas2d_context *__single rc = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(rc != NULL);
        if (rc) {
            CHECK(canvas2d_record_to(rc, path2));
            canvas2d_set_font_size(rc, 18.0f);
            canvas2d_set_fill_rgba(rc, CANVAS2D_CS_SRGB, 0.9f, 0.9f, 0.95f, 1.0f);
            canvas2d_set_letter_spacing(rc, 2.0f);   // spacing A
            canvas2d_fill_text(rc, "x y z", 4.0f, 20.0f);
            canvas2d_set_letter_spacing(rc, 6.0f);   // spacing B
            canvas2d_fill_text(rc, "x y z", 4.0f, 32.0f);
            canvas2d_set_letter_spacing(rc, 2.0f);   // back to A -- reuses A's block
            canvas2d_fill_text(rc, "x y z", 4.0f, 44.0f);
            canvas2d_read_rgba(rc, CANVAS2D_CS_SRGB, reuse_px, (int)sizeof reuse_px);
            canvas2d_free(rc);
        }
    }
    {
        struct canvas2d_context *__single pc = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(pc != NULL);
        if (pc) {
            CHECK(canvas2d_replay_from(pc, path2));
            uint8_t replayed_px[NPX];
            canvas2d_read_rgba(pc, CANVAS2D_CS_SRGB, replayed_px, (int)sizeof replayed_px);
            CHECK(memcmp(reuse_px, replayed_px, sizeof reuse_px) == 0);
            canvas2d_free(pc);
        }
    }

    return TEST_REPORT();
}
