#include "cnvs_blend.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>
#include <string.h>

// Premultiplied blend tile from an unpremultiplied colour (channels in [0,1]).
static cnvs_premul *__counted_by(w * h) make_tile16(int w, int h,
                                                    float r, float g, float b, float a) {
    int n = w * h;
    cnvs_premul *t = malloc((size_t)n * sizeof *t);
    if (!t) {
        return NULL;
    }
    cnvs_premul p = cnvs_premultiply(cnvs_unpremul_of(r, g, b, a));
    for (int i = 0; i < n; i++) {
        t[i] = p;
    }
    return t;
}

// Read the premultiplied target back as unpremultiplied RGBA8 (mirrors the
// canvas-side conversion), so the pixel checks can use unpremultiplied values.
static void read8(canvas *__single c, int w, int h,
                  uint8_t *__counted_by(w * h * 4) out) {
    int const n = w * h;
    cnvs_premul *__counted_by(n) buf = malloc((size_t)n * sizeof *buf);
    if (!buf) {
        return;
    }
    cnvs_blend_read(c, buf, n);
    for (int i = 0; i < n; i++) {
        cnvs_unpremul s = cnvs_unpremultiply(buf[i]);
        out[i * 4]     = (uint8_t)((float)s.r * 255.0f + 0.5f);
        out[i * 4 + 1] = (uint8_t)((float)s.g * 255.0f + 0.5f);
        out[i * 4 + 2] = (uint8_t)((float)s.b * 255.0f + 0.5f);
        out[i * 4 + 3] = (uint8_t)((float)s.a * 255.0f + 0.5f);
    }
    free(buf);
}

// clearRect == destination-out of a unit-alpha tile.
static void clear_all(canvas *__single c, int w, int h) {
    cnvs_premul *t = make_tile16(w, h, 0.0f, 0.0f, 0.0f, 1.0f);
    if (t) {
        cnvs_blend(c, 0, 0, w, h, t, NULL, NULL, 0, CANVAS_OP_DESTINATION_OUT);
        free(t);
    }
}

// The colour pipeline's accuracy gate (docs/decisions/color-axis.md,
// experiment 2): source-over through the real kernel -- premultiply, store,
// blend, un-premultiply, 8-bit quantize -- must land within 1/255 of the
// correctly rounded double-precision reference.  Swept over (src colour x
// src alpha x dst colour) triples onto an opaque destination: a 256x256
// target where x is the source colour and y the destination colour, with the
// source alpha stepped over [0, 255] hitting both endpoints (52 alphas x 64K
// colour pairs = 3.4M triples; the exhaustive 16.7M sweep lives in the memo).
// An aggregate check so a regression reports once, not millions of times.
static void source_over_vs_double(void) {
    enum { N = 256, ASTEP = 5 };
    int const n = N * N;
    cnvs_premul *__counted_by(n) dst = malloc((size_t)n * sizeof *dst);
    cnvs_premul *__counted_by(n) src = malloc((size_t)n * sizeof *src);
    cnvs_premul *__counted_by(n) out = malloc((size_t)n * sizeof *out);
    canvas *__single c = canvas_create(N, N);
    CHECK(dst != NULL && src != NULL && out != NULL && c != NULL);
    if (dst && src && out && c) {
        for (int y = 0; y < N; y++) {
            float dc = (float)y / 255.0f;
            cnvs_premul p = cnvs_premultiply(cnvs_unpremul_of(dc, dc, dc, 1.0f));
            for (int x = 0; x < N; x++) {
                dst[y * N + x] = p;
            }
        }
        int max_delta = 0;
        for (int ai = 0; ai <= 255; ai += ASTEP) {
            float sa = (float)ai / 255.0f;
            for (int x = 0; x < N; x++) {  // one row of sources, replicated
                float sc = (float)x / 255.0f;
                src[x] = cnvs_premultiply(cnvs_unpremul_of(sc, sc, sc, sa));
            }
            for (int y = 1; y < N; y++) {
                memcpy(src + y * N, src, (size_t)N * sizeof *src);
            }
            cnvs_blend(c, 0, 0, N, N, dst, NULL, NULL, 0, CANVAS_OP_COPY);
            cnvs_blend(c, 0, 0, N, N, src, NULL, NULL, 0, CANVAS_OP_SOURCE_OVER);
            cnvs_blend_read(c, out, n);
            for (int y = 0; y < N; y++) {
                for (int x = 0; x < N; x++) {
                    // co = s + (1-sa)*d onto opaque d: ao == 1, so the
                    // unpremultiplied result is co itself.
                    double s = ((double)x / 255.0) * ((double)ai / 255.0);
                    double co = s + (1.0 - (double)ai / 255.0) * ((double)y / 255.0);
                    int want = (int)(co * 255.0 + 0.5);
                    cnvs_unpremul u = cnvs_unpremultiply(out[y * N + x]);
                    int got = (int)((float)u.r * 255.0f + 0.5f);
                    int da = abs(got - want);
                    int aa = abs((int)((float)u.a * 255.0f + 0.5f) - 255);
                    if (da > max_delta) { max_delta = da; }
                    if (aa > max_delta) { max_delta = aa; }
                }
            }
        }
        CHECK(max_delta <= 1);
    }
    canvas_destroy(c);
    free(dst);
    free(src);
    free(out);
}

