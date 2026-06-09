// Unit tests for the scalar gradient solve.  The renderer fills via the
// vectorized cnvs_gradient_param_row, so the scalar cnvs_gradient_param /
// cnvs_gradient_sample (used by bench_gradient) go untested by the API-level
// test_gradient.c -- coverage flagged them at 18% / 0%.  Drive them directly, and
// pin them to the vector path and the documented ramp identity as metamorphic
// cross-checks.  Includes the internal header, like test_mem.c.

#include "test_util.h"

#include "cnvs_gradient.h"
#include "cnvs_math.h"

#include <math.h>

static bool fnear(float a, float b, float tol) {
    return fabsf(a - b) <= tol;
}

static bool col_near(cnvs_unpremul a, cnvs_unpremul b, float tol) {
    return fnear((float)a.r, (float)b.r, tol) && fnear((float)a.g, (float)b.g, tol) &&
           fnear((float)a.b, (float)b.b, tol) && fnear((float)a.a, (float)b.a, tol);
}

// Compare the scalar param against the vectorized row, pixel by pixel: equal t
// where both have one (within float-solve tolerance), and "outside" agreeing.
static void check_param_matches_row(cnvs_gradient const *gr, int x0, float y, int n) {
    float row[64];
    if (n > 64) { n = 64; }
    cnvs_gradient_param_row(gr, x0, y, n, row);
    for (int i = 0; i < n; i++) {
        float t = -2.0f;
        bool ok = cnvs_gradient_param(gr, (cnvs_vec2){ .x = (float)(x0 + i) + 0.5f, .y = y }, &t);
        if (row[i] < 0.0f) {
            CHECK(!ok);
        } else {
            CHECK(ok && fnear(t, row[i], 1e-3f));
        }
    }
}

int main(void) {
    // Linear gradient, red -> blue along x in [0,64].
    cnvs_gradient lin = { .kind = CNVS_GRAD_LINEAR, .p0 = { .x = 0.0f, .y = 0.0f },
                          .p1 = { .x = 64.0f, .y = 0.0f } };
    cnvs_gradient_add_stop(&lin, 0.0f, cnvs_unpremul_of(1.0f, 0.0f, 0.0f, 1.0f));
    cnvs_gradient_add_stop(&lin, 1.0f, cnvs_unpremul_of(0.0f, 0.0f, 1.0f, 1.0f));

    // 1. build_ramp[i] == color_at(i/(N-1)) exactly (the documented identity).
    static cnvs_unpremul ramp[CNVS_GRAD_RAMP_N];
    cnvs_gradient_build_ramp(&lin, ramp, CNVS_GRAD_RAMP_N);
    for (int i = 0; i < CNVS_GRAD_RAMP_N; i += 17) {
        cnvs_unpremul c = cnvs_gradient_color_at(&lin, (float)i / (float)(CNVS_GRAD_RAMP_N - 1));
        CHECK(col_near(ramp[i], c, 0.0f));
    }

    // 2. scalar cnvs_gradient_param agrees with the vectorized row -- linear,
    //    concentric radial, and an eccentric/focal radial (which has "outside"
    //    points, exercising the no-solution branch).
    check_param_matches_row(&lin, 0, 8.0f, 64);

    cnvs_gradient rad = { .kind = CNVS_GRAD_RADIAL, .p0 = { .x = 32.0f, .y = 32.0f },
                          .p1 = { .x = 32.0f, .y = 32.0f }, .r0 = 0.0f, .r1 = 28.0f };
    cnvs_gradient_add_stop(&rad, 0.0f, cnvs_unpremul_of(1.0f, 1.0f, 0.0f, 1.0f));
    cnvs_gradient_add_stop(&rad, 1.0f, cnvs_unpremul_of(1.0f, 0.0f, 0.0f, 1.0f));
    check_param_matches_row(&rad, 0, 32.0f, 64);

    cnvs_gradient focal = { .kind = CNVS_GRAD_RADIAL, .p0 = { .x = 20.0f, .y = 32.0f },
                            .p1 = { .x = 44.0f, .y = 32.0f }, .r0 = 3.0f, .r1 = 18.0f };
    cnvs_gradient_add_stop(&focal, 0.0f, cnvs_unpremul_of(1.0f, 1.0f, 1.0f, 1.0f));
    cnvs_gradient_add_stop(&focal, 1.0f, cnvs_unpremul_of(0.0f, 0.0f, 0.2f, 1.0f));
    check_param_matches_row(&focal, 0, 10.0f, 64);
    check_param_matches_row(&focal, 0, 32.0f, 64);

    // 3. cnvs_gradient_sample(p, alpha) == color_at(param(p)) with alpha folded in.
    {
        cnvs_vec2 p = { .x = 18.5f, .y = 8.0f };
        float t = -2.0f;
        CHECK(cnvs_gradient_param(&lin, p, &t));
        cnvs_unpremul want = cnvs_gradient_color_at(&lin, t);
        cnvs_unpremul got = cnvs_gradient_sample(&lin, p, 0.5f);
        CHECK(fnear((float)got.r, (float)want.r, 2e-3f) &&
              fnear((float)got.g, (float)want.g, 2e-3f) &&
              fnear((float)got.b, (float)want.b, 2e-3f) &&
              fnear((float)got.a, (float)want.a * 0.5f, 2e-3f));
    }

    // 4. add_stop: offsets clamp to [0,1], and it's a no-op once full.
    {
        cnvs_gradient g = { .kind = CNVS_GRAD_LINEAR, .p0 = { .x = 0.0f, .y = 0.0f },
                            .p1 = { .x = 1.0f, .y = 0.0f } };
        cnvs_gradient_add_stop(&g, 2.0f, cnvs_unpremul_of(1.0f, 1.0f, 1.0f, 1.0f));   // -> 1
        cnvs_gradient_add_stop(&g, -1.0f, cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 1.0f));  // -> 0
        CHECK(g.stop_count == 2);
        CHECK(fnear(g.stops[0].offset, 0.0f, 0.0f) && fnear(g.stops[1].offset, 1.0f, 0.0f));
        for (int i = 0; i < CNVS_MAX_STOPS + 4; i++) {
            cnvs_gradient_add_stop(&g, 0.5f, cnvs_unpremul_of(0.5f, 0.5f, 0.5f, 1.0f));
        }
        CHECK(g.stop_count == CNVS_MAX_STOPS);  // full -> no-op
    }

    // 5. Degenerate color_at: no stops -> transparent black; a single stop is
    //    constant across the whole parameter range.
    {
        cnvs_gradient g = { .kind = CNVS_GRAD_LINEAR, .p0 = { .x = 0.0f, .y = 0.0f },
                            .p1 = { .x = 1.0f, .y = 0.0f } };
        cnvs_unpremul empty = cnvs_gradient_color_at(&g, 0.5f);
        CHECK(fnear((float)empty.a, 0.0f, 0.0f));
        cnvs_gradient_add_stop(&g, 0.5f, cnvs_unpremul_of(0.2f, 0.4f, 0.6f, 1.0f));
        CHECK(col_near(cnvs_gradient_color_at(&g, 0.0f), cnvs_gradient_color_at(&g, 1.0f), 0.0f));
    }

    return TEST_REPORT();
}
