// Isolated benchmark: scanline fill rasterization (edge table + per-row crossing
// sort + winding walk).  The scene is built once; only the fill runs timed.
#include "bench_util.h"

#include "cnvs_fill.h"
#include "cnvs_geom.h"
#include "cnvs_path.h"

#include <stdio.h>

#define ITERS 32
#define STARS 200
#define DIM 512

int main(void) {
    cnvs_path path;
    cnvs_path_init(&path);
    bench_stars(&path, STARS, (float)DIM, (float)DIM);

    cnvs_verts verts = { .data = NULL, .len = 0, .cap = 0 };
    cnvs_edges edges = { .data = NULL, .len = 0, .cap = 0 };
    cnvs_xings xings = { .data = NULL, .len = 0, .cap = 0 };
    double sink = 0.0;

    for (int it = 0; it < ITERS; it++) {
        cnvs_verts_reset(&verts);
        cnvs_fill_path(&path, CNVS_NONZERO, DIM, DIM, &verts, &edges, &xings);
        sink += (double)verts.len;
    }

    cnvs_path_free(&path);
    cnvs_verts_free(&verts);
    cnvs_edges_free(&edges);
    cnvs_xings_free(&xings);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
