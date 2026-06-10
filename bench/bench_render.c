// Real-pipeline benchmark: a representative frame driven through the public canvas
// API -- solid + gradient fills, strokes, a clip, blend-mode composites, and a
// readback -- so it exercises the whole shipping pipeline (rasterize -> paint_tile
// -> compositor blend -> premultiply/unpremultiply conversions) the way actual
// rendering does, not isolated kernels.  Profile it with `sample`.
#include "bench_reps.h"

#include "canvas.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define DIM 256
#define FRAMES 30

// One frame: a deterministic scene (varies slightly with `f`) covering the ops a
// real page mixes -- the point is proportion, not a pretty picture.
static void scene(canvas *__single cv, int f) {
    float w = (float)DIM, h = (float)DIM;
    canvas_clear_rect(cv, 0.0f, 0.0f, w, h);
    canvas_set_fill_rgba(cv, 0.10f, 0.12f, 0.15f, 1.0f);  // opaque background
    canvas_fill_rect(cv, 0.0f, 0.0f, w, h);

    // A handful of translucent star fills (concave coverage + per-fill blend).
    for (int i = 0; i < 6; i++) {
        float cx = 30.0f + (float)((i * 47 + f * 3) % 200);
        float cy = 40.0f + (float)((i * 31 + f * 5) % 180);
        canvas_set_fill_rgba(cv, 0.2f + 0.1f * (float)i, 0.5f, 0.9f - 0.1f * (float)i, 0.8f);
        canvas_begin_path(cv);
        canvas_move_to(cv, cx + 20.0f, cy);
        for (int k = 1; k < 10; k++) {
            float a = 6.2831853f * (float)k / 10.0f;
            float r = (k % 2) ? 8.0f : 20.0f;
            canvas_line_to(cv, cx + r * cosf(a), cy + r * sinf(a));
        }
        canvas_close_path(cv);
        canvas_fill(cv);
    }

    // Linear + radial gradient fills (paint_tile premultiply + gradient ramp/solve).
    canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, w, h);
    canvas_add_fill_color_stop(cv, 0.0f, 1.0f, 0.3f, 0.2f, 0.9f);
    canvas_add_fill_color_stop(cv, 1.0f, 0.2f, 0.4f, 1.0f, 0.9f);
    canvas_fill_rect(cv, 20.0f, 20.0f, 100.0f, 100.0f);

    canvas_set_fill_radial_gradient(cv, 180.0f, 180.0f, 4.0f, 180.0f, 180.0f, 60.0f);
    canvas_add_fill_color_stop(cv, 0.0f, 1.0f, 1.0f, 1.0f, 0.95f);
    canvas_add_fill_color_stop(cv, 1.0f, 0.1f, 0.2f, 0.5f, 0.0f);
    canvas_fill_rect(cv, 120.0f, 120.0f, 120.0f, 120.0f);

    // A curved stroke.
    canvas_set_stroke_rgba(cv, 0.95f, 0.85f, 0.2f, 0.9f);
    canvas_set_line_width(cv, 3.0f);
    canvas_begin_path(cv);
    canvas_move_to(cv, 10.0f, 200.0f);
    canvas_bezier_curve_to(cv, 80.0f, 120.0f, 160.0f, 240.0f, 240.0f, 160.0f);
    canvas_stroke(cv);

    // A circular clip with fills under it (clip coverage mask).
    canvas_save(cv);
    canvas_begin_path(cv);
    canvas_arc(cv, 128.0f, 128.0f, 70.0f, 0.0f, 6.2831853f, false);
    canvas_clip(cv);
    canvas_set_fill_rgba(cv, 1.0f, 0.4f, 0.7f, 0.6f);
    canvas_fill_rect(cv, 60.0f, 60.0f, 140.0f, 140.0f);
    canvas_restore(cv);

    // A blend-mode composite (compositor blend math beyond source-over).
    canvas_composite_op modes[3] = { CANVAS_OP_MULTIPLY, CANVAS_OP_SCREEN,
                                     CANVAS_OP_LIGHTEN };
    canvas_set_global_composite_operation(cv, modes[f % 3]);
    canvas_set_fill_rgba(cv, 0.9f, 0.3f, 0.5f, 0.7f);
    canvas_fill_rect(cv, 80.0f, 30.0f, 90.0f, 90.0f);
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

    // BENCH_READBACK=end renders every frame but reads back only once at the very
    // end -- the render-without-per-frame-readback case, where the GPU pipelines
    // frames instead of syncing each one (Metal's strength).  Default reads each
    // frame (the getImageData / PNG-export shape, which forces a sync per frame).
    char const *__null_terminated rb = getenv("BENCH_READBACK");
    bool read_each = !(rb && rb[0] == 'e');

    double sink = 0.0;
    int reps = bench_reps();
    double t0 = bench_now_s();
    for (int rep = 0; rep < reps; rep++) {
        for (int f = 0; f < FRAMES; f++) {
            scene(cv, f);
            if (read_each) {
                canvas_read_rgba(cv, px, len);  // readback: compositor_read + unpremultiply
                sink += (double)px[(DIM / 2 * DIM + DIM / 2) * 4];
            }
        }
    }
    if (!read_each) {
        canvas_read_rgba(cv, px, len);  // a single readback at the end
        sink += (double)px[(DIM / 2 * DIM + DIM / 2) * 4];
    }
    double secs = bench_now_s() - t0;

    // Output pixels produced: one finished DIM*DIM canvas per frame.  (Overdraw --
    // the scene's fills cover only parts of the canvas -- isn't counted; this is
    // finished-frame throughput, comparable across canvas sizes.)
    bench_report_throughput(secs, (double)DIM * (double)DIM * (double)FRAMES * (double)reps);
    free(px);
    canvas_destroy(cv);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
