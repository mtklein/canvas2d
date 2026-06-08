#include "cnvs_math.h"
#include "test_util.h"

#include <math.h>

static bool near(float x, float y) {
    return fabsf(x - y) < 1e-5f;
}

static bool vnear(cnvs_vec2 p, float x, float y) {
    return near(p.x, x) && near(p.y, y);
}

int main(void) {
    cnvs_mat id = cnvs_mat_identity();
    CHECK(vnear(cnvs_mat_apply(id, (cnvs_vec2){ .x = 3.0f, .y = 4.0f }), 3.0f, 4.0f));

    cnvs_mat t = cnvs_mat_translate(10.0f, -5.0f);
    CHECK(vnear(cnvs_mat_apply(t, (cnvs_vec2){ .x = 1.0f, .y = 1.0f }), 11.0f, -4.0f));

    cnvs_mat s = cnvs_mat_scale(2.0f, 3.0f);
    CHECK(vnear(cnvs_mat_apply(s, (cnvs_vec2){ .x = 4.0f, .y = 4.0f }), 8.0f, 12.0f));

    // Composition order: mul(m, n) applies n first, then m -- like Canvas
    // translate() followed by scale().
    cnvs_mat ts = cnvs_mat_mul(t, s);
    CHECK(vnear(cnvs_mat_apply(ts, (cnvs_vec2){ .x = 1.0f, .y = 1.0f }), 12.0f, -2.0f));

    // Rotate +90 degrees: (1,0) -> (0,1) in a +y-down frame.
    cnvs_mat r = cnvs_mat_rotate((float)M_PI / 2.0f);
    CHECK(vnear(cnvs_mat_apply(r, (cnvs_vec2){ .x = 1.0f, .y = 0.0f }), 0.0f, 1.0f));

    return TEST_REPORT();
}
