#include "canvas2d.h"
#include "test_util.h"

#include <math.h>

static bool mat_near(canvas2d_matrix m, float a, float b, float c, float d,
                     float e, float f) {
    float const tol = 1e-5f;
    return fabsf(m.a - a) < tol && fabsf(m.b - b) < tol &&
           fabsf(m.c - c) < tol && fabsf(m.d - d) < tol &&
           fabsf(m.e - e) < tol && fabsf(m.f - f) < tol;
}

int main(void) {
    struct canvas2d_context *__single cv = canvas2d(16, 16, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return TEST_REPORT();
    }

    // A fresh canvas starts at the identity transform.
    CHECK(mat_near(canvas2d_get_transform(cv), 1, 0, 0, 1, 0, 0));

    // translate populates the (e,f) column.
    canvas2d_translate(cv, 10.0f, 20.0f);
    CHECK(mat_near(canvas2d_get_transform(cv), 1, 0, 0, 1, 10, 20));

    // scale multiplies into (a,d), composing with the existing translate.
    canvas2d_scale(cv, 2.0f, 3.0f);
    CHECK(mat_near(canvas2d_get_transform(cv), 2, 0, 0, 3, 10, 20));

    // set_transform replaces the matrix wholesale.
    canvas2d_set_transform(cv, 2.0f, 1.0f, -1.0f, 2.0f, 5.0f, 7.0f);
    CHECK(mat_near(canvas2d_get_transform(cv), 2, 1, -1, 2, 5, 7));

    // reset_transform returns to identity.
    canvas2d_reset_transform(cv);
    CHECK(mat_near(canvas2d_get_transform(cv), 1, 0, 0, 1, 0, 0));

    // save/restore brackets the transform.
    canvas2d_set_transform(cv, 1.0f, 0.0f, 0.0f, 1.0f, 4.0f, 8.0f);
    canvas2d_save(cv);
    canvas2d_rotate(cv, 1.0f);
    canvas2d_restore(cv);
    CHECK(mat_near(canvas2d_get_transform(cv), 1, 0, 0, 1, 4, 8));

    canvas2d_free(cv);
    return TEST_REPORT();
}
