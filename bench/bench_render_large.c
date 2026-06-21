// Real-pipeline benchmark, large-fill variant: a big canvas with a few
// nearly-full-canvas fills per frame -- the opposite shape from bench_render's
// dozen tiny fills.  Here pixels-per-draw is enormous and draw count is tiny, so
// the per-pixel software blend dominates rather than per-draw overhead.
// BENCH_READBACK=end reads once at the end.
#include "bench_reps.h"

#include "canvas2d.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define DIM 1024
#define FRAMES 10

// One frame: a few big fills, each covering ~the whole canvas (millions of pixels),
// so per-draw/encode overhead is negligible next to the per-pixel work.
static void scene(struct canvas2d_context *__single cv, int f) {
    float w = (float)DIM, h = (float)DIM;
    canvas2d_clear_rect(cv, 0.0f, 0.0f, w, h);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.10f, 0.12f, 0.15f, 1.0f);  // full-canvas opaque background
    canvas2d_fill_rect(cv, 0.0f, 0.0f, w, h);

    // Full-canvas linear gradient over the whole surface.
    canvas2d_set_fill_linear_gradient(cv, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, 0.0f, 0.0f, w, h);
    canvas2d_add_fill_color_stop(cv, CANVAS2D_CS_SRGB, 0.0f, 1.0f, 0.3f, 0.2f, 0.85f);
    canvas2d_add_fill_color_stop(cv, CANVAS2D_CS_SRGB, 1.0f, 0.2f, 0.4f, 1.0f, 0.85f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, w, h);

    // Full-canvas radial gradient centred a bit off, large radius.
    canvas2d_set_fill_radial_gradient(cv, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, w * 0.4f, h * 0.45f, 8.0f,
                                    w * 0.5f, h * 0.5f, w * 0.7f);
    canvas2d_add_fill_color_stop(cv, CANVAS2D_CS_SRGB, 0.0f, 1.0f, 1.0f, 1.0f, 0.9f);
    canvas2d_add_fill_color_stop(cv, CANVAS2D_CS_SRGB, 1.0f, 0.1f, 0.2f, 0.5f, 0.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, w, h);

    // A big circular clip with a full-canvas fill under it.
    canvas2d_save(cv);
    canvas2d_begin_path(cv);
    canvas2d_arc(cv, w * 0.5f, h * 0.5f, w * 0.45f, 0.0f, 6.2831853f, false);
    canvas2d_clip(cv, CANVAS2D_NONZERO);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 0.4f, 0.7f, 0.5f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, w, h);
    canvas2d_restore(cv);

    // A full-canvas blend-mode composite (the per-pixel blend math at scale).
    enum canvas2d_composite_op modes[3] = { CANVAS2D_OP_MULTIPLY, CANVAS2D_OP_SCREEN,
                                     CANVAS2D_OP_LIGHTEN };
    canvas2d_set_global_composite_operation(cv, modes[f % 3]);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.9f, 0.3f, 0.5f, 0.6f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, w, h);
    canvas2d_set_global_composite_operation(cv, CANVAS2D_OP_SOURCE_OVER);
}

int main(void) {
    struct canvas2d_context *__single cv = canvas2d(DIM, DIM, CANVAS2D_CS_SRGB);
    int const len = DIM * DIM * 4;
    uint8_t *px = malloc((size_t)len);
    if (!cv || !px) {
        free(px);
        canvas2d_free(cv);
        return 1;
    }

    char const *__null_terminated rb = getenv("BENCH_READBACK");
    bool const read_each = !(rb && rb[0] == 'e');

    double sink = 0.0;
    int const reps = bench_reps();
    double const t0 = bench_now_s();
    for (int rep = 0; rep < reps; rep++) {
        for (int f = 0; f < FRAMES; f++) {
            scene(cv, f);
            if (read_each) {
                canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
                sink += (double)px[(DIM / 2 * DIM + DIM / 2) * 4];
            }
        }
    }
    if (!read_each) {
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
        sink += (double)px[(DIM / 2 * DIM + DIM / 2) * 4];
    }
    double const secs = bench_now_s() - t0;

    // Output pixels produced: one finished DIM*DIM canvas per frame (finished-frame
    // throughput, comparable across canvas sizes).
    bench_report_throughput(secs, (double)DIM * (double)DIM * (double)FRAMES * (double)reps);
    free(px);
    canvas2d_free(cv);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
