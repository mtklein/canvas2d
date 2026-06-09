// Isolated benchmark: masked ring sum (buf[(tail+i) & (cap-1)]) -- one (provably-
// true) bounds check per element, since the flag won't fold the mask.  Pairs with
// bench_ring_runs.c; release-vs-unsafe isolates the irreducible check cost.
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
            sink += ring_sum_masked(buf, CAP, it & (CAP - 1), CAP);
        }
    }

    free(buf);
    fprintf(stderr, "sink=%ld\n", sink);
    return 0;
}
