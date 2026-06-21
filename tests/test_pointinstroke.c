#include "canvas2d.h"
#include "test_util.h"

#include <math.h>

int main(void) {
    struct canvas2d_context *__single cv = canvas2d(80, 80, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return TEST_REPORT();
    }

    // A width-8 butt-capped horizontal line from (10,20) to (40,20): the stroke
    // covers x in [10,40], y in [16,24].
    canvas2d_set_line_width(cv, 8.0f);
    canvas2d_set_line_cap(cv, CANVAS2D_CAP_BUTT);
    canvas2d_begin_path(cv);
    canvas2d_move_to(cv, 10.0f, 20.0f);
    canvas2d_line_to(cv, 40.0f, 20.0f);
    CHECK(canvas2d_is_point_in_stroke(cv, 25.0f, 20.0f));    // on the line
    CHECK(canvas2d_is_point_in_stroke(cv, 25.0f, 23.0f));    // within half-width
    CHECK(!canvas2d_is_point_in_stroke(cv, 25.0f, 30.0f));   // below the stroke
    CHECK(!canvas2d_is_point_in_stroke(cv, 25.0f, 10.0f));   // above the stroke
    CHECK(!canvas2d_is_point_in_stroke(cv, 5.0f, 20.0f));    // before the butt cap

    // Non-finite coordinates are never in the stroke.
    CHECK(!canvas2d_is_point_in_stroke(cv, (float)INFINITY, 20.0f));

    // A stroked rectangle outline: the border is in the stroke, the interior and
    // the far exterior are not.
    canvas2d_set_line_width(cv, 4.0f);
    canvas2d_begin_path(cv);
    canvas2d_rect(cv, 10.0f, 10.0f, 40.0f, 40.0f);
    CHECK(canvas2d_is_point_in_stroke(cv, 10.0f, 30.0f));    // on the left edge
    CHECK(canvas2d_is_point_in_stroke(cv, 30.0f, 10.0f));    // on the top edge
    CHECK(!canvas2d_is_point_in_stroke(cv, 30.0f, 30.0f));   // interior: not stroked
    CHECK(!canvas2d_is_point_in_stroke(cv, 30.0f, 2.0f));    // well outside

    // An empty path strokes nothing.
    canvas2d_begin_path(cv);
    CHECK(!canvas2d_is_point_in_stroke(cv, 25.0f, 20.0f));

    // The query point is transformed by the current transform (and the line width
    // by its scale), as for is_point_in_path.
    canvas2d_reset_transform(cv);
    canvas2d_translate(cv, 10.0f, 10.0f);
    canvas2d_set_line_width(cv, 8.0f);
    canvas2d_begin_path(cv);
    canvas2d_move_to(cv, 0.0f, 10.0f);
    canvas2d_line_to(cv, 20.0f, 10.0f);
    CHECK(canvas2d_is_point_in_stroke(cv, 10.0f, 10.0f));    // -> device (20,20), on line
    CHECK(!canvas2d_is_point_in_stroke(cv, 10.0f, 30.0f));   // -> device (20,40), far off

    canvas2d_free(cv);
    return TEST_REPORT();
}
