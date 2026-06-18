// The reified image API: canvas_image / canvas_snapshot construction,
// explicit canvas_image_build_mips (cached chain == the bitmap path's
// per-draw chain, byte for byte; without it, minification falls back to
// bilinear -- the documented explicit-cost contract), snapshot fidelity
// (premultiplied quantize, no unpremultiply round trip), and the pimage /
// image_mips record/replay round trip.

#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>
#include <string.h>

enum { N = 32 };

// Fill a straight-alpha test source: opaque colour ramp left half, 50%-alpha
// right half -- enough structure that samplers and quantizers can't fake it.
static void fill_src(uint8_t *__counted_by(N * N * 4) src) {
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            int const o = (y * N + x) * 4;
            src[o + 0] = (uint8_t)(x * 8);
            src[o + 1] = (uint8_t)(y * 8);
            src[o + 2] = 200;
            src[o + 3] = x < N / 2 ? 255 : 128;
        }
    }
}

// An image draw and a bitmap draw of the same straight pixels are
// byte-identical: 1:1, and minified at medium with the image's mips built
// (the cached chain IS the per-draw chain).
static void image_matches_bitmap(void) {
    int const len = N * N * 4;
    uint8_t src[N * N * 4];
    uint8_t pa[N * N * 4];
    uint8_t pb[N * N * 4];
    fill_src(src);
    struct canvas_image *__single img = canvas_image_unorm8(src, N, N, CANVAS_ALPHA_UNPREMUL);
    struct canvas *__single a = canvas(N, N);
    struct canvas *__single b = canvas(N, N);
    CHECK(img != NULL && a != NULL && b != NULL);
    if (img && a && b) {
        CHECK(canvas_image_width(img) == N && canvas_image_height(img) == N);
        CHECK(canvas_image_build_mips(img));
        CHECK(canvas_image_build_mips(img));  // idempotent

        canvas_draw_bitmap(a, src, N, N, 0.0f, 0.0f);
        canvas_draw_image(b, img, 0.0f, 0.0f);
        canvas_read_rgba(a, CANVAS_CS_SRGB, pa, len);
        canvas_read_rgba(b, CANVAS_CS_SRGB, pb, len);
        CHECK(memcmp(pa, pb, (size_t)len) == 0);  // 1:1

        canvas_clear_rect(a, 0.0f, 0.0f, (float)N, (float)N);
        canvas_clear_rect(b, 0.0f, 0.0f, (float)N, (float)N);
        canvas_set_image_smoothing_quality(a, CANVAS_SMOOTHING_MEDIUM);
        canvas_set_image_smoothing_quality(b, CANVAS_SMOOTHING_MEDIUM);
        canvas_draw_bitmap_scaled(a, src, N, N, 2.0f, 2.0f, 8.0f, 8.0f);
        canvas_draw_image_scaled(b, img, 2.0f, 2.0f, 8.0f, 8.0f);
        canvas_read_rgba(a, CANVAS_CS_SRGB, pa, len);
        canvas_read_rgba(b, CANVAS_CS_SRGB, pb, len);
        CHECK(memcmp(pa, pb, (size_t)len) == 0);  // 4x minify, shared chain
    }
    canvas_image_free(img);
    if (a) { canvas_free(a); }
    if (b) { canvas_free(b); }
}

// Without build_mips a minifying image draw at medium is plain bilinear --
// exactly the bitmap path at LOW -- not a hidden chain rebuild.
static void no_mips_falls_back_bilinear(void) {
    int const len = N * N * 4;
    uint8_t src[N * N * 4];
    uint8_t pa[N * N * 4];
    uint8_t pb[N * N * 4];
    fill_src(src);
    struct canvas_image *__single img = canvas_image_unorm8(src, N, N, CANVAS_ALPHA_UNPREMUL);
    struct canvas *__single a = canvas(N, N);
    struct canvas *__single b = canvas(N, N);
    CHECK(img != NULL && a != NULL && b != NULL);
    if (img && a && b) {
        canvas_set_image_smoothing_quality(a, CANVAS_SMOOTHING_LOW);
        canvas_set_image_smoothing_quality(b, CANVAS_SMOOTHING_MEDIUM);
        canvas_draw_bitmap_scaled(a, src, N, N, 2.0f, 2.0f, 8.0f, 8.0f);
        canvas_draw_image_scaled(b, img, 2.0f, 2.0f, 8.0f, 8.0f);  // mip-less
        canvas_read_rgba(a, CANVAS_CS_SRGB, pa, len);
        canvas_read_rgba(b, CANVAS_CS_SRGB, pb, len);
        CHECK(memcmp(pa, pb, (size_t)len) == 0);
    }
    canvas_image_free(img);
    if (a) { canvas_free(a); }
    if (b) { canvas_free(b); }
}

