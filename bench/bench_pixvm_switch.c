// Isolated benchmark: the one-big-switch pixel VM (design A) running a
// premultiplied source-over program over a tile (load dst, scale src by coverage,
// blend, store).  Pairs with bench_pixvm_thread.c (same program, threaded dispatch).
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

    pixop const prog[] = {
        { .kind = PIXOP_SPLAT, .dst = 0, .imm = 0.0f },
        { .kind = PIXOP_SPLAT, .dst = 1, .imm = 0.5f },
        { .kind = PIXOP_SPLAT, .dst = 2, .imm = 0.0f },
        { .kind = PIXOP_SPLAT, .dst = 3, .imm = 0.5f },
        { .kind = PIXOP_LOAD_COV, .dst = 4 },
        { .kind = PIXOP_MUL, .dst = 0, .a = 0, .b = 4 },
        { .kind = PIXOP_MUL, .dst = 1, .a = 1, .b = 4 },
        { .kind = PIXOP_MUL, .dst = 2, .a = 2, .b = 4 },
        { .kind = PIXOP_MUL, .dst = 3, .a = 3, .b = 4 },
        { .kind = PIXOP_LOAD_DST, .dst = 8 },
        { .kind = PIXOP_SPLAT, .dst = 5, .imm = 1.0f },
        { .kind = PIXOP_SUB, .dst = 6, .a = 5, .b = 3 },
        { .kind = PIXOP_MAD, .dst = 8,  .a = 8,  .b = 6, .c = 0 },
        { .kind = PIXOP_MAD, .dst = 9,  .a = 9,  .b = 6, .c = 1 },
        { .kind = PIXOP_MAD, .dst = 10, .a = 10, .b = 6, .c = 2 },
        { .kind = PIXOP_MAD, .dst = 11, .a = 11, .b = 6, .c = 3 },
        { .kind = PIXOP_STORE, .dst = 8 },
    };
    int const nops = (int)(sizeof prog / sizeof prog[0]);

    double sink = 0.0;
    int reps = bench_reps();
    for (int rep = 0; rep < reps; rep++) {
        for (int it = 0; it < ITERS; it++) {
            pixvm_run_switch(prog, nops, px, NULL, cov, n);
            sink += (double)px[(n / 2) * 4 + 1];
        }
    }

    free(px);
    free(cov);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