// cnvs_blend_solid must be byte-identical to cnvs_blend of a tile whose every
// pixel is the colour -- the splat IS the constant tile, minus the round
// trip.  Sweep every mode under all four coverage shapes (none, op plane,
// clip, both), over a gradient destination wide enough to exercise full
// blocks and a 5-pixel tail, and compare the premultiplied targets bit for
// bit.
static void solid_vs_tile(void) {
    enum { W = 61, H = 4 };  // 7 full blocks + a 5-pixel tail per row
    int const n = W * H;
    cnvs_premul *__counted_by(n) dst = malloc((size_t)n * sizeof *dst);
    cnvs_premul *__counted_by(n) tile = malloc((size_t)n * sizeof *tile);
    cnvs_premul *__counted_by(n) got = malloc((size_t)n * sizeof *got);
    cnvs_premul *__counted_by(n) want = malloc((size_t)n * sizeof *want);
    uint8_t *__counted_by(n) covp = malloc((size_t)n);
    uint8_t *__counted_by(n) mask = malloc((size_t)n);
    canvas *__single c = canvas_create(W, H);
    CHECK(dst && tile && got && want && covp && mask && c);
    if (dst && tile && got && want && covp && mask && c) {
        cnvs_premul const color =
            cnvs_premultiply(cnvs_unpremul_of(0.8f, 0.35f, 0.1f, 0.6f));
        for (int i = 0; i < n; i++) {
            float t = (float)i / (float)(n - 1);
            dst[i] = cnvs_premultiply(cnvs_unpremul_of(t, 1.0f - t, 0.4f, t));
            tile[i] = color;
            covp[i] = (uint8_t)(i * 7);   // every byte value over the sweep
            mask[i] = (uint8_t)(255 - i % 256);
        }
        for (int mode = 0; mode < CNVS_BLEND_MODE_COUNT; mode++) {
            for (int shape = 0; shape < 4; shape++) {
                uint8_t const *cv = (shape & 1) ? covp : NULL;
                bool const clipped = (shape & 2) != 0;
                uint8_t const *clip = clipped ? mask : NULL;
                int const clip_len = clipped ? n : 0;

                // Reset the target with no clip (a clipped COPY is a lerp
                // toward the current target, not a reset), then blend under
                // test with the clip in force.
                cnvs_blend(c, 0, 0, W, H, dst, NULL, NULL, 0, CANVAS_OP_COPY);
                cnvs_blend(c, 0, 0, W, H, tile, cv, clip, clip_len,
                           (canvas_composite_op)mode);
                cnvs_blend_read(c, want, n);

                cnvs_blend(c, 0, 0, W, H, dst, NULL, NULL, 0, CANVAS_OP_COPY);
                cnvs_blend_solid(c, 0, 0, W, H, color, cv, clip, clip_len,
                                 (canvas_composite_op)mode);
                cnvs_blend_read(c, got, n);

                CHECK(memcmp(got, want, (size_t)n * sizeof *got) == 0);
            }
        }
    }
    canvas_destroy(c);
    free(dst);
    free(tile);
    free(got);
    free(want);
    free(covp);
    free(mask);
}

