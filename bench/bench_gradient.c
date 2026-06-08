// Isolated benchmark: gradient evaluation -- the radial parameter solve plus the
// multi-stop ramp lookup, sampled across a pixel grid.  This is the per-vertex
// work behind gradient fills (the radial kind is the more expensive of the two).
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
    cnvs_gradient_add_stop(&gr, 0.00f, (gpu_rgba){ .r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f });
    cnvs_gradient_add_stop(&gr, 0.35f, (gpu_rgba){ .r = 0.3f, .g = 0.6f, .b = 0.95f, .a = 1.0f });
    cnvs_gradient_add_stop(&gr, 0.70f, (gpu_rgba){ .r = 0.9f, .g = 0.3f, .b = 0.4f, .a = 1.0f });
    cnvs_gradient_add_stop(&gr, 1.00f, (gpu_rgba){ .r = 0.05f, .g = 0.1f, .b = 0.3f, .a = 1.0f });

    double sink = 0.0;
    for (int it = 0; it < ITERS; it++) {
        for (int y = 0; y < DIM; y++) {
            for (int x = 0; x < DIM; x++) {
                gpu_rgba c = cnvs_gradient_sample(
                    &gr, (cnvs_vec2){ .x = (float)x + 0.5f, .y = (float)y + 0.5f }, 1.0f);
                sink += (double)c.r + (double)c.g + (double)c.b + (double)c.a;
            }
        }
    }
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
