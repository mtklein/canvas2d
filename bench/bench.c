// End-to-end CPU benchmark of the bounds-safety-covered hot paths: curve
// flattening, ear-clip tessellation, stroke expansion, and PNG encoding.  No GPU
// work, so the timing reflects the checked C, not Metal.  See bench_flatten.c /
// bench_tess.c / bench_stroke.c / bench_png.c for the isolated per-phase versions.
//
// Built in two variants that differ ONLY in -fbounds-safety (release vs unsafe),
// so `ninja benchcmp` (hyperfine) measures exactly what the bounds checks cost.
// Deterministic: a fixed-seed LCG drives all geometry.

#include "bench_util.h"

#include "cnvs_geom.h"
#include "cnvs_path.h"
#include "cnvs_png.h"
#include "cnvs_stroke.h"
#include "cnvs_tess.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    cnvs_path path;
    cnvs_path_init(&path);
    cnvs_verts verts = { .data = NULL, .len = 0, .cap = 0 };
    cnvs_ints ring = { .data = NULL, .len = 0, .cap = 0 };

    double sink = 0.0;
    int const frames = 400;
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

        // Fill: tessellate every fillable subpath.
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

        // Stroke: expand every subpath.
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

    // PNG encoding -> stress the per-byte bounds-checked output cursor plus the
    // CRC32/Adler32 loops.  Output goes to /dev/null so disk I/O isn't timed.
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
    cnvs_ints_free(&ring);

    fprintf(stderr, "sink=%.0f\n", sink);  // defeat dead-code elimination
    return 0;
}
