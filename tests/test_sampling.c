// imageSmoothingQuality's sampler tiers: the premultiplied mip chain +
// trilinear minification (medium/high) and the 4x4 BC-spline magnification
// (high).  The cubic pins are Catmull-Rom's values -- if canvas.c's
// CUBIC_B/CUBIC_C one-liner swaps to Mitchell, re-pin here (the half-phase
// weight 0.5625 becomes ~0.47, so 143 becomes ~120).

#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>

// A solid colour survives every tier exactly: mips of a constant are that
// constant, trilinear between equal levels is exact, and the cubic kernel's
// weights sum to 1.
static void flat_exact(void) {
    enum { N = 32 };
    int const len = N * N * 4;
    uint8_t px[N * N * 4];
    uint8_t src[N * N * 4];
    for (int i = 0; i < N * N; i++) {
        src[i * 4 + 0] = 37;
        src[i * 4 + 1] = 90;
        src[i * 4 + 2] = 200;
        src[i * 4 + 3] = 255;
    }
    struct canvas *__single cv = canvas(N, N);
    CHECK(cv != NULL);
    if (cv) {
        canvas_set_image_smoothing_quality(cv, CANVAS_SMOOTHING_MEDIUM);
        canvas_draw_bitmap_scaled(cv, src, N, N, 4.0f, 4.0f, 8.0f, 8.0f);  // 4x minify
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 8, 8), 37, 90, 200, 255, 0));

        canvas_clear_rect(cv, 0.0f, 0.0f, (float)N, (float)N);
        canvas_set_image_smoothing_quality(cv, CANVAS_SMOOTHING_HIGH);
        canvas_draw_bitmap_subrect(cv, src, N, N, 8.0f, 8.0f, 8.0f, 8.0f,
                                  0.0f, 0.0f, 32.0f, 32.0f);  // 4x magnify, cubic
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 16, 16), 37, 90, 200, 255, 1));
        canvas_free(cv);
    }
}

// One-pixel stripes minified 8x: every mip level past the first is exactly
// 50% gray, so medium reads 128; nearest (smoothing off) picks one stripe's
// pole, the aliasing mips exist to prevent.
static void stripes_average(void) {
    enum { N = 32 };
    int const len = N * N * 4;
    uint8_t px[N * N * 4];
    uint8_t src[N * N * 4];
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            uint8_t const v = (x & 1) ? 255 : 0;
            int const o = (y * N + x) * 4;
            src[o + 0] = v;
            src[o + 1] = v;
            src[o + 2] = v;
            src[o + 3] = 255;
        }
    }
    struct canvas *__single cv = canvas(N, N);
    CHECK(cv != NULL);
    if (cv) {
        canvas_set_image_smoothing_quality(cv, CANVAS_SMOOTHING_MEDIUM);
        canvas_draw_bitmap_scaled(cv, src, N, N, 0.0f, 0.0f, 4.0f, 4.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 1, 1), 128, 128, 128, 255, 1));

        canvas_clear_rect(cv, 0.0f, 0.0f, (float)N, (float)N);
        canvas_set_image_smoothing_enabled(cv, false);
        canvas_draw_bitmap_scaled(cv, src, N, N, 0.0f, 0.0f, 4.0f, 4.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        struct rgba const p = pixel_at(px, len, N, 1, 1);
        CHECK(p.r == 0 || p.r == 255);  // one pole, not the average
        canvas_free(cv);
    }
}

// Catmull-Rom's half-phase: a white column among black, upscaled 4x with the
// dest nudged half a pixel, puts a dest centre exactly between the white
// texel and its neighbour -- w(+-0.5) = 0.5625 / -0.0625, so the sample is
// 0.5625 * 255 = 143.  Bilinear (medium magnifies bilinearly) says 128.
static void catmullrom_phase(void) {
    enum { N = 32 };
    int const len = N * N * 4;
    uint8_t px[N * N * 4];
    uint8_t src[4 * 4 * 4];
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            uint8_t const v = x == 1 ? 255 : 0;
            int const o = (y * 4 + x) * 4;
            src[o + 0] = v;
            src[o + 1] = v;
            src[o + 2] = v;
            src[o + 3] = 255;
        }
    }
    struct canvas *__single cv = canvas(N, N);
    CHECK(cv != NULL);
    if (cv) {
        canvas_set_image_smoothing_quality(cv, CANVAS_SMOOTHING_HIGH);
        canvas_draw_bitmap_scaled(cv, src, 4, 4, 0.5f, 0.5f, 16.0f, 16.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 8, 8), 143, 143, 143, 255, 1));

        canvas_clear_rect(cv, 0.0f, 0.0f, (float)N, (float)N);
        canvas_set_image_smoothing_quality(cv, CANVAS_SMOOTHING_MEDIUM);
        canvas_draw_bitmap_scaled(cv, src, 4, 4, 0.5f, 0.5f, 16.0f, 16.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 8, 8), 128, 128, 128, 255, 1));
        canvas_free(cv);
    }
}

// The cubic premultiplies its taps, so a fully transparent texel cannot
// donate colour: a transparent-red / opaque-blue boundary upscaled at high
// has no red anywhere.  (The legacy bilinear filters straight alpha and DOES
// bleed -- pinned here as documentation; if bilinear ever goes
// premultiplied, update this arm.)
static void no_transparent_bleed(void) {
    enum { N = 32 };
    int const len = N * N * 4;
    uint8_t px[N * N * 4];
    uint8_t src[2 * 2 * 4];
    for (int y = 0; y < 2; y++) {
        int const lo = (y * 2 + 0) * 4, ro = (y * 2 + 1) * 4;
        src[lo + 0] = 255; src[lo + 1] = 0; src[lo + 2] = 0;   src[lo + 3] = 0;
        src[ro + 0] = 0;   src[ro + 1] = 0; src[ro + 2] = 255; src[ro + 3] = 255;
    }
    struct canvas *__single cv = canvas(N, N);
    CHECK(cv != NULL);
    if (cv) {
        canvas_set_image_smoothing_quality(cv, CANVAS_SMOOTHING_HIGH);
        canvas_draw_bitmap_scaled(cv, src, 2, 2, 0.0f, 0.0f, 16.0f, 16.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(pixel_at(px, len, N, 8, 8).r <= 1);   // premultiplied taps: no ghost red
        CHECK(pixel_at(px, len, N, 8, 8).b >= 200);

        canvas_clear_rect(cv, 0.0f, 0.0f, (float)N, (float)N);
        canvas_set_image_smoothing_quality(cv, CANVAS_SMOOTHING_LOW);
        canvas_draw_bitmap_scaled(cv, src, 2, 2, 0.0f, 0.0f, 16.0f, 16.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(pixel_at(px, len, N, 8, 8).r > 50);   // straight-alpha bilinear bleeds
        canvas_free(cv);
    }
}

int main(void) {
    flat_exact();
    stripes_average();
    catmullrom_phase();
    no_transparent_bleed();
    return TEST_REPORT();
}