// canvas_snapshot: a 1:1 draw of the snapshot reproduces the source canvas
// EXACTLY, translucency included -- the snapshot is the premultiplied f16
// surface memcpy'd, so there is no quantize anywhere in the round trip.
static void snapshot_roundtrip(void) {
    int const len = N * N * 4;
    uint8_t pa[N * N * 4];
    uint8_t pb[N * N * 4];
    struct canvas *__single a = canvas(N, N);
    struct canvas *__single b = canvas(N, N);
    CHECK(a != NULL && b != NULL);
    if (a && b) {
        canvas_set_fill_rgba(a, CANVAS_CS_SRGB, 0.8f, 0.3f, 0.2f, 1.0f);
        canvas_fill_rect(a, 0.0f, 0.0f, 16.0f, 32.0f);
        canvas_set_fill_rgba(a, CANVAS_CS_SRGB, 0.1f, 0.5f, 0.9f, 0.5f);
        canvas_fill_rect(a, 16.0f, 0.0f, 16.0f, 32.0f);
        struct canvas_image *__single snap = canvas_snapshot(a);
        CHECK(snap != NULL);
        if (snap) {
            canvas_draw_image(b, snap, 0.0f, 0.0f);
            canvas_read_rgba(a, CANVAS_CS_SRGB, pa, len);
            canvas_read_rgba(b, CANVAS_CS_SRGB, pb, len);
            CHECK(px_near(pixel_at(pb, len, N, 4, 4), pa[(4 * N + 4) * 4 + 0],
                          pa[(4 * N + 4) * 4 + 1], pa[(4 * N + 4) * 4 + 2],
                          pa[(4 * N + 4) * 4 + 3], 0));  // opaque: exact
            CHECK(px_near(pixel_at(pb, len, N, 24, 4), pa[(4 * N + 24) * 4 + 0],
                          pa[(4 * N + 24) * 4 + 1], pa[(4 * N + 24) * 4 + 2],
                          pa[(4 * N + 24) * 4 + 3], 0));  // translucent: exact
            canvas_image_free(snap);
        }
        canvas_free(a);
        canvas_free(b);
    }
}

// Record image draws (a mipped snapshot and a mip-less straight image), then
// replay the program onto a fresh canvas: pixels byte-identical -- the
// pimage block, the image_mips line, and the bilinear-fallback semantics all
// survive the text format.
static void record_replay_roundtrip(void) {
    char const *__null_terminated path = "build/test_imageobj.canvas";
    int const len = N * N * 4;
    uint8_t src[N * N * 4];
    uint8_t pa[N * N * 4];
    uint8_t pb[N * N * 4];
    fill_src(src);
    struct canvas *__single content = canvas(N, N);
    struct canvas *__single a = canvas(N, N);
    struct canvas *__single b = canvas(N, N);
    CHECK(content != NULL && a != NULL && b != NULL);
    if (content && a && b) {
        canvas_set_fill_rgba(content, CANVAS_CS_SRGB, 0.2f, 0.7f, 0.4f, 0.8f);
        canvas_fill_rect(content, 4.0f, 4.0f, 24.0f, 24.0f);
        struct canvas_image *__single snap = canvas_snapshot(content);
        struct canvas_image *__single img = canvas_image_unorm8(src, N, N, CANVAS_ALPHA_UNPREMUL);
        CHECK(snap != NULL && img != NULL);
        if (snap && img) {
            CHECK(canvas_image_build_mips(snap));
            CHECK(canvas_record_to(a, path));
            canvas_set_image_smoothing_quality(a, CANVAS_SMOOTHING_MEDIUM);
            canvas_draw_image_scaled(a, snap, 0.0f, 0.0f, 8.0f, 8.0f);  // mips
            canvas_draw_image_scaled(a, img, 8.0f, 8.0f, 8.0f, 8.0f);  // mip-less
            canvas_read_rgba(a, CANVAS_CS_SRGB, pa, len);
            canvas_free(a);  // flush + close
            a = NULL;

            CHECK(canvas_replay_from(b, path));
            canvas_read_rgba(b, CANVAS_CS_SRGB, pb, len);
            CHECK(memcmp(pa, pb, (size_t)len) == 0);
        }
        canvas_image_free(snap);
        canvas_image_free(img);
    }
    if (content) { canvas_free(content); }
    if (a) { canvas_free(a); }
    if (b) { canvas_free(b); }
}

// The f16 constructors, both alpha types: known f16 values draw 1:1 to the
// expected bytes, and a premultiplied f16 image minifies through its own f16
// chain (a solid colour survives exactly).  All four formats are peers --
// this plus the unorm8 cases above covers the 2x2.
static void f16_formats(void) {
    int const len = N * N * 4;
    uint8_t px[N * N * 4];
    _Float16 src[N * N * 4];
    // Straight f16: a 50%-alpha pure red.
    for (int i = 0; i < N * N; i++) {
        src[i * 4 + 0] = (_Float16)1.0f;
        src[i * 4 + 1] = (_Float16)0.0f;
        src[i * 4 + 2] = (_Float16)0.0f;
        src[i * 4 + 3] = (_Float16)0.5f;
    }
    struct canvas_image *__single un =
        canvas_image_f16(src, N, N, CANVAS_ALPHA_UNPREMUL);
    struct canvas *__single cv = canvas(N, N);
    CHECK(un != NULL && cv != NULL);
    if (un && cv) {
        canvas_draw_image(cv, un, 0.0f, 0.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 8, 8), 255, 0, 0, 128, 0));
    }
    canvas_image_free(un);

    // Premultiplied f16: the same colour spelled premultiplied.
    for (int i = 0; i < N * N; i++) {
        src[i * 4 + 0] = (_Float16)0.5f;
    }
    struct canvas_image *__single pm =
        canvas_image_f16(src, N, N, CANVAS_ALPHA_PREMUL);
    CHECK(pm != NULL);
    if (pm && cv) {
        CHECK(canvas_image_build_mips(pm));
        canvas_clear_rect(cv, 0.0f, 0.0f, (float)N, (float)N);
        canvas_set_image_smoothing_quality(cv, CANVAS_SMOOTHING_MEDIUM);
        canvas_draw_image_scaled(cv, pm, 4.0f, 4.0f, 8.0f, 8.0f);  // 4x minify
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 8, 8), 255, 0, 0, 128, 0));
    }
    canvas_image_free(pm);
    if (cv) { canvas_free(cv); }
}

int main(void) {
    image_matches_bitmap();
    no_mips_falls_back_bilinear();
    snapshot_roundtrip();
    record_replay_roundtrip();
    f16_formats();
    return TEST_REPORT();
}
