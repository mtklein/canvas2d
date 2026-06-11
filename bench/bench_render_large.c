// Real-pipeline benchmark, large-fill variant: a big canvas with a few
// nearly-full-canvas fills per frame -- the opposite shape from bench_render's
// dozen tiny fills.  Here pixels-per-draw is enormous and draw count is tiny, so
// the per-pixel software blend dominates rather than per-draw overhead.
// BENCH_READBACK=end reads once at the end.
#include "bench_reps.h"

#include "canvas.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define DIM 1024
#define FRAMES 10

// One frame: a few big fills, each covering ~the whole canvas (millions of pixels),
// so per-draw/encode overhead is negligible next to the per-pixel work.
static void scene(canvas *__single cv, int f) {
    float w = (float)DIM, h = (float)DIM;
    canvas_clear_rect(cv, 0.0f, 0.0f, w, h);
    canvas_set_fill_rgba(cv, 0.10f, 0.12f, 0.15f, 1.0f);  // full-canvas opaque background
    canvas_fill_rect(cv, 0.0f, 0.0f, w, h);

    // Full-canvas linear gradient over the whole surface.
    canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, w, h);
    canvas_add_fill_color_stop(cv, 0.0f, 1.0f, 0.3f, 0.2f, 0.85f);
    canvas_add_fill_color_stop(cv, 1.0f, 0.2f, 0.4f, 1.0f, 0.85f);
    canvas_fill_rect(cv, 0.0f, 0.0f, w, h);

    // Full-canvas radial gradient centred a bit off, large radius.
    canvas_set_fill_radial_gradient(cv, w * 0.4f, h * 0.45f, 8.0f,
                                    w * 0.5f, h * 0.5f, w * 0.7f);
    canvas_add_fill_color_stop(cv, 0.0f, 1.0f, 1.0f, 1.0f, 0.9f);
    canvas_add_fill_color_stop(cv, 1.0f, 0.1f, 0.2f, 0.5f, 0.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, w, h);

    // A big circular clip with a full-canvas fill under it.
    canvas_save(cv);
    canvas_begin_path(cv);
    canvas_arc(cv, w * 0.5f, h * 0.5f, w * 0.45f, 0.0f, 6.2831853f, false);
    canvas_clip(cv);
    canvas_set_fill_rgba(cv, 1.0f, 0.4f, 0.7f, 0.5f);
    canvas_fill_rect(cv, 0.0f, 0.0f, w, h);
    canvas_restore(cv);

    // A full-canvas blend-mode composite (the per-pixel blend math at scale).
    enum canvas_composite_op modes[3] = { CANVAS_OP_MULTIPLY, CANVAS_OP_SCREEN,
                                     CANVAS_OP_LIGHTEN };
    canvas_set_global_composite_operation(cv, modes[f % 3]);
    canvas_set_fill_rgba(cv, 0.9f, 0.3f, 0.5f, 0.6f);
    canvas_fill_rect(cv, 0.0f, 0.0f, w, h);
    canvas_set_global_composite_operation(cv, CANVAS_OP_SOURCE_OVER);
}

int main(void) {
    canvas *__single cv = canvas_create(DIM, DIM);
    int const len = DIM * DIM * 4;
    uint8_t *px = malloc((size_t)len);
    if (!cv || !px) {
        free(px);
        canvas_destroy(cv);
        return 1;
    }

    char const *__null_terminated rb = getenv("BENCH_READBACK");
    bool read_each = !(rb && rb[0] == 'e');

    double sink = 0.0;
    int reps = bench_reps();
    double t0 = bench_now_s();
    for (int rep = 0; rep < reps; rep++) {
        for (int f = 0; f < FRAMES; f++) {
            scene(cv, f);
            if (read_each) {
                canvas_read_rgba(cv, px, len);
                sink += (double)px[(DIM / 2 * DIM + DIM / 2) * 4];
            }
        }
    }
    if (!read_each) {
        canvas_read_rgba(cv, px, len);
        sink += (double)px[(DIM / 2 * DIM + DIM / 2) * 4];
    }
    double secs = bench_now_s() - t0;

    // Output pixels produced: one finished DIM*DIM canvas per frame (finished-frame
    // throughput, comparable across canvas sizes).
    bench_report_throughput(secs, (double)DIM * (double)DIM * (double)FRAMES * (double)reps);
    free(px);
    canvas_destroy(cv);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
