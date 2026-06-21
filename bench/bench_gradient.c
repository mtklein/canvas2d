// The deliberate naive comparator: per-pixel SCALAR gradient evaluation
// (radial parameter solve + stop scan) across a grid, one canvas2d_gradient_sample
// call per pixel.  The renderer never takes this path -- paint_tile runs the
// 8-wide row kernels (bench_gradient_fill) -- but keeping the scalar walk
// benchmarked is what lets the README quote the vectorized path's speedup as a
// measured ratio rather than folklore, and it exercises the scalar evaluators
// the row kernels are pinned against (test_gradient_solve).
#include "bench_reps.h"

#include "canvas2d_gradient.h"

#include <stdio.h>

#define ITERS 40
#define DIM 384

int main(void) {
    struct canvas2d_gradient gr = {
        .kind = CANVAS2D_GRAD_RADIAL,
        .p0 = { .x = (float)DIM * 0.4f, .y = (float)DIM * 0.45f },
        .p1 = { .x = (float)DIM * 0.5f, .y = (float)DIM * 0.5f },
        .r0 = 4.0f,
        .r1 = (float)DIM * 0.6f,
        .stop_count = 0,
    };
    canvas2d_gradient_add_stop(&gr, 0.00f, canvas2d_unpremul_of(1.0f, 1.0f, 1.0f, 1.0f));
    canvas2d_gradient_add_stop(&gr, 0.35f, canvas2d_unpremul_of(0.3f, 0.6f, 0.95f, 1.0f));
    canvas2d_gradient_add_stop(&gr, 0.70f, canvas2d_unpremul_of(0.9f, 0.3f, 0.4f, 1.0f));
    canvas2d_gradient_add_stop(&gr, 1.00f, canvas2d_unpremul_of(0.05f, 0.1f, 0.3f, 1.0f));

    double sink = 0.0;
    int const reps = bench_reps();
    for (int rep = 0; rep < reps; rep++) {
        for (int it = 0; it < ITERS; it++) {
            for (int y = 0; y < DIM; y++) {
                for (int x = 0; x < DIM; x++) {
                    canvas2d_unpremul c = canvas2d_gradient_sample(
                        &gr, (canvas2d_vec2){ .x = (float)x + 0.5f, .y = (float)y + 0.5f }, 1.0f);
                    sink += (double)c.r + (double)c.g + (double)c.b + (double)c.a;
                }
            }
        }
    }
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
