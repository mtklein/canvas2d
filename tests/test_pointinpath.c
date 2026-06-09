#include "canvas.h"
#include "test_util.h"

#include <math.h>

int main(void) {
    canvas *__single cv = canvas_create(32, 32);
    CHECK(cv != NULL);
    if (!cv) {
        return TEST_REPORT();
    }

    // A simple square [4,24]x[4,24].
    canvas_begin_path(cv);
    canvas_rect(cv, 4.0f, 4.0f, 20.0f, 20.0f);
    CHECK(canvas_is_point_in_path(cv, 12.0f, 12.0f, CANVAS_NONZERO));   // centre
    CHECK(!canvas_is_point_in_path(cv, 1.0f, 1.0f, CANVAS_NONZERO));    // outside
    CHECK(!canvas_is_point_in_path(cv, 30.0f, 12.0f, CANVAS_NONZERO));  // right of it

    // Non-finite coordinates are never inside.
    CHECK(!canvas_is_point_in_path(cv, (float)INFINITY, 12.0f, CANVAS_NONZERO));

    // An empty path contains nothing.
    canvas_begin_path(cv);
    CHECK(!canvas_is_point_in_path(cv, 12.0f, 12.0f, CANVAS_NONZERO));

    // Two concentric same-wound squares: the inner overlap has winding 2 (nonzero
    // inside) but two crossings (even-odd outside) -- the rules disagree there.
    canvas_begin_path(cv);
    canvas_rect(cv, 2.0f, 2.0f, 28.0f, 28.0f);   // outer [2,30]
    canvas_rect(cv, 10.0f, 10.0f, 12.0f, 12.0f);  // inner [10,22]
    CHECK(canvas_is_point_in_path(cv, 16.0f, 16.0f, CANVAS_NONZERO));    // inner: nonzero in
    CHECK(!canvas_is_point_in_path(cv, 16.0f, 16.0f, CANVAS_EVENODD));   // inner: even-odd out
    CHECK(canvas_is_point_in_path(cv, 5.0f, 16.0f, CANVAS_NONZERO));     // ring: both in
    CHECK(canvas_is_point_in_path(cv, 5.0f, 16.0f, CANVAS_EVENODD));
    CHECK(!canvas_is_point_in_path(cv, 1.0f, 1.0f, CANVAS_EVENODD));     // outside both

    // The query point is transformed by the current transform, like the path.
    canvas_reset_transform(cv);
    canvas_translate(cv, 10.0f, 10.0f);
    canvas_begin_path(cv);
    canvas_rect(cv, 0.0f, 0.0f, 10.0f, 10.0f);   // device [10,20]
    CHECK(canvas_is_point_in_path(cv, 5.0f, 5.0f, CANVAS_NONZERO));   // -> device (15,15) in
    CHECK(!canvas_is_point_in_path(cv, -5.0f, -5.0f, CANVAS_NONZERO)); // -> device (5,5) out

    canvas_destroy(cv);
    return TEST_REPORT();
}
