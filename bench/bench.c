// End-to-end CPU benchmark (no GPU): flatten, fill, stroke, encode.  The
// bench_*.c files isolate each phase.  release vs unsafe differ only in
// -fbounds-safety, so `ninja benchcmp` measures what the bounds checks cost.

#include "bench_reps.h"
#include "bench_util.h"

#include "cnvs_cover.h"
#include "cnvs_geom.h"
#include "cnvs_path.h"
#include "cnvs_png.h"
#include "cnvs_stroke.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    // Repeat the e2e workload BENCH_REPS times (default 1, so benchcmp is
    // unchanged); `ninja profile` raises it for a longer, samplable run.
    int reps = bench_reps();

    struct cnvs_path path;
    cnvs_path_init(&path);
    struct cnvs_verts verts = { .data = NULL, .len = 0, .cap = 0 };
    struct cnvs_cover cover = { .acc = NULL, .cap = 0 };

    int const w = 512;
    int const h = 512;
    uint8_t *cov = malloc((size_t)(w * h));
    double sink = 0.0;
    int const frames = 100;

    for (int rep = 0; rep < reps; rep++) {
        for (int f = 0; f < frames && cov; f++) {
            cnvs_path_reset(&path);

            // Concave, self-intersecting star polygons -> stress the fill rasterizer.
            bench_stars(&path, 40, (float)w, (float)h);

            // Cubic Beziers -> stress adaptive de Casteljau flattening.
            for (int k = 0; k < 40; k++) {
                cnvs_path_move_to(&path, bench_rpt((float)w, (float)h));
                cnvs_path_cubic_to(&path, bench_rpt((float)w, (float)h),
                                   bench_rpt((float)w, (float)h),
                                   bench_rpt((float)w, (float)h), 0.25f);
            }

            bench_cover_path(&cover, w, h, &path);
            cnvs_cover_resolve(&cover, w, h, CNVS_NONZERO, cov);
            sink += (double)cov[(h / 2) * w + w / 2];

            cnvs_verts_reset(&verts);
            for (int s = 0; s < path.sp_len; s++) {
                cnvs_subpath sp = path.subs[s];
                if (sp.count < 2) {
                    continue;
                }
                cnvs_vec2 *poly = path.pts + sp.start;
                cnvs_stroke_polyline(poly, sp.count, sp.closed, 2.0f,
                                     CNVS_JOIN_MITER, CNVS_CAP_BUTT, 10.0f, &verts);
            }
            sink += (double)verts.len;
        }

        // /dev/null keeps disk I/O out of the timing; the encode still runs in full.
        int const iw = 256;
        int const ih = 256;
        int const len = iw * ih * 4;
        uint8_t *px = malloc((size_t)len);
        if (px) {
            for (int i = 0; i < len; i++) {
                px[i] = (uint8_t)((i * 37 + frames) & 0xFF);
            }
            for (int i = 0; i < 40; i++) {
                if (cnvs_png_write("/dev/null", px, iw, ih)) {
                    sink += 1.0;
                }
            }
            free(px);
        }
    }

    cnvs_path_free(&path);
    cnvs_verts_free(&verts);
    cnvs_cover_free(&cover);
    free(cov);

    fprintf(stderr, "sink=%.0f\n", sink);  // defeat dead-code elimination
    return 0;
}
