// Isolated benchmark: adaptive de Casteljau flattening of cubic Beziers.
#include "bench_reps.h"
#include "bench_util.h"

#include "cnvs_path.h"

#include <stdio.h>

#define ITERS 4000
#define CURVES 100

int main(void) {
    cnvs_path path;
    cnvs_path_init(&path);
    double sink = 0.0;

    int reps = bench_reps();
    for (int rep = 0; rep < reps; rep++) {
        for (int it = 0; it < ITERS; it++) {
            cnvs_path_reset(&path);
            for (int k = 0; k < CURVES; k++) {
                cnvs_path_move_to(&path, bench_rpt(512.0f, 512.0f));
                cnvs_path_cubic_to(&path, bench_rpt(512.0f, 512.0f),
                                   bench_rpt(512.0f, 512.0f),
                                   bench_rpt(512.0f, 512.0f), 0.25f);
            }
            sink += (double)path.pt_len;
        }
    }

    cnvs_path_free(&path);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
