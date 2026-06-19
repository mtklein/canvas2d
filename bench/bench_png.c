// Isolated benchmark: PNG encoding.  /dev/null keeps disk I/O out of the timing.
#include "bench_reps.h"

#include "cnvs_png.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ITERS 80
#define DIM 256

int main(void) {
    int const w = DIM;
    int const h = DIM;
    int const len = w * h * 4;  // uint16 samples
    uint16_t *px = malloc((size_t)len * sizeof *px);
    if (!px) {
        return 1;
    }
    for (int i = 0; i < len; i++) {
        px[i] = (uint16_t)((i * 37) & 0xFFFF);
    }

    double sink = 0.0;
    int const reps = bench_reps();
    for (int rep = 0; rep < reps; rep++) {
        for (int it = 0; it < ITERS; it++) {
            if (cnvs_png_write("/dev/null", px, w, h)) {
                sink += 1.0;
            }
        }
    }

    free(px);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
