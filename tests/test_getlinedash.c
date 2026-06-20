#include "canvas.h"
#include "test_util.h"

#include <math.h>

int main(void) {
    struct canvas *__single cv = canvas(16, 16, CANVAS_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return TEST_REPORT();
    }

    // The default dash pattern is empty (solid).
    float out[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    CHECK(canvas_get_line_dash(cv, out, 8) == 0);

    // Round-trip an even pattern.
    float const dash[4] = { 4.0f, 2.0f, 6.0f, 1.0f };
    canvas_set_line_dash(cv, dash, 4);
    int const n = canvas_get_line_dash(cv, out, 8);
    CHECK(n == 4);
    CHECK(fabsf(out[0] - 4.0f) < 1e-6f);
    CHECK(fabsf(out[1] - 2.0f) < 1e-6f);
    CHECK(fabsf(out[2] - 6.0f) < 1e-6f);
    CHECK(fabsf(out[3] - 1.0f) < 1e-6f);

    // Querying with cap 0 (and a NULL buffer) returns the length without writing.
    CHECK(canvas_get_line_dash(cv, NULL, 0) == 4);

    // A cap smaller than the pattern copies only `cap` entries but still reports
    // the true length, and leaves the tail of the caller buffer untouched.
    float two[2] = { -1.0f, -1.0f };
    CHECK(canvas_get_line_dash(cv, two, 2) == 4);
    CHECK(fabsf(two[0] - 4.0f) < 1e-6f);
    CHECK(fabsf(two[1] - 2.0f) < 1e-6f);

    // The returned values are a copy: mutating them doesn't change canvas state.
    out[0] = 999.0f;
    CHECK(canvas_get_line_dash(cv, out, 8) == 4);
    CHECK(fabsf(out[0] - 4.0f) < 1e-6f);

    // Clearing the dash returns to empty.
    canvas_set_line_dash(cv, NULL, 0);
    CHECK(canvas_get_line_dash(cv, out, 8) == 0);

    canvas_free(cv);
    return TEST_REPORT();
}
