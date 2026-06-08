#pragma once

#include "cnvs_cover.h"
#include "cnvs_math.h"
#include "cnvs_path.h"

#include <math.h>
#include <stdint.h>

// Deterministic LCG in [0, 1).
static inline float bench_frand(void) {
    static uint64_t state = 0x9E3779B97F4A7C15ULL;
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (float)(uint32_t)(state >> 32) / 4294967296.0f;
}

static inline cnvs_vec2 bench_rpt(float w, float h) {
    return (cnvs_vec2){ .x = bench_frand() * w, .y = bench_frand() * h };
}

// Append `count` concave star polygons (5..19 spikes each) over a w x h area.
static inline void bench_stars(cnvs_path *path, int count, float w, float h) {
    for (int k = 0; k < count; k++) {
        float cx = bench_frand() * w;
        float cy = bench_frand() * h;
        int spikes = 5 + (int)(bench_frand() * 15.0f);
        float r1 = 12.0f + bench_frand() * 60.0f;
        float r2 = r1 * 0.45f;
        int n = spikes * 2;
        for (int i = 0; i < n; i++) {
            float a = (float)i * (float)M_PI / (float)spikes;
            float r = (i % 2 == 0) ? r1 : r2;
            cnvs_vec2 p = { .x = cx + r * cosf(a), .y = cy + r * sinf(a) };
            if (i == 0) {
                cnvs_path_move_to(path, p);
            } else {
                cnvs_path_line_to(path, p);
            }
        }
        cnvs_path_close(path);
    }
}

// Accumulate every subpath edge of `p` into a w*h coverage raster (each subpath
// implicitly closed).  The shared fill hot path for the benchmarks.
static inline void bench_cover_path(cnvs_cover *cov, int w, int h,
                                    cnvs_path const *p) {
    cnvs_cover_reset(cov, w, h);
    for (int s = 0; s < p->sp_len; s++) {
        cnvs_subpath sp = p->subs[s];
        if (sp.count < 2) {
            continue;
        }
        for (int k = 0; k < sp.count; k++) {
            cnvs_vec2 a = p->pts[sp.start + k];
            cnvs_vec2 b = p->pts[sp.start + (k + 1) % sp.count];
            cnvs_cover_add_edge(cov, w, h, a.x, a.y, b.x, b.y);
        }
    }
}
