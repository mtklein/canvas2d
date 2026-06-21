#include "canvas2d.h"
#include "test_util.h"

#include <math.h>
#include <string.h>

// The public selection/caret queries (canvas2d_text_index_at_x /
// canvas2d_text_x_at_index / canvas2d_text_selection) wrap the shaped-line logic
// covered in depth by test_shaping.c.  These checks prove the public wrappers
// thread the canvas state -- font size, direction, letterSpacing -- into the
// shaping and report positions in the same user-px-from-start space as
// canvas2d_measure_text.

int main(void) {
    struct canvas2d_context *__single cv = canvas2d(64, 64, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return TEST_REPORT();
    }
    canvas2d_set_font_size(cv, 20.0f);

    char const *__null_terminated txt = "Hello";  // Latin, covered by Libian TC
    float const w = canvas2d_measure_text(cv, txt);
    CHECK(w > 0.0f);

    // Caret at index 0 is the start; at/past the end is the advance width.
    CHECK(canvas2d_text_x_at_index(cv, txt, 0) == 0.0f);
    float const x_end = canvas2d_text_x_at_index(cv, txt, (int)strlen(txt));
    CHECK(fabsf(x_end - w) < 0.5f);
    // Past the end clamps to the same end caret.
    CHECK(fabsf(canvas2d_text_x_at_index(cv, txt, 999) - w) < 0.5f);

    // Caret x is monotonic non-decreasing across logical indices (LTR text).
    float prev = -1.0f;
    for (int i = 0; i <= (int)strlen(txt); i++) {
        float const xi = canvas2d_text_x_at_index(cv, txt, i);
        CHECK(xi >= prev - 0.001f);
        CHECK(xi >= 0.0f && xi <= w + 0.5f);
        prev = xi;
    }

    // Round trip: hit-testing a caret x lands back on that index's cluster.  The
    // caret x is the cluster's leading edge; a hair inside it hits the cluster.
    for (int i = 0; i < (int)strlen(txt); i++) {
        float const xi = canvas2d_text_x_at_index(cv, txt, i);
        int const hit = canvas2d_text_index_at_x(cv, txt, xi + 0.1f);
        CHECK(hit == i);
    }

    // A visual x far past the line's end maps to no index.
    CHECK(canvas2d_text_index_at_x(cv, txt, w + 100.0f) == -1);

    // Full-range selection collapses to one span covering ~the whole width.
    canvas2d_text_span sp[8];
    int const nfull = canvas2d_text_selection(cv, txt, 0, (int)strlen(txt), sp, 8);
    CHECK(nfull == 1);
    CHECK(sp[0].x0 == 0.0f);
    CHECK(fabsf(sp[0].x1 - w) < 0.5f);

    // A sub-range yields >= 1 span, each within [0, width] and ordered.
    int const nsub = canvas2d_text_selection(cv, txt, 1, 3, sp, 8);
    CHECK(nsub >= 1);
    for (int k = 0; k < nsub; k++) {
        CHECK(sp[k].x0 <= sp[k].x1);
        CHECK(sp[k].x0 >= 0.0f && sp[k].x1 <= w + 0.5f);
    }

    // letterSpacing shifts caret positions: a positive spacing grows an interior
    // caret x versus no spacing (it rides each cluster's advance).
    float const mid_no_ls = canvas2d_text_x_at_index(cv, txt, 3);
    canvas2d_set_letter_spacing(cv, 5.0f);
    float const mid_ls = canvas2d_text_x_at_index(cv, txt, 3);
    CHECK(mid_ls > mid_no_ls);
    // And the full width grows with it.
    CHECK(canvas2d_measure_text(cv, txt) > w);
    canvas2d_set_letter_spacing(cv, 0.0f);

    // Edges: empty text, max <= 0, NULL out -> no crash, sensible empties.
    CHECK(canvas2d_text_x_at_index(cv, "", 0) == 0.0f);
    CHECK(canvas2d_text_x_at_index(cv, "", 5) == 0.0f);
    CHECK(canvas2d_text_index_at_x(cv, "", 0.0f) == -1
       || canvas2d_text_index_at_x(cv, "", 0.0f) == 0);  // empty line: no glyphs to hit
    CHECK(canvas2d_text_selection(cv, "", 0, 0, sp, 8) == 0);
    CHECK(canvas2d_text_selection(cv, txt, 0, (int)strlen(txt), sp, 0) == 0);
    // max == 0 with a NULL buffer: a 0-count __counted_by buffer may be NULL, so
    // this reaches the wrapper's own (!out || max <= 0) guard.  A negative max or a
    // NULL buffer with a positive count can't be expressed: -fbounds-safety traps
    // such a call at the boundary before it ever reaches the wrapper.
    canvas2d_text_span *__counted_by(0) none = 0;  // a NULL buffer with count 0
    CHECK(canvas2d_text_selection(cv, txt, 0, (int)strlen(txt), none, 0) == 0);
    // An inverted range selects nothing.
    CHECK(canvas2d_text_selection(cv, txt, 3, 1, sp, 8) == 0);

    // Bonus: an RTL/bidi string still reports finite, in-range positions through
    // the public wrappers (the bidi split logic itself is covered by test_shaping).
    canvas2d_set_direction(cv, CANVAS2D_DIRECTION_RTL);
    char const *__null_terminated bidi = "Hi \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D!";  // "Hi " + Hebrew + "!"
    float const wb = canvas2d_measure_text(cv, bidi);
    CHECK(wb > 0.0f);
    int const ub = (int)strlen(bidi);  // an upper bound on the UTF-16 length
    canvas2d_text_span bsp[16];
    int const nb = canvas2d_text_selection(cv, bidi, 1, 5, bsp, 16);
    CHECK(nb >= 1);
    for (int k = 0; k < nb; k++) {
        CHECK(bsp[k].x0 <= bsp[k].x1);
        CHECK(bsp[k].x0 >= 0.0f && bsp[k].x1 <= wb + 0.5f);
    }
    for (int i = 0; i <= ub; i++) {
        float const xi = canvas2d_text_x_at_index(cv, bidi, i);
        CHECK(xi >= 0.0f && xi <= wb + 0.5f);
    }
    canvas2d_set_direction(cv, CANVAS2D_DIRECTION_LTR);

    canvas2d_free(cv);
    return TEST_REPORT();
}
