// Isolated benchmark: analytic coverage fill (signed-area accumulation + per-row
// prefix-sum resolve).  The scene is built once; only the rasterization is timed.
#include "bench_util.h"

#include "cnvs_cover.h"
#include "cnvs_path.h"

#include <stdio.h>
#include <stdlib.h>

#define ITERS 32
#define STARS 200
#define DIM 512

int main(void) {
    cnvs_path path;
    cnvs_path_init(&path);
    bench_stars(&path, STARS, (float)DIM, (float)DIM);

    cnvs_cover cover = { .acc = NULL, .cap = 0 };
    uint8_t *cov = malloc((size_t)(DIM * DIM));
    double sink = 0.0;

    for (int it = 0; it < ITERS && cov; it++) {
        bench_cover_path(&cover, DIM, DIM, &path);
        cnvs_cover_resolve(&cover, DIM, DIM, CNVS_NONZERO, cov);
        sink += (double)cov[(DIM / 2) * DIM + DIM / 2];
    }

    cnvs_path_free(&path);
    cnvs_cover_free(&cover);
    free(cov);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
