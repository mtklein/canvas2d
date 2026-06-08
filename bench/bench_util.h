#pragma once

// Shared helpers for the benchmarks.  Marked [[maybe_unused]] (C23) so each
// bench TU can include this and use only what it needs under -Weverything.

#include "cnvs_math.h"
#include "cnvs_path.h"

#include <math.h>
#include <stdint.h>

[[maybe_unused]] static uint64_t bench_rng = 0x9E3779B97F4A7C15ULL;

// Deterministic LCG in [0, 1).
[[maybe_unused]] static float bench_frand(void) {
    bench_rng = bench_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (float)(uint32_t)(bench_rng >> 32) / 4294967296.0f;
}

[[maybe_unused]] static cnvs_vec2 bench_rpt(float w, float h) {
    return (cnvs_vec2){ .x = bench_frand() * w, .y = bench_frand() * h };
}

// Append `count` concave star polygons (5..19 spikes each) over a w x h area.
[[maybe_unused]] static void bench_stars(cnvs_path *path, int count, float w, float h) {
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
