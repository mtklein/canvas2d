// Isolated benchmark: clipped 2D RGBA8 blit (the getImageData copy path).
#include "bench_reps.h"

#include "canvas2d_blit.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ITERS 1500
#define DIM 256

int main(void) {
    int const len = DIM * DIM * 4;
    uint8_t *src = malloc((size_t)len);
    uint8_t *dst = malloc((size_t)len);
    if (!src || !dst) {
        free(src);
        free(dst);
        return 1;
    }
    for (int i = 0; i < len; i++) {
        src[i] = (uint8_t)i;
    }

    double sink = 0.0;
    int const reps = bench_reps();
    for (int rep = 0; rep < reps; rep++) {
        for (int it = 0; it < ITERS; it++) {
            int const off = it % 64 - 32;  // some iterations clip at the edges
            canvas2d_blit_rgba(dst, DIM, DIM, off, off, src, DIM, DIM, 0, 0, DIM, DIM);
            sink += (double)dst[0];
        }
    }

    free(src);
    free(dst);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
