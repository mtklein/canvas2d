#include "canvas2d.h"
#include "test_util.h"

#include <math.h>

int main(void) {
    struct canvas2d_context *__single cv = canvas2d(32, 32, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return TEST_REPORT();
    }

    // A simple square [4,24]x[4,24].
    canvas2d_begin_path(cv);
    canvas2d_rect(cv, 4.0f, 4.0f, 20.0f, 20.0f);
    CHECK(canvas2d_is_point_in_path(cv, 12.0f, 12.0f, CANVAS2D_NONZERO));   // centre
    CHECK(!canvas2d_is_point_in_path(cv, 1.0f, 1.0f, CANVAS2D_NONZERO));    // outside
    CHECK(!canvas2d_is_point_in_path(cv, 30.0f, 12.0f, CANVAS2D_NONZERO));  // right of it

    // Non-finite coordinates are never inside.
    CHECK(!canvas2d_is_point_in_path(cv, (float)INFINITY, 12.0f, CANVAS2D_NONZERO));

    // An empty path contains nothing.
    canvas2d_begin_path(cv);
    CHECK(!canvas2d_is_point_in_path(cv, 12.0f, 12.0f, CANVAS2D_NONZERO));

    // Two concentric same-wound squares: the inner overlap has winding 2 (nonzero
    // inside) but two crossings (even-odd outside) -- the rules disagree there.
    canvas2d_begin_path(cv);
    canvas2d_rect(cv, 2.0f, 2.0f, 28.0f, 28.0f);   // outer [2,30]
    canvas2d_rect(cv, 10.0f, 10.0f, 12.0f, 12.0f);  // inner [10,22]
    CHECK(canvas2d_is_point_in_path(cv, 16.0f, 16.0f, CANVAS2D_NONZERO));    // inner: nonzero in
    CHECK(!canvas2d_is_point_in_path(cv, 16.0f, 16.0f, CANVAS2D_EVENODD));   // inner: even-odd out
    CHECK(canvas2d_is_point_in_path(cv, 5.0f, 16.0f, CANVAS2D_NONZERO));     // ring: both in
    CHECK(canvas2d_is_point_in_path(cv, 5.0f, 16.0f, CANVAS2D_EVENODD));
    CHECK(!canvas2d_is_point_in_path(cv, 1.0f, 1.0f, CANVAS2D_EVENODD));     // outside both

    // The query point is transformed by the current transform, like the path.
    canvas2d_reset_transform(cv);
    canvas2d_translate(cv, 10.0f, 10.0f);
    canvas2d_begin_path(cv);
    canvas2d_rect(cv, 0.0f, 0.0f, 10.0f, 10.0f);   // device [10,20]
    CHECK(canvas2d_is_point_in_path(cv, 5.0f, 5.0f, CANVAS2D_NONZERO));   // -> device (15,15) in
    CHECK(!canvas2d_is_point_in_path(cv, -5.0f, -5.0f, CANVAS2D_NONZERO)); // -> device (5,5) out

    canvas2d_free(cv);
    return TEST_REPORT();
}
