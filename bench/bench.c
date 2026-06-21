// End-to-end CPU benchmark (no GPU): flatten, fill, stroke, encode.  The
// bench_*.c files isolate each phase.  release vs unsafe differ only in
// -fbounds-safety, so `ninja benchcmp` measures what the bounds checks cost.

#include "bench_reps.h"
#include "bench_util.h"

#include "canvas2d_cover.h"
#include "canvas2d_geom.h"
#include "canvas2d_path.h"
#include "canvas2d_png.h"
#include "canvas2d_stroke.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    // Repeat the e2e workload BENCH_REPS times (default 1, so benchcmp is
    // unchanged); `ninja profile` raises it for a longer, samplable run.
    int const reps = bench_reps();

    struct canvas2d_path path;
    canvas2d_path_init(&path);
    struct canvas2d_verts verts = { .data = NULL, .nverts = 0, .cap = 0 };
    struct canvas2d_cover cover = { .acc = NULL, .cap = 0 };

    int const w = 512;
    int const h = 512;
    uint8_t *cov = malloc((size_t)(w * h));
    double sink = 0.0;
    int const frames = 100;

    for (int rep = 0; rep < reps; rep++) {
        for (int f = 0; f < frames && cov; f++) {
            canvas2d_path_reset(&path);

            // Concave, self-intersecting star polygons -> stress the fill rasterizer.
            bench_stars(&path, 40, (float)w, (float)h);

            // Cubic Beziers -> stress adaptive de Casteljau flattening.
            for (int k = 0; k < 40; k++) {
                canvas2d_path_move_to(&path, bench_rpt((float)w, (float)h));
                canvas2d_path_cubic_to(&path, bench_rpt((float)w, (float)h),
                                   bench_rpt((float)w, (float)h),
                                   bench_rpt((float)w, (float)h), 0.25f);
            }

            bench_cover_path(&cover, w, h, &path);
            canvas2d_cover_resolve(&cover, w, h, CANVAS2D_NONZERO, cov);
            sink += (double)cov[(h / 2) * w + w / 2];

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

        // /dev/null keeps disk I/O out of the timing; the encode still runs in full.
        int const iw = 256;
        int const ih = 256;
        int const len = iw * ih * 4;  // uint16 samples
        uint16_t *px = malloc((size_t)len * sizeof *px);
        if (px) {
            for (int i = 0; i < len; i++) {
                px[i] = (uint16_t)((i * 37 + frames) & 0xFFFF);
            }
            for (int i = 0; i < 40; i++) {
                if (canvas2d_png_write("/dev/null", px, iw, ih)) {
                    sink += 1.0;
                }
            }
            free(px);
        }
    }

    canvas2d_path_free(&path);
    canvas2d_verts_free(&verts);
    canvas2d_cover_free(&cover);
    free(cov);

    fprintf(stderr, "sink=%.0f\n", sink);  // defeat dead-code elimination
    return 0;
}
