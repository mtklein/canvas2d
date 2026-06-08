// CPU-only benchmark of the bounds-safety-covered hot paths: curve flattening,
// ear-clip tessellation, stroke expansion, and PNG encoding.  No GPU work, so
// the timing reflects the checked C, not Metal.
//
// Built in two variants that differ ONLY in -fbounds-safety (release vs unsafe),
// so `ninja benchcmp` (hyperfine) measures exactly what the bounds checks cost.
// Deterministic: a fixed-seed LCG drives all geometry.

#include "cnvs_geom.h"
#include "cnvs_math.h"
#include "cnvs_path.h"
#include "cnvs_png.h"
#include "cnvs_stroke.h"
#include "cnvs_tess.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;

static float frand(void) {
    g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    uint32_t x = (uint32_t)(g_rng >> 32);
    return (float)x / 4294967296.0f;  // [0, 1)
}

static cnvs_vec2 rpt(float w, float h) {
    return (cnvs_vec2){ .x = frand() * w, .y = frand() * h };
}

int main(void) {
    cnvs_path path;
    cnvs_path_init(&path);
    cnvs_verts verts = { .data = NULL, .len = 0, .cap = 0 };
    cnvs_ints ring = { .data = NULL, .len = 0, .cap = 0 };

    double sink = 0.0;
    const int frames = 400;
    const float w = 512.0f;
    const float h = 512.0f;

    for (int f = 0; f < frames; f++) {
        cnvs_path_reset(&path);

        // Concave star polygons -> stress ear clipping (the O(n^2) inner loop
        // does a point-in-triangle test per remaining vertex: heavy checked
        // indexing of both the polygon and the working ring).
        for (int k = 0; k < 40; k++) {
            float cx = frand() * w;
            float cy = frand() * h;
            int spikes = 5 + (int)(frand() * 15.0f);
            float r1 = 12.0f + frand() * 60.0f;
            float r2 = r1 * 0.45f;
            int count = spikes * 2;
            for (int i = 0; i < count; i++) {
                float a = (float)i * (float)M_PI / (float)spikes;
                float r = (i % 2 == 0) ? r1 : r2;
                cnvs_vec2 p = { .x = cx + r * cosf(a), .y = cy + r * sinf(a) };
                if (i == 0) {
                    cnvs_path_move_to(&path, p);
                } else {
                    cnvs_path_line_to(&path, p);
                }
            }
            cnvs_path_close(&path);
        }

        // Cubic Beziers -> stress adaptive de Casteljau flattening.
        for (int k = 0; k < 40; k++) {
            cnvs_path_move_to(&path, rpt(w, h));
            cnvs_path_cubic_to(&path, rpt(w, h), rpt(w, h), rpt(w, h), 0.25f);
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
        const int iw = 256;
        const int ih = 256;
        const int len = iw * ih * 4;
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
