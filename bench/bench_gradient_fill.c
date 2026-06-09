// Isolated benchmark: the gradient *fill* path as paint_tile runs it -- build the
// colour ramp once per fill, then per pixel solve the parameter and index the ramp.
// Mirrors bench_gradient's scene (same kind/size/iters) so the two are directly
// comparable: the delta is what the precomputed ramp buys over a per-pixel stop scan.
#include "bench_reps.h"

#include "cnvs_gradient.h"

#include <stdio.h>

#define ITERS 40
#define DIM 384

int main(void) {
    cnvs_gradient gr = {
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

    static cnvs_unpremul ramp[CNVS_GRAD_RAMP_N];
    double sink = 0.0;
    int reps = bench_reps();
    for (int rep = 0; rep < reps; rep++) {
        for (int it = 0; it < ITERS; it++) {
            cnvs_gradient_build_ramp(&gr, ramp, CNVS_GRAD_RAMP_N);  // once per fill
            for (int y = 0; y < DIM; y++) {
                for (int x = 0; x < DIM; x++) {
                    cnvs_vec2 p = { .x = (float)x + 0.5f, .y = (float)y + 0.5f };
                    float t;
                    cnvs_unpremul c = cnvs_gradient_param(&gr, p, &t)
                        ? ramp[(int)(t * (float)(CNVS_GRAD_RAMP_N - 1) + 0.5f)]
                        : cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 0.0f);
                    sink += (double)c.r + (double)c.g + (double)c.b + (double)c.a;
                }
            }
        }
    }
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
