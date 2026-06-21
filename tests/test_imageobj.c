// The reified image API: canvas2d_image / canvas2d_snapshot construction,
// explicit canvas2d_image_build_mips (cached chain == the bitmap path's
// per-draw chain, byte for byte; without it, minification falls back to
// bilinear -- the documented explicit-cost contract), snapshot fidelity
// (premultiplied quantize, no unpremultiply round trip), and the pimage /
// image_mips record/replay round trip.

#include "canvas2d.h"
#include "canvas2d_image.h"
#include "test_pixels.h"
#include "test_util.h"

#include <ptrcheck.h>
#include <stdio.h>
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
    struct canvas2d_image *__single img = canvas2d_image_unorm8(CANVAS2D_CS_SRGB, src, N, N, CANVAS2D_ALPHA_UNPREMUL);
    struct canvas2d_context *__single a = canvas2d(N, N, CANVAS2D_CS_SRGB);
    struct canvas2d_context *__single b = canvas2d(N, N, CANVAS2D_CS_SRGB);
    CHECK(img != NULL && a != NULL && b != NULL);
    if (img && a && b) {
        CHECK(canvas2d_image_width(img) == N && canvas2d_image_height(img) == N);
        CHECK(canvas2d_image_build_mips(img));
        CHECK(canvas2d_image_build_mips(img));  // idempotent

        canvas2d_draw_bitmap(a, CANVAS2D_CS_SRGB, src, N, N, 0.0f, 0.0f);
        canvas2d_draw_image(b, img, 0.0f, 0.0f);
        canvas2d_read_rgba(a, CANVAS2D_CS_SRGB, pa, len);
        canvas2d_read_rgba(b, CANVAS2D_CS_SRGB, pb, len);
        CHECK(memcmp(pa, pb, (size_t)len) == 0);  // 1:1

        canvas2d_clear_rect(a, 0.0f, 0.0f, (float)N, (float)N);
        canvas2d_clear_rect(b, 0.0f, 0.0f, (float)N, (float)N);
        canvas2d_set_image_smoothing_quality(a, CANVAS2D_SMOOTHING_MEDIUM);
        canvas2d_set_image_smoothing_quality(b, CANVAS2D_SMOOTHING_MEDIUM);
        canvas2d_draw_bitmap_scaled(a, CANVAS2D_CS_SRGB, src, N, N, 2.0f, 2.0f, 8.0f, 8.0f);
        canvas2d_draw_image_scaled(b, img, 2.0f, 2.0f, 8.0f, 8.0f);
        canvas2d_read_rgba(a, CANVAS2D_CS_SRGB, pa, len);
        canvas2d_read_rgba(b, CANVAS2D_CS_SRGB, pb, len);
        CHECK(memcmp(pa, pb, (size_t)len) == 0);  // 4x minify, shared chain
    }
    canvas2d_image_free(img);
    if (a) { canvas2d_free(a); }
    if (b) { canvas2d_free(b); }
}

// Without build_mips a minifying image draw at medium is plain bilinear --
// exactly the bitmap path at LOW -- not a hidden chain rebuild.
static void no_mips_falls_back_bilinear(void) {
    int const len = N * N * 4;
    uint8_t src[N * N * 4];
    uint8_t pa[N * N * 4];
    uint8_t pb[N * N * 4];
    fill_src(src);
    struct canvas2d_image *__single img = canvas2d_image_unorm8(CANVAS2D_CS_SRGB, src, N, N, CANVAS2D_ALPHA_UNPREMUL);
    struct canvas2d_context *__single a = canvas2d(N, N, CANVAS2D_CS_SRGB);
    struct canvas2d_context *__single b = canvas2d(N, N, CANVAS2D_CS_SRGB);
    CHECK(img != NULL && a != NULL && b != NULL);
    if (img && a && b) {
        canvas2d_set_image_smoothing_quality(a, CANVAS2D_SMOOTHING_LOW);
        canvas2d_set_image_smoothing_quality(b, CANVAS2D_SMOOTHING_MEDIUM);
        canvas2d_draw_bitmap_scaled(a, CANVAS2D_CS_SRGB, src, N, N, 2.0f, 2.0f, 8.0f, 8.0f);
        canvas2d_draw_image_scaled(b, img, 2.0f, 2.0f, 8.0f, 8.0f);  // mip-less
        canvas2d_read_rgba(a, CANVAS2D_CS_SRGB, pa, len);
        canvas2d_read_rgba(b, CANVAS2D_CS_SRGB, pb, len);
        CHECK(memcmp(pa, pb, (size_t)len) == 0);
    }
    canvas2d_image_free(img);
    if (a) { canvas2d_free(a); }
    if (b) { canvas2d_free(b); }
}

