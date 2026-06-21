#pragma once

#include "canvas2d_cover.h"
#include "canvas2d_math.h"
#include "canvas2d_path.h"

#include <math.h>
#include <stdint.h>

// Deterministic LCG in [0, 1).
static inline float bench_frand(void) {
    static uint64_t state = 0x9E3779B97F4A7C15ULL;
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (float)(uint32_t)(state >> 32) / 4294967296.0f;
}

static inline canvas2d_vec2 bench_rpt(float w, float h) {
    return (canvas2d_vec2){ .x = bench_frand() * w, .y = bench_frand() * h };
}

// Append `count` concave star polygons (5..19 spikes each) over a w x h area.
static inline void bench_stars(struct canvas2d_path *path, int count, float w, float h) {
    for (int k = 0; k < count; k++) {
        float const cx = bench_frand() * w;
        float const cy = bench_frand() * h;
        int const spikes = 5 + (int)(bench_frand() * 15.0f);
        float const r1 = 12.0f + bench_frand() * 60.0f;
        float const r2 = r1 * 0.45f;
        int const n = spikes * 2;
        for (int i = 0; i < n; i++) {
            float const a = (float)i * (float)M_PI / (float)spikes;
            float const r = (i % 2 == 0) ? r1 : r2;
            canvas2d_vec2 const p = { .x = cx + r * cosf(a), .y = cy + r * sinf(a) };
            if (i == 0) {
                canvas2d_path_move_to(path, p);
            } else {
                canvas2d_path_line_to(path, p);
            }
        }
        canvas2d_path_close(path);
    }
}

// Accumulate every subpath edge of `p` into a w*h coverage raster (each subpath
// implicitly closed).
static inline void bench_cover_path(struct canvas2d_cover *cov, int w, int h,
                                    struct canvas2d_path const *p) {
    canvas2d_cover_reset(cov, w, h);
    for (int s = 0; s < p->nsubs; s++) {
        canvas2d_subpath const sp = p->subs[s];
        if (sp.count < 2) {
            continue;
        }
        for (int k = 0; k < sp.count; k++) {
            canvas2d_vec2 const a = p->pts[sp.start + k];
            canvas2d_vec2 const b = p->pts[sp.start + (k + 1) % sp.count];
            canvas2d_cover_add_edge(cov, w, h, a.x, a.y, b.x, b.y);
        }
    }
}

// Fill each subpath of `p` as its own shape over its tight, clamped bbox -- the way
// the renderer drives one fill() per path (bbox -> reset -> accumulate offset edges
// -> resolve), instead of one full-canvas pass.  This is the representative case:
// many shapes / glyphs, each resolved over its own (mostly covered) bbox.  `cov`
// holds at least cov_cap bytes; returns a checksum to defeat dead-code elimination.
static inline double bench_fill_shapes(struct canvas2d_cover *cover,
                                       uint8_t *__counted_by(cov_cap) cov, int cov_cap,
                                       int clampw, int clamph, struct canvas2d_path const *p,
                                       enum canvas2d_fill_rule rule) {
    double sink = 0.0;
    for (int s = 0; s < p->nsubs; s++) {
        canvas2d_subpath const sp = p->subs[s];
        if (sp.count < 2) {
            continue;
        }
        float minx = p->pts[sp.start].x, maxx = minx;
        float miny = p->pts[sp.start].y, maxy = miny;
        for (int k = 1; k < sp.count; k++) {
            canvas2d_vec2 const q = p->pts[sp.start + k];
            minx = q.x < minx ? q.x : minx;
            maxx = q.x > maxx ? q.x : maxx;
            miny = q.y < miny ? q.y : miny;
            maxy = q.y > maxy ? q.y : maxy;
        }
        float fx0 = floorf(minx), fy0 = floorf(miny);
        float fx1 = ceilf(maxx), fy1 = ceilf(maxy);
        int x0 = (int)fx0, y0 = (int)fy0, x1 = (int)fx1, y1 = (int)fy1;
        if (x0 < 0) { x0 = 0; }
        if (y0 < 0) { y0 = 0; }
        if (x1 > clampw) { x1 = clampw; }
        if (y1 > clamph) { y1 = clamph; }
        int bw = x1 - x0, bh = y1 - y0;
        if (bw <= 0 || bh <= 0 || bw * bh > cov_cap) {
            continue;
        }
        canvas2d_cover_reset(cover, bw, bh);
        for (int k = 0; k < sp.count; k++) {
            canvas2d_vec2 const a = p->pts[sp.start + k];
            canvas2d_vec2 const b = p->pts[sp.start + (k + 1) % sp.count];
            canvas2d_cover_add_edge(cover, bw, bh, a.x - (float)x0, a.y - (float)y0,
                                b.x - (float)x0, b.y - (float)y0);
        }
        canvas2d_cover_resolve(cover, bw, bh, rule, cov);
        sink += (double)cov[(bh / 2) * bw + bw / 2];
    }
    return sink;
}
