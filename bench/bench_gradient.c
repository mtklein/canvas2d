// Isolated benchmark: gradient evaluation (radial parameter solve + multi-stop ramp
// lookup) sampled across a pixel grid; radial is the costlier of the two kinds.
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

    double sink = 0.0;
    for (int it = 0; it < ITERS; it++) {
        for (int y = 0; y < DIM; y++) {
            for (int x = 0; x < DIM; x++) {
                cnvs_unpremul c = cnvs_gradient_sample(
                    &gr, (cnvs_vec2){ .x = (float)x + 0.5f, .y = (float)y + 0.5f }, 1.0f);
                sink += (double)c.r + (double)c.g + (double)c.b + (double)c.a;
            }
        }
    }
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
