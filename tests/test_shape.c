#include "cnvs_shape.h"
#include "test_util.h"

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

int main(void) {
    check_shape("ffi waffle", false);             // Latin with ligatures (cluster gaps)
    check_shape("a\xF0\x9F\x98\x80""b", false);   // a + U+1F600 emoji + b (multi-run)
    check_shape("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D", true);  // Hebrew "shalom" (RTL)
    check_shape("Hi \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D!", true);  // mixed bidi
    return TEST_REPORT();
}
