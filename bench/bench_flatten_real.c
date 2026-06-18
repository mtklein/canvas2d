// Isolated benchmark: adaptive de Casteljau flattening of representative curves
// -- circle-arc quadrants (roundRect / ellipse / arc corners) and gentle
// glyph-stroke-like cubics, at realistic sizes, which flatten to a handful of
// segments. The median twin of bench_flatten, which uses random control points
// across the canvas (deep subdivision, worst case).
#include "bench_reps.h"
#include "bench_util.h"

#include "cnvs_path.h"

#include <math.h>
#include <stdio.h>

#define ITERS 4000
#define CURVES 100

// A quarter-circle as a cubic (kappa = 4/3*(sqrt(2)-1)): centre (cx,cy),
// radius r, start angle a0 sweeping +90 degrees.
static void arc_quadrant(struct cnvs_path *p, float cx, float cy, float r, float a0) {
    float const k = 0.5522847498f * r;
    float const c0 = cosf(a0), s0 = sinf(a0);
    float const a1 = a0 + (float)M_PI_2, c1 = cosf(a1), s1 = sinf(a1);
    cnvs_vec2 const p0 = { cx + r * c0, cy + r * s0 };
    cnvs_vec2 const p3 = { cx + r * c1, cy + r * s1 };
    cnvs_vec2 const cp1 = { p0.x - k * s0, p0.y + k * c0 };
    cnvs_vec2 const cp2 = { p3.x + k * s1, p3.y - k * c1 };
    cnvs_path_move_to(p, p0);
    cnvs_path_cubic_to(p, cp1, cp2, p3, 0.25f);
}

// A shallow cubic over a chord a->b, control points bowed perpendicular by a
// small fraction of the chord -- the gentle curvature of a glyph stroke.
static void gentle_cubic(struct cnvs_path *p, cnvs_vec2 a, cnvs_vec2 b, float bow) {
    float const dx = b.x - a.x, dy = b.y - a.y;
    cnvs_vec2 const c1 = { a.x + dx / 3.0f - dy * bow, a.y + dy / 3.0f + dx * bow };
    cnvs_vec2 const c2 = { a.x + 2.0f * dx / 3.0f - dy * bow,
                           a.y + 2.0f * dy / 3.0f + dx * bow };
    cnvs_path_move_to(p, a);
    cnvs_path_cubic_to(p, c1, c2, b, 0.25f);
}

int main(void) {
    struct cnvs_path path;
    cnvs_path_init(&path);
    double sink = 0.0;

    int const reps = bench_reps();
    for (int rep = 0; rep < reps; rep++) {
        for (int it = 0; it < ITERS; it++) {
            cnvs_path_reset(&path);
            for (int k = 0; k < CURVES; k++) {
                if (k % 2 == 0) {  // roundRect/ellipse-scale arc, radius 4..132px
                    float const r = 4.0f + bench_frand() * 128.0f;
                    arc_quadrant(&path, bench_frand() * 512.0f, bench_frand() * 512.0f,
                                 r, bench_frand() * 6.2831853f);
                } else {  // glyph-stroke-scale gentle cubic, chord 8..72px
                    cnvs_vec2 const a = bench_rpt(512.0f, 512.0f);
                    float const ang = bench_frand() * 6.2831853f;
                    float const len = 8.0f + bench_frand() * 64.0f;
                    cnvs_vec2 const b = { a.x + len * cosf(ang), a.y + len * sinf(ang) };
                    gentle_cubic(&path, a, b, 0.05f + bench_frand() * 0.15f);
                }
            }
            sink += (double)path.npts;
        }
    }

    cnvs_path_free(&path);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
