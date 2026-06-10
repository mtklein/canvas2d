// Unit tests for the gradient solve and colour evaluation.  The renderer fills
// via the vectorized cnvs_gradient_param_row + cnvs_gradient_color_row pair, so
// the scalar cnvs_gradient_param / cnvs_gradient_sample (used by bench_gradient)
// go untested by the API-level test_gradient.c -- coverage flagged them at
// 18% / 0%.  Drive them directly, pin the vector rows to the scalar evaluators
// (the colour row at tolerance ZERO -- it is bit-identical by design), and gate
// the colour error against an exact double-precision reference
// (docs/decisions/gradient-eval.md).  Includes the internal header, like
// test_mem.c.

#include "test_util.h"

#include "cnvs_gradient.h"
#include "cnvs_math.h"

#include <math.h>
#include <string.h>

static bool fnear(float a, float b, float tol) {
    return fabsf(a - b) <= tol;
}

static bool col_near(cnvs_unpremul a, cnvs_unpremul b, float tol) {
    return fnear((float)a.r, (float)b.r, tol) && fnear((float)a.g, (float)b.g, tol) &&
           fnear((float)a.b, (float)b.b, tol) && fnear((float)a.a, (float)b.a, tol);
}

// Exact piecewise-linear colour in double over the f16 stop colours -- the
// reference the evaluators may deviate from only by the f16 lerp's rounding.
// Mirrors cnvs_gradient_color_at's segment selection; the lerp is exact.
static void ref_color_at(cnvs_gradient const *gr, double t,
                         double *__counted_by(4) out) {
    int n = gr->stop_count;
    if (n == 0) {
        out[0] = out[1] = out[2] = out[3] = 0.0;
        return;
    }
    cnvs_unpremul c = gr->stops[0].color;
    if (t > (double)gr->stops[0].offset) {
        c = gr->stops[n - 1].color;
        if (t < (double)gr->stops[n - 1].offset) {
            for (int i = 0; i + 1 < n; i++) {
                double lo = (double)gr->stops[i].offset;
                double hi = (double)gr->stops[i + 1].offset;
                if (t <= hi) {
                    double u = hi > lo ? (t - lo) / (hi - lo) : 0.0;
                    cnvs_unpremul a = gr->stops[i].color, b = gr->stops[i + 1].color;
                    out[0] = (double)a.r + ((double)b.r - (double)a.r) * u;
                    out[1] = (double)a.g + ((double)b.g - (double)a.g) * u;
                    out[2] = (double)a.b + ((double)b.b - (double)a.b) * u;
                    out[3] = (double)a.a + ((double)b.a - (double)a.a) * u;
                    return;
                }
            }
        }
    }
    out[0] = (double)c.r;
    out[1] = (double)c.g;
    out[2] = (double)c.b;
    out[3] = (double)c.a;
}

// The f16 stop lerp's rounding budget against the double reference: measured
// 0.156/255 worst-case across ~11M-sample sweeps of the gallery's gradients
// (docs/decisions/gradient-eval.md); 0.25/255 leaves margin for stop sets not
// sampled and still sits below the 8-bit rounding step.
#define GRAD_ERR_TOL (0.25 / 255.0)

