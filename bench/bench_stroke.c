// Isolated benchmark: stroke expansion (segment quads + bevel joins).
// The scene is built once; only stroking runs in the timed loop.
#include "bench_reps.h"
#include "bench_util.h"

#include "canvas2d_geom.h"
#include "canvas2d_path.h"
#include "canvas2d_stroke.h"

#include <stdio.h>

#define ITERS 400
#define STARS 300

int main(void) {
    struct canvas2d_path path;
    canvas2d_path_init(&path);
    bench_stars(&path, STARS, 512.0f, 512.0f);

    struct canvas2d_verts verts = { .data = NULL, .nverts = 0, .cap = 0 };
    double sink = 0.0;

    int const reps = bench_reps();
    for (int rep = 0; rep < reps; rep++) {
        for (int it = 0; it < ITERS; it++) {
            canvas2d_verts_reset(&verts);
            for (int s = 0; s < path.nsubs; s++) {
                canvas2d_subpath const sp = path.subs[s];
                if (sp.count < 2) {
                    continue;
                }
                canvas2d_vec2 *poly = path.pts + sp.start;
                canvas2d_stroke_polyline(poly, sp.count, sp.closed, 2.0f,
                                     CANVAS2D_JOIN_MITER, CANVAS2D_CAP_BUTT, 10.0f, &verts);
            }
            sink += (double)verts.nverts;
        }
    }

    canvas2d_path_free(&path);
    canvas2d_verts_free(&verts);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