int main(void) {
    int const w = 16, h = 16, len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    canvas *__single c = canvas_create(w, h);
    CHECK(px != NULL);
    CHECK(c != NULL);
    if (!px || !c) {
        return TEST_REPORT();
    }

    // Fresh target is transparent.
    read8(c, w, h, px);
    CHECK(px_near(pixel_at(px, len, w, 0, 0), 0, 0, 0, 0, 0));

    // Blend an opaque red tile over a 4x4 region.
    cnvs_premul *red = make_tile16(4, 4, 1.0f, 0.0f, 0.0f, 1.0f);
    if (red) {
        cnvs_blend(c, 2, 2, 4, 4, red, NULL, NULL, 0, CANVAS_OP_SOURCE_OVER);
        read8(c, w, h, px);
        CHECK(px_near(pixel_at(px, len, w, 3, 3), 255, 0, 0, 255, 1));  // painted
        CHECK(px_near(pixel_at(px, len, w, 0, 0), 0, 0, 0, 0, 0));      // outside
        free(red);
    }

    // COPY overwrites the destination -- putImageData's compositing operator.
    cnvs_premul *blue = make_tile16(2, 2, 0.0f, 0.0f, 1.0f, 1.0f);
    if (blue) {
        cnvs_blend(c, 3, 3, 2, 2, blue, NULL, NULL, 0, CANVAS_OP_COPY);
        read8(c, w, h, px);
        CHECK(px_near(pixel_at(px, len, w, 3, 3), 0, 0, 255, 255, 1));  // overwritten
        CHECK(px_near(pixel_at(px, len, w, 2, 2), 255, 0, 0, 255, 1));  // still red
        free(blue);
    }

    // DESTINATION_OUT of a unit-alpha tile erases toward transparent (clearRect).
    clear_all(c, w, h);
    read8(c, w, h, px);
    CHECK(px_near(pixel_at(px, len, w, 3, 3), 0, 0, 0, 0, 0));

    // Translucent over transparent must read back STRAIGHT: a half-alpha red tile
    // is stored premultiplied (0.5,0,0,0.5) but un-premultiplies to (255,0,0,128).
    cnvs_premul *half_red = make_tile16(w, h, 1.0f, 0.0f, 0.0f, 0.5f);
    if (half_red) {
        cnvs_blend(c, 0, 0, w, h, half_red, NULL, NULL, 0, CANVAS_OP_SOURCE_OVER);
        read8(c, w, h, px);
        CHECK(px_near(pixel_at(px, len, w, 8, 8), 255, 0, 0, 128, 3));
        free(half_red);
    }
    clear_all(c, w, h);

    // Clip mask: left half open (255), right half closed (0).
    uint8_t *mask = malloc((size_t)(w * h));
    if (mask) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                mask[y * w + x] = x < 8 ? 255 : 0;
            }
        }
        cnvs_premul *green = make_tile16(w, h, 0.0f, 1.0f, 0.0f, 1.0f);
        if (green) {
            cnvs_blend(c, 0, 0, w, h, green, NULL, mask, w * h,
                       CANVAS_OP_SOURCE_OVER);
            read8(c, w, h, px);
            CHECK(px_near(pixel_at(px, len, w, 4, 8), 0, 255, 0, 255, 1));  // clip open
            CHECK(px_near(pixel_at(px, len, w, 12, 8), 0, 0, 0, 0, 1));     // clip closed
            free(green);
        }
        free(mask);
    }

    // With the clip open again, check source-over compositing of a half-alpha tile.
    clear_all(c, w, h);
    cnvs_premul *opaque_red = make_tile16(w, h, 1.0f, 0.0f, 0.0f, 1.0f);
    cnvs_premul *half_green = make_tile16(w, h, 0.0f, 1.0f, 0.0f, 0.5f);
    if (opaque_red && half_green) {
        cnvs_blend(c, 0, 0, w, h, opaque_red, NULL, NULL, 0, CANVAS_OP_SOURCE_OVER);
        cnvs_blend(c, 0, 0, w, h, half_green, NULL, NULL, 0, CANVAS_OP_SOURCE_OVER);
        read8(c, w, h, px);
        struct px4 m = pixel_at(px, len, w, 8, 8);
        CHECK(m.r > 110 && m.r < 145);  // ~half red survives
        CHECK(m.g > 110 && m.g < 145);  // ~half green over
        CHECK(m.b < 8);
    }
    free(opaque_red);
    free(half_green);

    canvas_destroy(c);
    free(px);

    source_over_vs_double();
    solid_vs_tile();
    return TEST_REPORT();
}
