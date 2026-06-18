// Non-finite and huge float inputs must not cause undefined behaviour.  The
// public API takes floats freely; before the fix, the float->int device-space
// casts (points_bbox, the coverage rasterizer, ellipse/stroke segment counts)
// and the float->uint8 colour quantization were UB on NaN/Inf/out-of-range
// values.  Under the debug variant's UBSan this test aborts if any regresses.
// See docs/decisions/security-review.md, Finding 4.

#include "test_util.h"

#include "canvas.h"

#include <math.h>
#include <ptrcheck.h>

int main(void) {
    struct canvas *__single cv = canvas(64, 64);
    CHECK(cv != NULL);

    // The reported crash value (finite, > INT_MAX) plus NaN/Inf and other huge
    // finite magnitudes -- each must flow through the casts without UB.
    float const bad[] = { 1.99544e12f, -3.0e30f, 9.0e37f, INFINITY, -INFINITY, NAN };
    int const nbad = (int)(sizeof bad / sizeof bad[0]);

    for (int i = 0; i < nbad; i++) {
        float const v = bad[i];
        canvas_clear_rect(cv, v, v, v, v);                 // points_bbox (int) cast

        canvas_begin_path(cv);
        canvas_move_to(cv, v, 0.0f);
        canvas_line_to(cv, 0.0f, v);
        canvas_line_to(cv, v, v);
        canvas_fill(cv, CANVAS_NONZERO);                                   // coverage rasterizer casts

        canvas_begin_path(cv);
        canvas_ellipse(cv, 0.0f, 0.0f, 10.0f, 10.0f, 0.0f, 0.0f, v, false);
        canvas_fill(cv, CANVAS_NONZERO);                                   // ellipse segment-count cast

        canvas_set_line_width(cv, v);
        canvas_begin_path(cv);
        canvas_move_to(cv, 0.0f, 0.0f);
        canvas_line_to(cv, 20.0f, 20.0f);
        canvas_stroke(cv);                                 // stroke segment-count cast

        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, v, v, v, v);              // colour clamp -> [0,1]
        canvas_set_global_alpha(cv, v);
    }

    // Finding 5: a sub-pixel dash over a long path must not spin or blow up the
    // vertex buffer (a ~2GB alloc was reachable).  The span cap bounds it, so
    // this returns promptly instead of hanging/OOMing.
    canvas_set_line_width(cv, 2.0f);
    float const tiny_dash[] = { 1.0e-4f, 1.0e-4f };
    canvas_set_line_dash(cv, tiny_dash, 2);
    canvas_begin_path(cv);
    canvas_move_to(cv, 0.0f, 0.0f);
    canvas_line_to(cv, 1.0e6f, 1.0e6f);
    canvas_stroke(cv);
    canvas_set_line_dash(cv, NULL, 0);  // back to solid

    // After all that abuse a normal draw + readback still works, and the
    // float->uint8 quantization in read-back stays in range.
    canvas_set_global_alpha(cv, 1.0f);
    canvas_set_line_width(cv, 1.0f);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.5f, 0.5f, 0.5f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, 64.0f, 64.0f);

    uint8_t px[64 * 64 * 4];
    canvas_get_image_data(cv, 0, 0, 64, 64, px, (int)sizeof px);
    CHECK(px[0] == 128 || px[0] == 127);  // mid-grey was painted

    canvas_free(cv);
    return TEST_REPORT();
}
