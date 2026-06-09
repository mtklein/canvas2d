// Isolated benchmark: the fully bounds-checked register pipeline (design D), same
// source-over computation and tile as the other pixvm benches -- so D vs C isolates
// the cost of refusing the unsafe seams (raw program pointer, void* ctx forge).
#include "bench_reps.h"

#include "pixvm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ITERS 300
#define DIM 256

int main(void) {
    int const n = DIM * DIM;
    int const len = n * 4;
    uint8_t *px = malloc((size_t)len);
    uint8_t *cov = malloc((size_t)n);
    if (!px || !cov) {
        free(px);
        free(cov);
        return 1;
    }
    for (int i = 0; i < n; i++) {
        px[i * 4] = 0; px[i * 4 + 1] = 0; px[i * 4 + 2] = 255; px[i * 4 + 3] = 255;
        cov[i] = (uint8_t)(i & 0xFF);
    }

    double sink = 0.0;
    int reps = bench_reps();
    for (int rep = 0; rep < reps; rep++) {
        for (int it = 0; it < ITERS; it++) {
            pixvm_run_pipe_checked(px, cov, n);
            sink += (double)px[(n / 2) * 4 + 1];
        }
    }

    free(px);
    free(cov);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
