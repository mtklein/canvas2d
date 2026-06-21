// Generates a seed corpus for fuzz_api.c: a handful of real drawing programs so
// AFL starts from meaningful renderer coverage rather than random bytes.  The
// byte layout mirrors fuzz_api.c's decoder exactly (4 leading bytes = canvas
// W,H via rd_range; then op byte + args per call; floats are raw 4-byte LE).
//
// Usage: ./seed_gen <out-dir>   (writes seedNN.bin)

#include "fuzz_ops.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct buf {
    uint8_t b[4096];
    int n;
};

static void u8(struct buf *s, int v)      { if (s->n < (int)sizeof s->b) { s->b[s->n++] = (uint8_t)v; } }
static void f32(struct buf *s, float f)   { uint8_t t[4]; memcpy(t, &f, 4); for (int i = 0; i < 4; i++) { u8(s, t[i]); } }
static void rng(struct buf *s, int lo, int v) { int d = v - lo; u8(s, (d >> 8) & 0xFF); u8(s, d & 0xFF); }  // -> lo + d
static void op(struct buf *s, enum fuzz_op o)  { u8(s, (int)o); }

static void canvas2d_hw(struct buf *s, int w, int h) { rng(s, 1, w); rng(s, 1, h); }

static void write_seed(char const *dir, int idx, struct buf const *s) {
    char path[512];
    (void)snprintf(path, sizeof path, "%s/seed%02d.bin", dir, idx);
    FILE *f = fopen(path, "wb");
    if (f) {
        (void)fwrite(s->b, 1, (size_t)s->n, f);
        (void)fclose(f);
    }
}

