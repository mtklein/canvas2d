// Isolated benchmark: scalar memory LUT (px[i] = lut[px[i]]), one bounds check per
// pixel on the data-dependent index.  Pairs with bench_lut_neon.c (vqtbl4q_u8).
#include "bench_reps.h"

#include "lut.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ITERS 200
#define N (1 << 20)

int main(void) {
    uint8_t *px = malloc(N);
    uint8_t lut[256];
    if (!px) {
        return 1;
    }
    for (int i = 0; i < N; i++) {
        px[i] = (uint8_t)((i * 53 + 7) & 0xFF);
    }
    for (int i = 0; i < 256; i++) {
        lut[i] = (uint8_t)(255 - i);
    }

    double sink = 0.0;
    int reps = bench_reps();
    for (int rep = 0; rep < reps; rep++) {
        for (int it = 0; it < ITERS; it++) {
            lut_apply_mem(px, N, lut);
            sink += (double)px[N / 2];
        }
    }

    free(px);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