// canvas2d_snapshot: a 1:1 draw of the snapshot reproduces the source canvas
// EXACTLY, translucency included -- the snapshot is the premultiplied f16
// surface memcpy'd, so there is no quantize anywhere in the round trip.
static void snapshot_roundtrip(void) {
    int const len = N * N * 4;
    uint8_t pa[N * N * 4];
    uint8_t pb[N * N * 4];
    struct canvas2d_context *__single a = canvas2d(N, N, CANVAS2D_CS_SRGB);
    struct canvas2d_context *__single b = canvas2d(N, N, CANVAS2D_CS_SRGB);
    CHECK(a != NULL && b != NULL);
    if (a && b) {
        canvas2d_set_fill_rgba(a, CANVAS2D_CS_SRGB, 0.8f, 0.3f, 0.2f, 1.0f);
        canvas2d_fill_rect(a, 0.0f, 0.0f, 16.0f, 32.0f);
        canvas2d_set_fill_rgba(a, CANVAS2D_CS_SRGB, 0.1f, 0.5f, 0.9f, 0.5f);
        canvas2d_fill_rect(a, 16.0f, 0.0f, 16.0f, 32.0f);
        struct canvas2d_image *__single snap = canvas2d_snapshot(a);
        CHECK(snap != NULL);
        if (snap) {
            canvas2d_draw_image(b, snap, 0.0f, 0.0f);
            canvas2d_read_rgba(a, CANVAS2D_CS_SRGB, pa, len);
            canvas2d_read_rgba(b, CANVAS2D_CS_SRGB, pb, len);
            CHECK(px_near(pixel_at(pb, len, N, 4, 4), pa[(4 * N + 4) * 4 + 0],
                          pa[(4 * N + 4) * 4 + 1], pa[(4 * N + 4) * 4 + 2],
                          pa[(4 * N + 4) * 4 + 3], 0));  // opaque: exact
            CHECK(px_near(pixel_at(pb, len, N, 24, 4), pa[(4 * N + 24) * 4 + 0],
                          pa[(4 * N + 24) * 4 + 1], pa[(4 * N + 24) * 4 + 2],
                          pa[(4 * N + 24) * 4 + 3], 0));  // translucent: exact
            canvas2d_image_free(snap);
        }
        canvas2d_free(a);
        canvas2d_free(b);
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
    struct canvas2d_context *__single content = canvas2d(N, N, CANVAS2D_CS_SRGB);
    struct canvas2d_context *__single a = canvas2d(N, N, CANVAS2D_CS_SRGB);
    struct canvas2d_context *__single b = canvas2d(N, N, CANVAS2D_CS_SRGB);
    CHECK(content != NULL && a != NULL && b != NULL);
    if (content && a && b) {
        canvas2d_set_fill_rgba(content, CANVAS2D_CS_SRGB, 0.2f, 0.7f, 0.4f, 0.8f);
        canvas2d_fill_rect(content, 4.0f, 4.0f, 24.0f, 24.0f);
        struct canvas2d_image *__single snap = canvas2d_snapshot(content);
        struct canvas2d_image *__single img = canvas2d_image_unorm8(CANVAS2D_CS_SRGB, src, N, N, CANVAS2D_ALPHA_UNPREMUL);
        CHECK(snap != NULL && img != NULL);
        if (snap && img) {
            CHECK(canvas2d_image_build_mips(snap));
            CHECK(canvas2d_record_to(a, path));
            canvas2d_set_image_smoothing_quality(a, CANVAS2D_SMOOTHING_MEDIUM);
            canvas2d_draw_image_scaled(a, snap, 0.0f, 0.0f, 8.0f, 8.0f);  // mips
            canvas2d_draw_image_scaled(a, img, 8.0f, 8.0f, 8.0f, 8.0f);  // mip-less
            canvas2d_read_rgba(a, CANVAS2D_CS_SRGB, pa, len);
            canvas2d_free(a);  // flush + close
            a = NULL;

            CHECK(canvas2d_replay_from(b, path));
            canvas2d_read_rgba(b, CANVAS2D_CS_SRGB, pb, len);
            CHECK(memcmp(pa, pb, (size_t)len) == 0);
        }
        canvas2d_image_free(snap);
        canvas2d_image_free(img);
    }
    if (content) { canvas2d_free(content); }
    if (a) { canvas2d_free(a); }
    if (b) { canvas2d_free(b); }
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
    struct canvas2d_image *__single un =
        canvas2d_image_f16(CANVAS2D_CS_SRGB, src, N, N, CANVAS2D_ALPHA_UNPREMUL);
    struct canvas2d_context *__single cv = canvas2d(N, N, CANVAS2D_CS_SRGB);
    CHECK(un != NULL && cv != NULL);
    if (un && cv) {
        canvas2d_draw_image(cv, un, 0.0f, 0.0f);
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 8, 8), 255, 0, 0, 128, 0));
    }
    canvas2d_image_free(un);

    // Premultiplied f16: the same colour spelled premultiplied.
    for (int i = 0; i < N * N; i++) {
        src[i * 4 + 0] = (_Float16)0.5f;
    }
    struct canvas2d_image *__single pm =
        canvas2d_image_f16(CANVAS2D_CS_SRGB, src, N, N, CANVAS2D_ALPHA_PREMUL);
    CHECK(pm != NULL);
    if (pm && cv) {
        CHECK(canvas2d_image_build_mips(pm));
        canvas2d_clear_rect(cv, 0.0f, 0.0f, (float)N, (float)N);
        canvas2d_set_image_smoothing_quality(cv, CANVAS2D_SMOOTHING_MEDIUM);
        canvas2d_draw_image_scaled(cv, pm, 4.0f, 4.0f, 8.0f, 8.0f);  // 4x minify
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 8, 8), 255, 0, 0, 128, 0));
    }
    canvas2d_image_free(pm);
    if (cv) { canvas2d_free(cv); }
}

