// Isolated benchmark: the gradient *fill* path as paint_tile runs it -- solve the
// parameter a row at a time (vectorized), then lerp the stop colours from it
// (vectorized too; no ramp table).  Mirrors bench_gradient's scene (same
// kind/size/iters) so the two are directly comparable: the delta is what the two
// row kernels buy over the naive per-pixel scan.
#include "bench_reps.h"

#include "cnvs_gradient.h"

#include <stdio.h>

#define ITERS 40
#define DIM 384

int main(void) {
    struct cnvs_gradient gr = {
        .kind = CNVS_GRAD_RADIAL,
        .p0 = { .x = (float)DIM * 0.4f, .y = (float)DIM * 0.45f },
        .p1 = { .x = (float)DIM * 0.5f, .y = (float)DIM * 0.5f },
        .r0 = 4.0f,
        .r1 = (float)DIM * 0.6f,
        .stop_count = 0,
    };
    cnvs_gradient_add_stop(&gr, 0.00f, cnvs_unpremul_of(1.0f, 1.0f, 1.0f, 1.0f));
    cnvs_gradient_add_stop(&gr, 0.35f, cnvs_unpremul_of(0.3f, 0.6f, 0.95f, 1.0f));
    cnvs_gradient_add_stop(&gr, 0.70f, cnvs_unpremul_of(0.9f, 0.3f, 0.4f, 1.0f));
    cnvs_gradient_add_stop(&gr, 1.00f, cnvs_unpremul_of(0.05f, 0.1f, 0.3f, 1.0f));

    static float trow[DIM];
    static cnvs_unpremul crow[DIM];
    double sink = 0.0;
    int reps = bench_reps();
    for (int rep = 0; rep < reps; rep++) {
        for (int it = 0; it < ITERS; it++) {
            for (int y = 0; y < DIM; y++) {
                cnvs_gradient_param_row(&gr, 0, (float)y + 0.5f, DIM, trow);
                cnvs_gradient_color_row(&gr, trow, DIM, crow);
                for (int x = 0; x < DIM; x++) {
                    cnvs_unpremul c = crow[x];  // outside is transparent black
                    sink += (double)c.r + (double)c.g + (double)c.b + (double)c.a;
                }
            }
        }
    }
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