// The vectorized colour row's two invariants: bit-identical to the scalar
// evaluator for every t >= 0 (tolerance zero -- the row kernel IS
// cnvs_gradient_color_at, eight lanes at a time), transparent black for the
// t < 0 "outside" sentinel, and within GRAD_ERR_TOL of the exact double
// reference.  The sweep brackets [0,1] densely plus every stop offset and its
// f32 neighbours; N % 8 == 5 so the 8-wide body and the scalar tail both run.
static void check_color_row(cnvs_gradient const *gr) {
    enum { N = 1029 };
    static float t[N];
    static cnvs_unpremul row[N];
    int n = 0;
    t[n++] = -1.0f;  // the row solver's "outside" sentinel
    for (int s = 0; s < gr->stop_count && n + 3 <= N; s++) {
        float o = gr->stops[s].offset;
        t[n++] = nextafterf(o, 0.0f);
        t[n++] = o;
        t[n++] = nextafterf(o, 1.0f);
    }
    int base = n;
    for (; n < N; n++) {
        t[n] = (float)(n - base) / (float)(N - 1 - base);  // dense [0,1]
    }
    cnvs_gradient_color_row(gr, t, N, row);
    for (int i = 0; i < N; i++) {
        cnvs_unpremul want = t[i] >= 0.0f
            ? cnvs_gradient_color_at(gr, t[i])
            : cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 0.0f);
        CHECK(memcmp(&row[i], &want, sizeof want) == 0);
        if (t[i] >= 0.0f) {
            double ref[4];
            ref_color_at(gr, (double)t[i], ref);
            CHECK(fabs((double)row[i].r - ref[0]) <= GRAD_ERR_TOL &&
                  fabs((double)row[i].g - ref[1]) <= GRAD_ERR_TOL &&
                  fabs((double)row[i].b - ref[2]) <= GRAD_ERR_TOL &&
                  fabs((double)row[i].a - ref[3]) <= GRAD_ERR_TOL);
        }
    }
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

    // 1. scalar cnvs_gradient_param agrees with the vectorized row -- linear,
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

    // 2. cnvs_gradient_sample(p, alpha) == color_at(param(p)) with alpha folded in.
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

    // 3. add_stop: offsets clamp to [0,1], and it's a no-op once full.
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

    // 4. Degenerate color_at: no stops -> transparent black; a single stop is
    //    constant across the whole parameter range.
    {
        cnvs_gradient g = { .kind = CNVS_GRAD_LINEAR, .p0 = { .x = 0.0f, .y = 0.0f },
                            .p1 = { .x = 1.0f, .y = 0.0f } };
        cnvs_unpremul empty = cnvs_gradient_color_at(&g, 0.5f);
        CHECK(fnear((float)empty.a, 0.0f, 0.0f));
        cnvs_gradient_add_stop(&g, 0.5f, cnvs_unpremul_of(0.2f, 0.4f, 0.6f, 1.0f));
        CHECK(col_near(cnvs_gradient_color_at(&g, 0.0f), cnvs_gradient_color_at(&g, 1.0f), 0.0f));
    }

    // 5. The vectorized colour row (check_color_row above), across the stop
    //    search's edge cases: two stops (no interior search), a multi-stop
    //    ramp, coincident "hard" stops, every stop coincident (the tie
    //    precedence), a single stop, no stops, and a full CNVS_MAX_STOPS set.
    {
        check_color_row(&lin);

        cnvs_gradient multi = { .kind = CNVS_GRAD_LINEAR, .p1 = { .x = 1.0f } };
        cnvs_gradient_add_stop(&multi, 0.00f, cnvs_unpremul_of(0.90f, 0.20f, 0.25f, 1.0f));
        cnvs_gradient_add_stop(&multi, 0.33f, cnvs_unpremul_of(0.95f, 0.80f, 0.25f, 1.0f));
        cnvs_gradient_add_stop(&multi, 0.66f, cnvs_unpremul_of(0.30f, 0.80f, 0.45f, 0.5f));
        cnvs_gradient_add_stop(&multi, 1.00f, cnvs_unpremul_of(0.30f, 0.45f, 0.95f, 0.0f));
        check_color_row(&multi);

        cnvs_gradient hard = { .kind = CNVS_GRAD_LINEAR, .p1 = { .x = 1.0f } };
        cnvs_gradient_add_stop(&hard, 0.0f, cnvs_unpremul_of(0.97f, 0.78f, 0.24f, 1.0f));
        cnvs_gradient_add_stop(&hard, 0.4f, cnvs_unpremul_of(0.97f, 0.78f, 0.24f, 1.0f));
        cnvs_gradient_add_stop(&hard, 0.4f, cnvs_unpremul_of(0.20f, 0.78f, 0.70f, 1.0f));
        cnvs_gradient_add_stop(&hard, 1.0f, cnvs_unpremul_of(0.20f, 0.78f, 0.70f, 1.0f));
        check_color_row(&hard);

        cnvs_gradient ties = { .kind = CNVS_GRAD_LINEAR, .p1 = { .x = 1.0f } };
        cnvs_gradient_add_stop(&ties, 0.5f, cnvs_unpremul_of(1.0f, 0.0f, 0.0f, 1.0f));
        cnvs_gradient_add_stop(&ties, 0.5f, cnvs_unpremul_of(0.0f, 1.0f, 0.0f, 1.0f));
        cnvs_gradient_add_stop(&ties, 0.5f, cnvs_unpremul_of(0.0f, 0.0f, 1.0f, 1.0f));
        check_color_row(&ties);

        cnvs_gradient one = { .kind = CNVS_GRAD_LINEAR, .p1 = { .x = 1.0f } };
        cnvs_gradient_add_stop(&one, 0.5f, cnvs_unpremul_of(0.2f, 0.4f, 0.6f, 0.8f));
        check_color_row(&one);

        cnvs_gradient none = { .kind = CNVS_GRAD_LINEAR, .p1 = { .x = 1.0f } };
        check_color_row(&none);

        cnvs_gradient full = { .kind = CNVS_GRAD_LINEAR, .p1 = { .x = 1.0f } };
        for (int k = 0; k < CNVS_MAX_STOPS; k++) {
            float o = (float)k / (float)(CNVS_MAX_STOPS - 1);
            cnvs_gradient_add_stop(&full, o,
                cnvs_unpremul_of(o, 1.0f - o, 0.5f + 0.4f * sinf(20.0f * o), 1.0f));
        }
        check_color_row(&full);
    }

    return TEST_REPORT();
}
