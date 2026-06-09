// Isolated benchmark: explicitly-vectorized contiguous-runs ring sum (one whole-
// vector bounds check per 16 elements + register accumulation).  Unlike the scalar
// runs, the flag doesn't block this -- it's fast and opt-level-independent (~same at
// -Os/-O1/-O2), the reliable answer where autovectorization isn't.
#include "bench_reps.h"

#include "ring.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ITERS 50000
#define CAP 4096

int main(void) {
    int32_t *buf = malloc(CAP * sizeof(int32_t));
    if (!buf) {
        return 1;
    }
    for (int i = 0; i < CAP; i++) {
        buf[i] = i * 7 + 1;
    }

    long sink = 0;
    int reps = bench_reps();
    for (int rep = 0; rep < reps; rep++) {
        for (int it = 0; it < ITERS; it++) {
            sink += ring_sum_simd(buf, CAP, it & (CAP - 1), CAP);
        }
    }

    free(buf);
    fprintf(stderr, "sink=%ld\n", sink);
    return 0;
}
