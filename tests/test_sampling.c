// imageSmoothingQuality's sampler tiers: the premultiplied mip chain +
// trilinear minification (medium/high) and the 4x4 BC-spline magnification
// (high).  The cubic pins are Catmull-Rom's values -- if canvas.c's
// CUBIC_B/CUBIC_C one-liner swaps to Mitchell, re-pin here (the half-phase
// weight 0.5625 becomes ~0.47, so 143 becomes ~120).

#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <ptrcheck.h>
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
        canvas_draw_bitmap_scaled(cv, CANVAS_CS_SRGB, src, N, N, 4.0f, 4.0f, 8.0f, 8.0f);  // 4x minify
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 8, 8), 37, 90, 200, 255, 0));

        canvas_clear_rect(cv, 0.0f, 0.0f, (float)N, (float)N);
        canvas_set_image_smoothing_quality(cv, CANVAS_SMOOTHING_HIGH);
        canvas_draw_bitmap_subrect(cv, CANVAS_CS_SRGB, src, N, N, 8.0f, 8.0f, 8.0f, 8.0f,
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
        canvas_draw_bitmap_scaled(cv, CANVAS_CS_SRGB, src, N, N, 0.0f, 0.0f, 4.0f, 4.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 1, 1), 128, 128, 128, 255, 1));

        canvas_clear_rect(cv, 0.0f, 0.0f, (float)N, (float)N);
        canvas_set_image_smoothing_enabled(cv, false);
        canvas_draw_bitmap_scaled(cv, CANVAS_CS_SRGB, src, N, N, 0.0f, 0.0f, 4.0f, 4.0f);
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
        canvas_draw_bitmap_scaled(cv, CANVAS_CS_SRGB, src, 4, 4, 0.5f, 0.5f, 16.0f, 16.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 8, 8), 143, 143, 143, 255, 1));

        canvas_clear_rect(cv, 0.0f, 0.0f, (float)N, (float)N);
        canvas_set_image_smoothing_quality(cv, CANVAS_SMOOTHING_MEDIUM);
        canvas_draw_bitmap_scaled(cv, CANVAS_CS_SRGB, src, 4, 4, 0.5f, 0.5f, 16.0f, 16.0f);
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
        canvas_draw_bitmap_scaled(cv, CANVAS_CS_SRGB, src, 2, 2, 0.0f, 0.0f, 16.0f, 16.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(pixel_at(px, len, N, 8, 8).r <= 1);   // premultiplied taps: no ghost red
        CHECK(pixel_at(px, len, N, 8, 8).b >= 200);

        canvas_clear_rect(cv, 0.0f, 0.0f, (float)N, (float)N);
        canvas_set_image_smoothing_quality(cv, CANVAS_SMOOTHING_LOW);
        canvas_draw_bitmap_scaled(cv, CANVAS_CS_SRGB, src, 2, 2, 0.0f, 0.0f, 16.0f, 16.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(pixel_at(px, len, N, 8, 8).r > 50);   // straight-alpha bilinear bleeds
        canvas_free(cv);
    }
}

// --- the f16 sampler family + a premul cubic ---------------------------------
//
// The f16 sampler trio (sample_src_nearest_f16 / sample_src_f16 /
// sample_src_cubic_f16) mirrors the unorm8 trio: an f16 image whose channels are
// byte values / 255 should sample to the same result a byte image of those
// bytes does, within f16/quantize tolerance.  We reference each f16 draw against
// the equivalent unorm8 draw rather than re-deriving the kernels.  A premul cubic
// arm covers CANVAS_ALPHA_PREMUL through the magnifying 4x4 path.

// Build a 4x4 source with a single bright column at x==1 (the catmullrom_phase
// pattern), opaque, into both byte and f16 (straight) buffers.
static void fill_column4(uint8_t *__counted_by(4 * 4 * 4) b8,
                         _Float16 *__counted_by(4 * 4 * 4) f16) {
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            uint8_t const v = x == 1 ? 255 : 0;
            int const o = (y * 4 + x) * 4;
            b8[o + 0] = v; b8[o + 1] = v; b8[o + 2] = v; b8[o + 3] = 255;
            f16[o + 0] = (_Float16)((float)v / 255.0f);
            f16[o + 1] = (_Float16)((float)v / 255.0f);
            f16[o + 2] = (_Float16)((float)v / 255.0f);
            f16[o + 3] = (_Float16)1.0f;
        }
    }
}

// f16 NEAREST (smoothing disabled): an f16 image drawn with smoothing off
// reaches sample_src_nearest_f16; it must match the unorm8 nearest draw of the
// same colours (a clean integer pick, no interpolation -> byte-exact).
static void f16_nearest_matches_unorm8(void) {
    enum { N = 32 };
    int const len = N * N * 4;
    uint8_t px_f[N * N * 4], px_u[N * N * 4];
    uint8_t b8[4 * 4 * 4];
    _Float16 f16[4 * 4 * 4];
    fill_column4(b8, f16);
    struct canvas_image *__single imf = canvas_image_f16(CANVAS_CS_SRGB, f16, 4, 4, CANVAS_ALPHA_UNPREMUL);
    struct canvas_image *__single imu = canvas_image_unorm8(CANVAS_CS_SRGB, b8, 4, 4, CANVAS_ALPHA_UNPREMUL);
    struct canvas *__single cf = canvas(N, N);
    struct canvas *__single cu = canvas(N, N);
    CHECK(imf != NULL && imu != NULL && cf != NULL && cu != NULL);
    if (imf && imu && cf && cu) {
        canvas_set_image_smoothing_enabled(cf, false);  // -> sample_src_nearest_f16
        canvas_set_image_smoothing_enabled(cu, false);
        canvas_draw_image_scaled(cf, imf, 0.0f, 0.0f, 16.0f, 16.0f);
        canvas_draw_image_scaled(cu, imu, 0.0f, 0.0f, 16.0f, 16.0f);
        canvas_read_rgba(cf, CANVAS_CS_SRGB, px_f, len);
        canvas_read_rgba(cu, CANVAS_CS_SRGB, px_u, len);
        struct rgba const pf = pixel_at(px_f, len, N, 5, 5);
        struct rgba const pu = pixel_at(px_u, len, N, 5, 5);
        CHECK(px_near(pf, pu.r, pu.g, pu.b, pu.a, 1));
        CHECK(pf.r == 0 || pf.r == 255);  // a nearest pole, not a blend
    }
    canvas_image_free(imf);
    canvas_image_free(imu);
    if (cf) { canvas_free(cf); }
    if (cu) { canvas_free(cu); }
}

