// Isolated benchmark: contiguous-runs ring sum -- the restructuring that was
// supposed to dodge the checks but doesn't (run lengths != cap, so neither inner
// loop's bound matches the count and the checks survive).  Pairs with
// bench_ring_masked.c.
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
            sink += ring_sum_runs(buf, CAP, it & (CAP - 1), CAP);
        }
    }

    free(buf);
    fprintf(stderr, "sink=%ld\n", sink);
    return 0;
}
