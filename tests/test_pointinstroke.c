#include "canvas.h"
#include "test_util.h"

#include <math.h>

int main(void) {
    struct canvas *__single cv = canvas_create(80, 80);
    CHECK(cv != NULL);
    if (!cv) {
        return TEST_REPORT();
    }

    // A width-8 butt-capped horizontal line from (10,20) to (40,20): the stroke
    // covers x in [10,40], y in [16,24].
    canvas_set_line_width(cv, 8.0f);
    canvas_set_line_cap(cv, CANVAS_CAP_BUTT);
    canvas_begin_path(cv);
    canvas_move_to(cv, 10.0f, 20.0f);
    canvas_line_to(cv, 40.0f, 20.0f);
    CHECK(canvas_is_point_in_stroke(cv, 25.0f, 20.0f));    // on the line
    CHECK(canvas_is_point_in_stroke(cv, 25.0f, 23.0f));    // within half-width
    CHECK(!canvas_is_point_in_stroke(cv, 25.0f, 30.0f));   // below the stroke
    CHECK(!canvas_is_point_in_stroke(cv, 25.0f, 10.0f));   // above the stroke
    CHECK(!canvas_is_point_in_stroke(cv, 5.0f, 20.0f));    // before the butt cap

    // Non-finite coordinates are never in the stroke.
    CHECK(!canvas_is_point_in_stroke(cv, (float)INFINITY, 20.0f));

    // A stroked rectangle outline: the border is in the stroke, the interior and
    // the far exterior are not.
    canvas_set_line_width(cv, 4.0f);
    canvas_begin_path(cv);
    canvas_rect(cv, 10.0f, 10.0f, 40.0f, 40.0f);
    CHECK(canvas_is_point_in_stroke(cv, 10.0f, 30.0f));    // on the left edge
    CHECK(canvas_is_point_in_stroke(cv, 30.0f, 10.0f));    // on the top edge
    CHECK(!canvas_is_point_in_stroke(cv, 30.0f, 30.0f));   // interior: not stroked
    CHECK(!canvas_is_point_in_stroke(cv, 30.0f, 2.0f));    // well outside

    // An empty path strokes nothing.
    canvas_begin_path(cv);
    CHECK(!canvas_is_point_in_stroke(cv, 25.0f, 20.0f));

    // The query point is transformed by the current transform (and the line width
    // by its scale), as for is_point_in_path.
    canvas_reset_transform(cv);
    canvas_translate(cv, 10.0f, 10.0f);
    canvas_set_line_width(cv, 8.0f);
    canvas_begin_path(cv);
    canvas_move_to(cv, 0.0f, 10.0f);
    canvas_line_to(cv, 20.0f, 10.0f);
    CHECK(canvas_is_point_in_stroke(cv, 10.0f, 10.0f));    // -> device (20,20), on line
    CHECK(!canvas_is_point_in_stroke(cv, 10.0f, 30.0f));   // -> device (20,40), far off

    canvas_destroy(cv);
    return TEST_REPORT();
}
