// Isolated benchmark: analytic coverage fill (signed-area accumulation + per-row
// prefix-sum resolve), driven the way the renderer does -- one fill() per shape over
// its own tight bbox (see bench_fill_shapes), not one full-canvas pass.  The scene
// is built once; only the rasterization is timed.
#include "bench_reps.h"
#include "bench_util.h"

#include "cnvs_cover.h"
#include "cnvs_path.h"

#include <stdio.h>
#include <stdlib.h>

#define ITERS 32
#define STARS 200
#define DIM 512

int main(void) {
    struct cnvs_path path;
    cnvs_path_init(&path);
    bench_stars(&path, STARS, (float)DIM, (float)DIM);

    struct cnvs_cover cover = { .acc = NULL, .cap = 0 };
    int const cov_cap = DIM * DIM;
    uint8_t *cov = malloc((size_t)cov_cap);
    double sink = 0.0;

    int reps = bench_reps();
    for (int rep = 0; rep < reps && cov; rep++) {
        for (int it = 0; it < ITERS; it++) {
            sink += bench_fill_shapes(&cover, cov, cov_cap, DIM, DIM, &path,
                                      CNVS_NONZERO);
        }
    }

    cnvs_path_free(&path);
    cnvs_cover_free(&cover);
    free(cov);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
