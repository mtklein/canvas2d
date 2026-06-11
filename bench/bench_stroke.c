// Isolated benchmark: stroke expansion (segment quads + bevel joins).
// The scene is built once; only stroking runs in the timed loop.
#include "bench_reps.h"
#include "bench_util.h"

#include "cnvs_geom.h"
#include "cnvs_path.h"
#include "cnvs_stroke.h"

#include <stdio.h>

#define ITERS 400
#define STARS 300

int main(void) {
    struct cnvs_path path;
    cnvs_path_init(&path);
    bench_stars(&path, STARS, 512.0f, 512.0f);

    struct cnvs_verts verts = { .data = NULL, .nverts = 0, .cap = 0 };
    double sink = 0.0;

    int reps = bench_reps();
    for (int rep = 0; rep < reps; rep++) {
        for (int it = 0; it < ITERS; it++) {
            cnvs_verts_reset(&verts);
            for (int s = 0; s < path.nsubs; s++) {
                cnvs_subpath sp = path.subs[s];
                if (sp.count < 2) {
                    continue;
                }
                cnvs_vec2 *poly = path.pts + sp.start;
                cnvs_stroke_polyline(poly, sp.count, sp.closed, 2.0f,
                                     CNVS_JOIN_MITER, CNVS_CAP_BUTT, 10.0f, &verts);
            }
            sink += (double)verts.nverts;
        }
    }

    cnvs_path_free(&path);
    cnvs_verts_free(&verts);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