// f16 CUBIC (high-quality magnification): an f16 image magnified at high reaches
// sample_src_cubic_f16 (otherwise entirely unexercised).  Catmull-Rom's
// half-phase puts a dest centre exactly between the bright texel and its
// neighbour -> 0.5625*255 = 143, the catmullrom_phase pin; it must match the
// unorm8 cubic too.
static void f16_cubic_magnify(void) {
    enum { N = 32 };
    int const len = N * N * 4;
    uint8_t px_f[N * N * 4], px_u[N * N * 4];
    uint8_t b8[4 * 4 * 4];
    _Float16 f16[4 * 4 * 4];
    fill_column4(b8, f16);
    struct canvas_image *__single imf = canvas_image_f16(CANVAS_CS_SRGB, f16, 4, 4, CANVAS_ALPHA_UNPREMUL);
    struct canvas_image *__single imu = canvas_image_unorm8(CANVAS_CS_SRGB, b8, 4, 4, CANVAS_ALPHA_UNPREMUL);
    struct canvas *__single cf = canvas(N, N);
    struct canvas *__single cu = canvas(N, N);
    CHECK(imf != NULL && imu != NULL && cf != NULL && cu != NULL);
    if (imf && imu && cf && cu) {
        canvas_set_image_smoothing_quality(cf, CANVAS_SMOOTHING_HIGH);  // -> cubic_f16
        canvas_set_image_smoothing_quality(cu, CANVAS_SMOOTHING_HIGH);
        canvas_draw_image_scaled(cf, imf, 0.5f, 0.5f, 16.0f, 16.0f);
        canvas_draw_image_scaled(cu, imu, 0.5f, 0.5f, 16.0f, 16.0f);
        canvas_read_rgba(cf, CANVAS_CS_SRGB, px_f, len);
        canvas_read_rgba(cu, CANVAS_CS_SRGB, px_u, len);
        CHECK(px_near(pixel_at(px_f, len, N, 8, 8), 143, 143, 143, 255, 2));
        struct rgba const pu = pixel_at(px_u, len, N, 8, 8);
        CHECK(px_near(pixel_at(px_f, len, N, 8, 8), pu.r, pu.g, pu.b, pu.a, 2));
    }
    canvas_image_free(imf);
    canvas_image_free(imu);
    if (cf) { canvas_free(cf); }
    if (cu) { canvas_free(cu); }
}

// A PREMUL unorm8 image magnified through the cubic: a transparent-red /
// opaque-blue boundary stored PREMULTIPLIED (red's premul rgb is 0, alpha 0),
// upscaled at high.  The cubic premultiplies its taps either way, so as with the
// straight no_transparent_bleed case there is no ghost red -- but here the data
// arrives already premultiplied (premul_src true), the other cubic branch.
static void premul_unorm8_cubic(void) {
    enum { N = 32 };
    int const len = N * N * 4;
    uint8_t px[N * N * 4];
    uint8_t src[2 * 2 * 4];  // premultiplied bytes
    for (int y = 0; y < 2; y++) {
        int const lo = (y * 2 + 0) * 4, ro = (y * 2 + 1) * 4;
        // left: transparent red -> premul rgb all 0, a 0
        src[lo + 0] = 0; src[lo + 1] = 0; src[lo + 2] = 0; src[lo + 3] = 0;
        // right: opaque blue -> premul == straight at a==255
        src[ro + 0] = 0; src[ro + 1] = 0; src[ro + 2] = 255; src[ro + 3] = 255;
    }
    struct canvas_image *__single im = canvas_image_unorm8(CANVAS_CS_SRGB, src, 2, 2, CANVAS_ALPHA_PREMUL);
    struct canvas *__single cv = canvas(N, N);
    CHECK(im != NULL && cv != NULL);
    if (im && cv) {
        canvas_set_image_smoothing_quality(cv, CANVAS_SMOOTHING_HIGH);
        canvas_draw_image_scaled(cv, im, 0.0f, 0.0f, 16.0f, 16.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        struct rgba const p = pixel_at(px, len, N, 8, 8);
        CHECK(p.r <= 1);     // premultiplied taps: no ghost red from the clear texel
        CHECK(p.b >= 100);   // blue carries through the boundary
    }
    canvas_image_free(im);
    if (cv) { canvas_free(cv); }
}

int main(void) {
    flat_exact();
    stripes_average();
    catmullrom_phase();
    no_transparent_bleed();
    f16_nearest_matches_unorm8();
    f16_cubic_magnify();
    premul_unorm8_cubic();
    return TEST_REPORT();
}
