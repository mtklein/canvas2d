// Isolated benchmark: the horizontal box-blur pass (stride 1, contiguous rows).
// Pairs with bench_blur_v.c (stride w) to compare the x and y axes at equal work.
#include "bench_reps.h"

#include "blur.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ITERS 200
#define DIM 512
#define RADIUS 4

int main(void) {
    int const n = DIM * DIM;
    uint8_t *src = malloc((size_t)n);
    uint8_t *dst = malloc((size_t)n);
    if (!src || !dst) {
        free(src); free(dst);
        return 1;
    }
    for (int i = 0; i < n; i++) {
        src[i] = (uint8_t)((i * 37 + 11) & 0xFF);
    }

    double sink = 0.0;
    int const reps = bench_reps();
    for (int rep = 0; rep < reps; rep++) {
        for (int it = 0; it < ITERS; it++) {
            blur_box_h(dst, src, DIM, DIM, RADIUS);
            sink += (double)dst[n / 2];
        }
    }

    free(src); free(dst);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
