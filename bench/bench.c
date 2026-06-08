// End-to-end CPU benchmark (no GPU): flatten, fill, stroke, encode.  The
// bench_*.c files isolate each phase.  release vs unsafe differ only in
// -fbounds-safety, so `ninja benchcmp` measures what the bounds checks cost.

#include "bench_util.h"

#include "cnvs_fill.h"
#include "cnvs_geom.h"
#include "cnvs_path.h"
#include "cnvs_png.h"
#include "cnvs_stroke.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    cnvs_path path;
    cnvs_path_init(&path);
    cnvs_verts verts = { .data = NULL, .len = 0, .cap = 0 };
    cnvs_edges edges = { .data = NULL, .len = 0, .cap = 0 };
    cnvs_xings xings = { .data = NULL, .len = 0, .cap = 0 };

    double sink = 0.0;
    int const frames = 100;
    float const w = 512.0f;
    float const h = 512.0f;

    for (int f = 0; f < frames; f++) {
        cnvs_path_reset(&path);

        // Concave star polygons -> stress ear clipping.
        bench_stars(&path, 40, w, h);

        // Cubic Beziers -> stress adaptive de Casteljau flattening.
        for (int k = 0; k < 40; k++) {
            cnvs_path_move_to(&path, bench_rpt(w, h));
            cnvs_path_cubic_to(&path, bench_rpt(w, h), bench_rpt(w, h),
                               bench_rpt(w, h), 0.25f);
        }

        cnvs_verts_reset(&verts);
        cnvs_fill_path(&path, CNVS_NONZERO, (int)w, (int)h, &verts, &edges, &xings);
        sink += (double)verts.len;

        cnvs_verts_reset(&verts);
        for (int s = 0; s < path.sp_len; s++) {
            cnvs_subpath sp = path.subs[s];
            if (sp.count < 2) {
                continue;
            }
            cnvs_vec2 *poly = path.pts + sp.start;
            cnvs_stroke_polyline(poly, sp.count, sp.closed, 2.0f, &verts);
        }
        sink += (double)verts.len;
    }

    // /dev/null keeps disk I/O out of the timing; the encode still runs in full.
    {
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
    cnvs_edges_free(&edges);
    cnvs_xings_free(&xings);

    fprintf(stderr, "sink=%.0f\n", sink);  // defeat dead-code elimination
    return 0;
}
