// Isolated benchmark: ear-clipping tessellation of concave polygons.
// The scene is built once; only the tessellation runs in the timed loop.
#include "bench_util.h"

#include "cnvs_geom.h"
#include "cnvs_path.h"
#include "cnvs_tess.h"

#include <stdio.h>

#define ITERS 200
#define STARS 300

int main(void) {
    cnvs_path path;
    cnvs_path_init(&path);
    bench_stars(&path, STARS, 512.0f, 512.0f);

    cnvs_verts verts = { .data = NULL, .len = 0, .cap = 0 };
    cnvs_ints ring = { .data = NULL, .len = 0, .cap = 0 };
    double sink = 0.0;

    for (int it = 0; it < ITERS; it++) {
        cnvs_verts_reset(&verts);
        for (int s = 0; s < path.sp_len; s++) {
            cnvs_subpath sp = path.subs[s];
            if (sp.count < 3) {
                continue;
            }
            cnvs_vec2 *poly = path.pts + sp.start;
            cnvs_tess_polygon(poly, sp.count, &verts, &ring);
        }
        sink += (double)verts.len;
    }

    cnvs_path_free(&path);
    cnvs_verts_free(&verts);
    cnvs_ints_free(&ring);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
