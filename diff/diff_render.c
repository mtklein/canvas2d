// Backend differential: render a fixed set of scenes through the public canvas
// API and dump each as raw getImageData RGBA8 to <outdir>/<name>.rgba (prefixed
// with an 8-byte [int32 w][int32 h] header).  Built once per backend
// (release = Metal, release-cpu = software); diff_compare then diffs the two
// dumps per channel.
//
// Geometry, antialiased coverage, and gradient evaluation are backend-agnostic
// CPU code, and the read-back unpremultiply is shared too -- so any pixel that
// differs between the two dumps isolates the compositor (blend math + the
// float->_Float16 rounding in compositor_cpu.c / shaders/compositor.metal).  The
// scenes therefore lean on compositing: every globalCompositeOperation, plus
// gradients, antialiased edges, and image sampling.

#include "canvas.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static float const TAU = 6.2831853f;

static void dump(canvas *__single cv, char const *dir, char const *name, int w, int h) {
    if (!cv) {
        return;
    }
    int const len = w * h * 4;
    uint8_t *px = malloc((size_t)len);
    if (px) {
        canvas_get_image_data(cv, 0, 0, w, h, px, len);
        char path[512];
        (void)snprintf(path, sizeof path, "%s/%s.rgba", dir, name);
        FILE *f = fopen(path, "wb");
        if (f) {
            int32_t hdr[2] = { w, h };
            (void)fwrite(hdr, sizeof hdr[0], 2, f);
            (void)fwrite(px, 1, (size_t)len, f);
            (void)fclose(f);
        }
        free(px);
    }
    canvas_destroy(cv);
}

// All 26 globalCompositeOperations in a grid: each cell is an opaque gradient
// backdrop (source-over) with a translucent disc composited under that cell's
// mode -- so the dump exercises every blend path over a varying backdrop, with
// antialiased disc edges feeding partial coverage into the blend.
static int const CELL = 56, COLS = 6, ROWS = 5;

static canvas *__single scene_modes(void) {
    int w = COLS * CELL, h = ROWS * CELL;
    canvas *__single c = canvas_create(w, h);
    if (!c) {
        return NULL;
    }
    for (int m = 0; m < 26; m++) {
        float ox = (float)((m % COLS) * CELL), oy = (float)((m / COLS) * CELL);

        canvas_set_global_composite_operation(c, CANVAS_OP_SOURCE_OVER);
        canvas_set_fill_linear_gradient(c, ox, oy, ox + (float)CELL, oy + (float)CELL);
        canvas_add_fill_color_stop(c, 0.0f, 0.15f, 0.35f, 0.85f, 1.0f);
        canvas_add_fill_color_stop(c, 1.0f, 0.95f, 0.80f, 0.20f, 1.0f);
        canvas_fill_rect(c, ox, oy, (float)CELL, (float)CELL);

        canvas_set_global_composite_operation(c, (canvas_composite_op)m);
        canvas_set_fill_rgba(c, 0.90f, 0.20f, 0.25f, 0.70f);
        canvas_begin_path(c);
        canvas_arc(c, ox + 0.42f * (float)CELL, oy + 0.45f * (float)CELL,
                   0.30f * (float)CELL, 0.0f, TAU, false);
        canvas_fill(c);
        canvas_set_fill_rgba(c, 0.20f, 0.85f, 0.40f, 0.60f);
        canvas_begin_path(c);
        canvas_arc(c, ox + 0.60f * (float)CELL, oy + 0.60f * (float)CELL,
                   0.28f * (float)CELL, 0.0f, TAU, false);
        canvas_fill(c);
    }
    return c;
}

// Radial + linear gradient fills (gradient eval is shared CPU code; this checks
// the compositor on smoothly varying premultiplied tiles).
static canvas *__single scene_gradient(void) {
    int w = 128, h = 128;
    canvas *__single c = canvas_create(w, h);
    if (!c) {
        return NULL;
    }
    canvas_set_fill_radial_gradient(c, 54.0f, 50.0f, 4.0f, 64.0f, 64.0f, 70.0f);
    canvas_add_fill_color_stop(c, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_add_fill_color_stop(c, 0.5f, 0.30f, 0.65f, 0.95f, 1.0f);
    canvas_add_fill_color_stop(c, 1.0f, 0.05f, 0.10f, 0.35f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, (float)w, (float)h);
    return c;
}

// drawImage with scaling (bilinear sampling) + a 1:1 blit, composited source-over.
static canvas *__single scene_image(void) {
    int w = 128, h = 128;
    canvas *__single c = canvas_create(w, h);
    if (!c) {
        return NULL;
    }
    canvas_set_fill_rgba(c, 0.10f, 0.11f, 0.14f, 1.0f);
    canvas_fill_rect(c, 0.0f, 0.0f, (float)w, (float)h);

    uint8_t img[16 * 16 * 4];
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            int i = (y * 16 + x) * 4;
            img[i + 0] = (uint8_t)(255.0f * (0.5f + 0.5f * cosf(TAU * (float)x / 16.0f)));
            img[i + 1] = (uint8_t)(255.0f * (0.5f + 0.5f * cosf(TAU * (float)y / 16.0f)));
            img[i + 2] = (uint8_t)(255.0f * (0.5f + 0.5f * cosf(TAU * (float)(x + y) / 16.0f)));
            img[i + 3] = 255;
        }
    }
    canvas_draw_image(c, img, 16, 16, 12.0f, 12.0f);
    canvas_draw_image_scaled(c, img, 16, 16, 44.0f, 12.0f, 72.0f, 72.0f);
    return c;
}

int main(int argc, char **argv) {
    char const *dir = argc > 1 ? argv[1] : ".";
    dump(scene_modes(), dir, "modes", COLS * CELL, ROWS * CELL);
    dump(scene_gradient(), dir, "gradient", 128, 128);
    dump(scene_image(), dir, "image", 128, 128);
    return 0;
}
