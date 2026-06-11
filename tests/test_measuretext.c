#include "canvas.h"
#include "test_util.h"

#include <math.h>

int main(void) {
    struct canvas *__single cv = canvas_create(64, 64);
    CHECK(cv != NULL);
    if (!cv) {
        return TEST_REPORT();
    }
    canvas_set_font_size(cv, 20.0f);

    // width matches the simple measureText, and a non-empty string is positive.
    canvas_text_metrics h = canvas_measure_text_full(cv, "H");
    CHECK(h.width > 0.0f);
    CHECK(fabsf(h.width - canvas_measure_text(cv, "H")) < 1e-3f);

    // Every field is finite.
    CHECK(isfinite(h.actual_bounding_box_left));
    CHECK(isfinite(h.actual_bounding_box_right));
    CHECK(isfinite(h.actual_bounding_box_ascent));
    CHECK(isfinite(h.actual_bounding_box_descent));

    // A capital glyph rises above the baseline.
    CHECK(h.actual_bounding_box_ascent > 0.0f);

    // Font-wide metrics are positive and don't depend on the text.
    CHECK(h.font_bounding_box_ascent > 0.0f);
    CHECK(h.font_bounding_box_descent >= 0.0f);

    // The em square splits into ascent + descent summing to the font size.
    CHECK(fabsf((h.em_height_ascent + h.em_height_descent) - 20.0f) < 0.01f);

    // Baselines, relative to the alphabetic baseline (the reference at 0).
    CHECK(h.alphabetic_baseline == 0.0f);
    CHECK(h.hanging_baseline >= 0.0f);
    CHECK(h.ideographic_baseline <= 0.0f);

    // Repeating the same glyph doubles the advance exactly.
    canvas_text_metrics hh = canvas_measure_text_full(cv, "HH");
    CHECK(fabsf(hh.width - 2.0f * h.width) < 0.05f);

    // A longer run has a wider actual bounding box.
    canvas_text_metrics h4 = canvas_measure_text_full(cv, "HHHH");
    CHECK(h4.actual_bounding_box_right > h.actual_bounding_box_right);

    // Empty string: zero advance and a zero *actual* box, but the font-wide
    // metrics still describe the current font.
    canvas_text_metrics e = canvas_measure_text_full(cv, "");
    CHECK(e.width == 0.0f);
    CHECK(e.actual_bounding_box_ascent == 0.0f);
    CHECK(e.actual_bounding_box_descent == 0.0f);
    CHECK(e.actual_bounding_box_left == 0.0f);
    CHECK(e.actual_bounding_box_right == 0.0f);
    CHECK(e.font_bounding_box_ascent > 0.0f);

    canvas_destroy(cv);
    return TEST_REPORT();
}