int main(int argc, char **argv) {
    char const *dir = argc > 1 ? argv[1] : ".";
    int idx = 0;

    {   // filled triangle
        struct buf s = { 0 }; canvas2d_hw(&s, 128, 128);
        op(&s, OP_SET_FILL_RGBA); f32(&s, 0.8f); f32(&s, 0.2f); f32(&s, 0.2f); f32(&s, 1.0f);
        op(&s, OP_BEGIN_PATH);
        op(&s, OP_MOVE_TO); f32(&s, 10); f32(&s, 10);
        op(&s, OP_LINE_TO); f32(&s, 110); f32(&s, 20);
        op(&s, OP_LINE_TO); f32(&s, 60); f32(&s, 100);
        op(&s, OP_CLOSE_PATH);
        op(&s, OP_FILL);
        write_seed(dir, idx++, &s);
    }
    {   // stroked bezier with dashes and joins
        struct buf s = { 0 }; canvas2d_hw(&s, 200, 80);
        op(&s, OP_SET_LINE_WIDTH); f32(&s, 4.0f);
        op(&s, OP_SET_LINE_JOIN); rng(&s, 0, 1);
        op(&s, OP_SET_LINE_DASH); rng(&s, 0, 2); f32(&s, 6); f32(&s, 3);
        op(&s, OP_BEGIN_PATH);
        op(&s, OP_MOVE_TO); f32(&s, 10); f32(&s, 40);
        op(&s, OP_CUBIC_TO); f32(&s, 60); f32(&s, 0); f32(&s, 140); f32(&s, 80); f32(&s, 190); f32(&s, 40);
        op(&s, OP_STROKE);
        write_seed(dir, idx++, &s);
    }
    {   // arc + ellipse + round_rect fills, even-odd
        struct buf s = { 0 }; canvas2d_hw(&s, 160, 160);
        op(&s, OP_SET_FILL_RULE); rng(&s, 0, 1);
        op(&s, OP_BEGIN_PATH);
        op(&s, OP_ARC); f32(&s, 80); f32(&s, 80); f32(&s, 50); f32(&s, 0); f32(&s, 6.2831853f); u8(&s, 0);
        op(&s, OP_FILL);
        op(&s, OP_BEGIN_PATH);
        op(&s, OP_ROUND_RECT); f32(&s, 20); f32(&s, 20); f32(&s, 60); f32(&s, 40); f32(&s, 8);
        op(&s, OP_FILL);
        write_seed(dir, idx++, &s);
    }
    {   // gradient fill + clip + transform
        struct buf s = { 0 }; canvas2d_hw(&s, 200, 120);
        op(&s, OP_TRANSLATE); f32(&s, 20); f32(&s, 10);
        op(&s, OP_ROTATE); f32(&s, 0.2f);
        op(&s, OP_FILL_LINEAR_GRAD); f32(&s, 0); f32(&s, 0); f32(&s, 180); f32(&s, 0);
        op(&s, OP_ADD_FILL_STOP); f32(&s, 1); f32(&s, 0); f32(&s, 0); f32(&s, 0); f32(&s, 1);
        op(&s, OP_ADD_FILL_STOP); f32(&s, 0); f32(&s, 0); f32(&s, 1); f32(&s, 1); f32(&s, 1);
        op(&s, OP_BEGIN_PATH);
        op(&s, OP_RECT); f32(&s, 0); f32(&s, 0); f32(&s, 100); f32(&s, 100);
        op(&s, OP_CLIP);
        op(&s, OP_FILL_RECT); f32(&s, 0); f32(&s, 0); f32(&s, 200); f32(&s, 200);
        write_seed(dir, idx++, &s);
    }
    {   // save/restore + image data round-trip + draw_image
        struct buf s = { 0 }; canvas2d_hw(&s, 96, 96);
        op(&s, OP_SAVE);
        op(&s, OP_SET_GLOBAL_ALPHA); f32(&s, 0.5f);
        op(&s, OP_FILL_RECT); f32(&s, 8); f32(&s, 8); f32(&s, 40); f32(&s, 40);
        op(&s, OP_RESTORE);
        op(&s, OP_GET_IMAGE_DATA); rng(&s, 0, 16); rng(&s, 0, 16); rng(&s, -8, 0); rng(&s, -8, 0);
        op(&s, OP_PUT_IMAGE_DATA); rng(&s, 0, 8); rng(&s, 0, 8); rng(&s, -8, 4); rng(&s, -8, 4);
        // PUT consumes w*h*4 data bytes; supply a few (short read clamps fine).
        for (int i = 0; i < 8 * 8 * 4; i++) { u8(&s, (i * 37) & 0xFF); }
        op(&s, OP_DRAW_IMAGE); rng(&s, 1, 8); rng(&s, 1, 8);
        for (int i = 0; i < 8 * 8 * 4; i++) { u8(&s, (i * 53) & 0xFF); }
        f32(&s, 4); f32(&s, 4); f32(&s, 40); f32(&s, 40);
        write_seed(dir, idx++, &s);
    }
    {   // composite modes over a gradient
        struct buf s = { 0 }; canvas2d_hw(&s, 120, 120);
        op(&s, OP_SET_COMPOSITE); rng(&s, 0, 13);
        op(&s, OP_SET_FILL_RGBA); f32(&s, 0.2f); f32(&s, 0.6f); f32(&s, 0.9f); f32(&s, 0.7f);
        op(&s, OP_BEGIN_PATH);
        op(&s, OP_ARC); f32(&s, 60); f32(&s, 60); f32(&s, 40); f32(&s, 0); f32(&s, 6.28f); u8(&s, 0);
        op(&s, OP_FILL);
        write_seed(dir, idx++, &s);
    }

    {   // perspective quad: map a source rect onto a keystoned destination, then
        // fill it -- drives the homography solve + the per-pixel projective divide.
        struct buf s = { 0 }; canvas2d_hw(&s, 160, 160);
        op(&s, OP_SET_PERSPECTIVE_QUAD);
        f32(&s, 0); f32(&s, 0); f32(&s, 100); f32(&s, 100);  // source rect
        f32(&s, 40); f32(&s, 10);    // TL
        f32(&s, 120); f32(&s, 10);   // TR
        f32(&s, 150); f32(&s, 150);  // BR
        f32(&s, 10); f32(&s, 150);   // BL
        op(&s, OP_SET_FILL_RGBA); f32(&s, 0.3f); f32(&s, 0.7f); f32(&s, 0.5f); f32(&s, 1.0f);
        op(&s, OP_FILL_RECT); f32(&s, 0); f32(&s, 0); f32(&s, 100); f32(&s, 100);
        write_seed(dir, idx++, &s);
    }
    {   // 3x3 homography directly, drawing an image through it -- the projective
        // image sampler (w<=0 clip + perspective-correct fetch).
        struct buf s = { 0 }; canvas2d_hw(&s, 128, 128);
        op(&s, OP_SET_TRANSFORM_3X3);
        f32(&s, 1.0f); f32(&s, 0.0f);    f32(&s, 0.0f);
        f32(&s, 1.0f); f32(&s, 10.0f);   f32(&s, 10.0f);
        f32(&s, 0.004f); f32(&s, 0.002f); f32(&s, 1.0f);  // non-zero g,h: projective
        op(&s, OP_DRAW_IMAGE); rng(&s, 1, 16); rng(&s, 1, 16);
        for (int i = 0; i < 16 * 16 * 4; i++) { u8(&s, (i * 29) & 0xFF); }
        f32(&s, 0); f32(&s, 0); f32(&s, 100); f32(&s, 100);
        write_seed(dir, idx++, &s);
    }

    (void)fprintf(stderr, "wrote %d seeds to %s\n", idx, dir);
    return 0;
}
