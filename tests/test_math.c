#include "canvas2d_matrix.h"
#include "test_util.h"

#include <math.h>

static bool near(float x, float y) {
    return fabsf(x - y) < 1e-5f;
}

static bool vnear(canvas2d_vec2 p, float x, float y) {
    return near(p.x, x) && near(p.y, y);
}

int main(void) {
    canvas2d_matrix const id = canvas2d_matrix_identity();
    CHECK(vnear(canvas2d_matrix_apply(id, (canvas2d_vec2){ .x = 3.0f, .y = 4.0f }), 3.0f, 4.0f));

    canvas2d_matrix const t = canvas2d_matrix_translate(10.0f, -5.0f);
    CHECK(vnear(canvas2d_matrix_apply(t, (canvas2d_vec2){ .x = 1.0f, .y = 1.0f }), 11.0f, -4.0f));

    canvas2d_matrix const s = canvas2d_matrix_scale(2.0f, 3.0f);
    CHECK(vnear(canvas2d_matrix_apply(s, (canvas2d_vec2){ .x = 4.0f, .y = 4.0f }), 8.0f, 12.0f));

    // Composition order: mul(m, n) applies n first, then m -- like Canvas
    // translate() followed by scale().
    canvas2d_matrix const ts = canvas2d_matrix_mul(t, s);
    CHECK(vnear(canvas2d_matrix_apply(ts, (canvas2d_vec2){ .x = 1.0f, .y = 1.0f }), 12.0f, -2.0f));

    // Rotate +90 degrees: (1,0) -> (0,1) in a +y-down frame.
    canvas2d_matrix const r = canvas2d_matrix_rotate((float)M_PI / 2.0f);
    CHECK(vnear(canvas2d_matrix_apply(r, (canvas2d_vec2){ .x = 1.0f, .y = 0.0f }), 0.0f, 1.0f));

    return TEST_REPORT();
}
