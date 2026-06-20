// Encode-path benchmark: the canvas -> BT.2100 16-bit PNG pipeline that no other
// bench reached -- surface_to_pq16 (unpremultiply, Rec.2020 matrix, the PQ OETF,
// quantize) followed by the zlib deflate.  A varied scene is rendered once, then
// the loop re-encodes it so the PQ OETF (now 8-wide) and the codec run on real,
// per-pixel-varying data.  Profile with `sample`; price with `ninja benchcmp`.
#include "bench_reps.h"

#include "canvas.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define DIM 512

int main(void) {
    struct canvas *__single cv = canvas(DIM, DIM, CANVAS_CS_SRGB);
    if (!cv) {
        return 1;
    }
    // Overlapping gradients so every pixel differs -- the PQ OETF does real
    // per-pixel work, not a constant the compiler could hoist.
    canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, (float)DIM, (float)DIM);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 0.02f, 0.10f, 0.40f, 1.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 1.0f, 0.90f, 0.50f, 0.10f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)DIM, (float)DIM);
    canvas_set_fill_radial_gradient(cv, (float)DIM * 0.5f, (float)DIM * 0.5f, 8.0f,
                                    (float)DIM * 0.5f, (float)DIM * 0.5f, (float)DIM * 0.6f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 1.0f, 1.0f, 0.8f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 1.0f, 0.10f, 0.20f, 0.50f, 0.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)DIM, (float)DIM);

    double sink = 0.0;
    int const reps = bench_reps();
    double const t0 = bench_now_s();
    for (int r = 0; r < reps; r++) {
        int len = 0;
        uint8_t *png = canvas_encode_png(cv, &len);
        if (png) {
            sink += (double)len + (double)png[len / 2];
            free(png);
        }
    }
    double const secs = bench_now_s() - t0;

    bench_report_throughput(secs, (double)DIM * (double)DIM * (double)reps);
    canvas_free(cv);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