// Read up to cap bytes of `path` into buf; byte count, or -1 if it won't open.
// (The recorded programs here are a few KB of deflated `bits`, well under cap.)
static int slurp(char const *__null_terminated path, char *__counted_by(cap) buf,
                 int cap) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    size_t const got = fread(buf, 1, (size_t)cap, f);
    (void)fclose(f);
    return (int)got;
}

// Whether `needle` occurs anywhere in buf[0,len) -- index-walked so the
// __counted_by buffer never crosses into __null_terminated pointer land.
static bool contains(char const *__counted_by(len) buf, int len,
                     char const *__null_terminated needle) {
    for (int i = 0; i < len; i++) {
        int k = 0;
        char const *__null_terminated p = needle;
        bool match = true;
        while (*p != '\0') {
            if (i + k >= len || buf[i + k] != *p) {
                match = false;
                break;
            }
            p++;
            k++;
        }
        if (match) {
            return true;
        }
    }
    return false;
}

// The colour-space tag is interpretation metadata that the sampler does NOT
// yet honour (the LINEAR-WORKING-SPACE deferral in canvas.c's
// draw_image_quad), so there is no pixel effect to assert -- this test pins
// the TAG'S PLUMBING AND SERIALIZATION only:
//   1. A CANVAS2D_CS_LINEAR_SRGB-tagged image records with the optional `linear`
//      token on its `image` block line, and the file replays cleanly (the
//      parser accepts and threads the tag).
//   2. A CANVAS2D_CS_SRGB image records WITHOUT any space token -- absence ==
//      sRGB, so every existing sRGB program stays byte-identical.
//   3. Record -> replay -> re-record of the linear-tagged image is
//      byte-idempotent: the tag survives a full round trip intact.
// When the sampler honours the tag (the deferred mip-pyramid-and-taps work),
// add the pixel assertion here; today there is nothing to measure.
static void colorspace_tag_serialization(void) {
    char const *__null_terminated lin_path = "build/test_imageobj_lin.canvas";
    char const *__null_terminated srgb_path = "build/test_imageobj_srgb.canvas";
    enum { CAP = 1 << 16 };  // a one-image program is a few KB of deflated bits
    static char lin_text[CAP], srgb_text[CAP];
    uint8_t src[N * N * 4];
    fill_src(src);

    // (1) A linear-tagged image records the `linear` token; (2) an sRGB image
    // records no token at all.  Both replay cleanly.
    struct canvas2d_context *__single lin = canvas2d(N, N, CANVAS2D_CS_SRGB);
    struct canvas2d_context *__single srgb = canvas2d(N, N, CANVAS2D_CS_SRGB);
    CHECK(lin != NULL && srgb != NULL);
    if (lin && srgb) {
        struct canvas2d_image *__single img_lin =
            canvas2d_image_unorm8(CANVAS2D_CS_LINEAR_SRGB, src, N, N,
                                CANVAS2D_ALPHA_UNPREMUL);
        struct canvas2d_image *__single img_srgb =
            canvas2d_image_unorm8(CANVAS2D_CS_SRGB, src, N, N, CANVAS2D_ALPHA_UNPREMUL);
        CHECK(img_lin != NULL && img_srgb != NULL);
        if (img_lin && img_srgb) {
            CHECK(canvas2d_record_to(lin, lin_path));
            canvas2d_draw_image(lin, img_lin, 0.0f, 0.0f);
            canvas2d_free(lin);  // flush + close
            lin = NULL;

            CHECK(canvas2d_record_to(srgb, srgb_path));
            canvas2d_draw_image(srgb, img_srgb, 0.0f, 0.0f);
            canvas2d_free(srgb);  // flush + close
            srgb = NULL;

            int const ln = slurp(lin_path, lin_text, CAP);
            int const sn = slurp(srgb_path, srgb_text, CAP);
            CHECK(ln > 0 && ln < CAP && sn > 0 && sn < CAP);
            if (ln > 0 && ln < CAP && sn > 0 && sn < CAP) {
                // The linear file's image block names its space; the sRGB file
                // carries no colour-space token anywhere (absence == sRGB).
                // (The image block leads the file, so no `\n` prefix.)
                CHECK(contains(lin_text, ln, "image 0 unorm8 unpremul"));
                CHECK(contains(lin_text, ln, " linear\n"));
                CHECK(contains(srgb_text, sn, "image 0 unorm8 unpremul"));
                CHECK(!contains(srgb_text, sn, " linear\n"));
                CHECK(!contains(srgb_text, sn, " oklab\n"));
            }

            // The linear file replays without error: the parser accepts the
            // optional token and threads the tag onto the rebuilt block.
            struct canvas2d_context *__single rb = canvas2d(N, N, CANVAS2D_CS_SRGB);
            CHECK(rb != NULL);
            if (rb) {
                CHECK(canvas2d_replay_from(rb, lin_path));
                canvas2d_free(rb);
            }
        }
        canvas2d_image_free(img_lin);
        canvas2d_image_free(img_srgb);
    }
    if (lin) { canvas2d_free(lin); }
    if (srgb) { canvas2d_free(srgb); }

    // (3) Replay the linear file onto a RECORDING canvas: the re-recorded file
    // is byte-identical to the original, so the tag survived record -> replay
    // -> re-record intact (the round-trip idempotence the format guarantees).
    char const *__null_terminated re_path = "build/test_imageobj_lin_re.canvas";
    static char re_text[CAP];
    struct canvas2d_context *__single re = canvas2d(N, N, CANVAS2D_CS_SRGB);
    CHECK(re != NULL);
    if (re) {
        CHECK(canvas2d_record_to(re, re_path));
        CHECK(canvas2d_replay_from(re, lin_path));
        canvas2d_free(re);  // flush + close

        int const an = slurp(lin_path, lin_text, CAP);
        int const bn = slurp(re_path, re_text, CAP);
        CHECK(an > 0 && an == bn);  // same length...
        if (an > 0 && an == bn) {
            CHECK(memcmp(lin_text, re_text, (size_t)an) == 0);  // ...and bytes
        }
    }
}

int main(void) {
    image_matches_bitmap();
    no_mips_falls_back_bilinear();
    snapshot_roundtrip();
    record_replay_roundtrip();
    f16_formats();
    colorspace_tag_serialization();
    return TEST_REPORT();
}
