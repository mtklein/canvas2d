#include "canvas.h"
#include "test_util.h"

#include <math.h>

static bool mat_near(canvas_matrix m, float a, float b, float c, float d,
                     float e, float f) {
    float const tol = 1e-5f;
    return fabsf(m.a - a) < tol && fabsf(m.b - b) < tol &&
           fabsf(m.c - c) < tol && fabsf(m.d - d) < tol &&
           fabsf(m.e - e) < tol && fabsf(m.f - f) < tol;
}

int main(void) {
    struct canvas *__single cv = canvas_create(16, 16);
    CHECK(cv != NULL);
    if (!cv) {
        return TEST_REPORT();
    }

    // A fresh canvas starts at the identity transform.
    CHECK(mat_near(canvas_get_transform(cv), 1, 0, 0, 1, 0, 0));

    // translate populates the (e,f) column.
    canvas_translate(cv, 10.0f, 20.0f);
    CHECK(mat_near(canvas_get_transform(cv), 1, 0, 0, 1, 10, 20));

    // scale multiplies into (a,d), composing with the existing translate.
    canvas_scale(cv, 2.0f, 3.0f);
    CHECK(mat_near(canvas_get_transform(cv), 2, 0, 0, 3, 10, 20));

    // set_transform replaces the matrix wholesale.
    canvas_set_transform(cv, 2.0f, 1.0f, -1.0f, 2.0f, 5.0f, 7.0f);
    CHECK(mat_near(canvas_get_transform(cv), 2, 1, -1, 2, 5, 7));

    // reset_transform returns to identity.
    canvas_reset_transform(cv);
    CHECK(mat_near(canvas_get_transform(cv), 1, 0, 0, 1, 0, 0));

    // save/restore brackets the transform.
    canvas_set_transform(cv, 1.0f, 0.0f, 0.0f, 1.0f, 4.0f, 8.0f);
    canvas_save(cv);
    canvas_rotate(cv, 1.0f);
    canvas_restore(cv);
    CHECK(mat_near(canvas_get_transform(cv), 1, 0, 0, 1, 4, 8));

    canvas_destroy(cv);
    return TEST_REPORT();
}
