// The CSS filter functions: hand-computed expectations for each colour kernel
// (premultiplied forms, Filter Effects matrices), list order, state plumbing
// (save/restore/reset/set_filter_none), spec clamping, that the list reaches
// every painted op (fills, images, text) but not put_image_data -- and blur(),
// held to a brute-force three-pass box reference over a float RGBA tile.

#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W 16

// --- blur() ------------------------------------------------------------------

// O(w*h*r) brute-force box passes over a premultiplied float RGBA tile,
// out-of-tile samples transparent black -- the oracle for the f16 kernel and
// its pipeline plumbing (bbox expansion, tile inset, readback).
static void ref_box_h(float *__counted_by(w * h * 4) dst,
                      float const *__counted_by(w * h * 4) src,
                      int w, int h, int r) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            for (int c = 0; c < 4; c++) {
                float s = 0.0f;
                for (int k = -r; k <= r; k++) {
                    int xx = x + k;
                    if (xx >= 0 && xx < w) {
                        s += src[(y * w + xx) * 4 + c];
                    }
                }
                dst[(y * w + x) * 4 + c] = s / (float)(2 * r + 1);
            }
        }
    }
}

static void ref_box_v(float *__counted_by(w * h * 4) dst,
                      float const *__counted_by(w * h * 4) src,
                      int w, int h, int r) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            for (int c = 0; c < 4; c++) {
                float s = 0.0f;
                for (int k = -r; k <= r; k++) {
                    int yy = y + k;
                    if (yy >= 0 && yy < h) {
                        s += src[(yy * w + x) * 4 + c];
                    }
                }
                dst[(y * w + x) * 4 + c] = s / (float)(2 * r + 1);
            }
        }
    }
}

// Three box passes ~ the Gaussian, in place (tmp is scratch).
static void ref_blur3(float *__counted_by(w * h * 4) px,
                      float *__counted_by(w * h * 4) tmp, int w, int h, int r) {
    for (int pass = 0; pass < 3; pass++) {
        ref_box_h(tmp, px, w, h, r);
        ref_box_v(px, tmp, w, h, r);
    }
}

// The premultiplied form of contrast(2) -- rgb' = 2*rgb - 0.5*a, clamped to
// [0, a] -- applied to the reference tile (the per-function clamp is what
// makes filter order visible around a blur; see blur_order_visible).
static void ref_contrast2(float *__counted_by(n4) px, int n4) {
    for (int i = 0; i + 3 < n4; i += 4) {
        float a = px[i + 3];
        for (int c = 0; c < 3; c++) {
            float v = 2.0f * px[i + c] - 0.5f * a;
            px[i + c] = v < 0.0f ? 0.0f : (v > a ? a : v);
        }
    }
}

// Compare a canvas readback (unpremultiplied RGBA8) against a premultiplied
// float reference tile.  Alpha must match within tol everywhere; rgb is
// checked (against the unpremultiplied ratio) only where the reference alpha
// is solid enough that unpremultiplying isn't noise on both sides -- the far
// skirt's near-zero alphas quantize to 0-vs-1 coin flips that say nothing.
static void check_vs_ref(uint8_t const *__counted_by(len) px, int len,
                         float const *__counted_by(w * h * 4) ref,
                         int w, int h, int tol) {
    bool ok = true;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int i = (y * w + x) * 4;
            float ra = ref[i + 3];
            struct px4 p = pixel_at(px, len, w, x, y);
            int ea = (int)(ra * 255.0f + 0.5f);
            if (abs((int)p.a - ea) > tol) {
                ok = false;
            }
            if (ea >= 8) {
                bool rgb_ok = true;
                for (int c = 0; c < 3; c++) {
                    float u = ref[i + c] / ra;
                    u = u > 1.0f ? 1.0f : u;
                    int ec = (int)(u * 255.0f + 0.5f);
                    int pc = c == 0 ? p.r : (c == 1 ? p.g : p.b);
                    if (abs(pc - ec) > tol) {
                        rgb_ok = false;
                    }
                }
                ok = ok && rgb_ok;
            }
        }
    }
    CHECK(ok);
}

// A translucent fill through blur(3) must match the brute-force reference:
// build the unblurred premultiplied tile the fill paints, blur it three-pass
// by hand, and hold the readback to it within 2/255.
static void blur_matches_reference(void) {
    enum { N = 48 };
    int const len = N * N * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    int const nf = N * N * 4;
    float *__counted_by(nf) ref = calloc((size_t)nf, sizeof(float));
    float *__counted_by(nf) tmp = calloc((size_t)nf, sizeof(float));
    canvas *__single cv = canvas_create(N, N);
    CHECK(px != NULL && ref != NULL && tmp != NULL && cv != NULL);
    if (px && ref && tmp && cv) {
        canvas_add_filter_blur(cv, 3.0f);  // stdDev 3 -> box radius 3
        canvas_set_fill_rgba(cv, 0.2f, 0.4f, 0.8f, 0.6f);
        canvas_fill_rect(cv, 16.0f, 16.0f, 16.0f, 16.0f);
        canvas_read_rgba(cv, px, len);
        for (int y = 16; y < 32; y++) {
            for (int x = 16; x < 32; x++) {
                int i = (y * N + x) * 4;
                ref[i + 0] = 0.2f * 0.6f;
                ref[i + 1] = 0.4f * 0.6f;
                ref[i + 2] = 0.8f * 0.6f;
                ref[i + 3] = 0.6f;
            }
        }
        ref_blur3(ref, tmp, N, N, 3);
        check_vs_ref(px, len, ref, N, N, 2);
    }
    if (cv) {
        canvas_destroy(cv);
    }
    free(px);
    free(ref);
    free(tmp);
}

// blur(0) is identity (it appends nothing), and negative / non-finite px are
// ignored outright: the fill stays crisp -- opaque inside, exactly 0 one pixel
// outside.
static void blur_noop_amounts(void) {
    enum { N = 32 };
    int const len = N * N * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    canvas *__single cv = canvas_create(N, N);
    CHECK(px != NULL && cv != NULL);
    if (px && cv) {
        canvas_add_filter_blur(cv, 0.0f);
        canvas_add_filter_blur(cv, -3.0f);
        canvas_add_filter_blur(cv, NAN);
        canvas_add_filter_blur(cv, INFINITY);
        canvas_set_fill_rgba(cv, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_fill_rect(cv, 8.0f, 8.0f, 8.0f, 8.0f);
        canvas_read_rgba(cv, px, len);
        CHECK(px_near(pixel_at(px, len, N, 12, 12), 255, 0, 0, 255, 1));
        CHECK(px_near(pixel_at(px, len, N, 8, 12), 255, 0, 0, 255, 1));   // crisp edge
        CHECK(px_near(pixel_at(px, len, N, 7, 12), 0, 0, 0, 0, 0));       // no skirt
    }
    if (cv) {
        canvas_destroy(cv);
    }
    free(px);
}

// The painted region grows by the blur's spread: an opaque square through
// blur(4) reads back a soft skirt OUTSIDE its unblurred bbox, still in the
// fill colour -- and beyond the three passes' 3*r reach it is exactly 0.
static void blur_expands_bbox(void) {
    enum { N = 48 };
    int const len = N * N * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    canvas *__single cv = canvas_create(N, N);
    CHECK(px != NULL && cv != NULL);
    if (px && cv) {
        canvas_add_filter_blur(cv, 4.0f);  // box radius 4: spread 12
        canvas_set_fill_rgba(cv, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_fill_rect(cv, 20.0f, 20.0f, 8.0f, 8.0f);
        canvas_read_rgba(cv, px, len);
        struct px4 skirt = pixel_at(px, len, N, 16, 24);  // 4px left of the bbox
        CHECK(skirt.a > 5);
        CHECK(skirt.r > 0 && skirt.g == 0 && skirt.b == 0);  // still red
        // The window (2*4+1) is wider than the 8px square, so even the centre
        // is translucent -- but solidly covered, and pure red.
        struct px4 centre = pixel_at(px, len, N, 24, 24);
        CHECK(centre.a > 80 && centre.a < 200);
        CHECK(centre.r == 255 && centre.g == 0 && centre.b == 0);
        CHECK(px_near(pixel_at(px, len, N, 2, 2), 0, 0, 0, 0, 0));  // beyond the spread
        CHECK(px_near(pixel_at(px, len, N, 44, 24), 0, 0, 0, 0, 0));
    }
    if (cv) {
        canvas_destroy(cv);
    }
    free(px);
}

// A blur between colour functions applies in list order.  Colour matrices are
// linear, so they would commute with the (linear) blur if not for the spec's
// per-function clamp: contrast(2) pins pure red and pure green in place, so
// contrast-then-blur leaves a soft red-green mix at a hard gradient seam,
// while blur-then-contrast re-saturates the mixed seam.  Both orders are held
// to the brute-force reference, and the seam pixel shows them apart.
static void blur_order_visible(void) {
    enum { N = 48 };
    int const len = N * N * 4;
    uint8_t *__counted_by(len) pxa = malloc((size_t)len);
    uint8_t *__counted_by(len) pxb = malloc((size_t)len);
    int const nf = N * N * 4;
    float *__counted_by(nf) ref = calloc((size_t)nf, sizeof(float));
    float *__counted_by(nf) tmp = calloc((size_t)nf, sizeof(float));
    canvas *__single cv = canvas_create(N, N);
    CHECK(pxa != NULL && pxb != NULL && ref != NULL && tmp != NULL && cv != NULL);
    if (pxa && pxb && ref && tmp && cv) {
        for (int order = 0; order < 2; order++) {
            canvas_reset(cv);
            if (order == 0) {
                canvas_add_filter_contrast(cv, 2.0f);
                canvas_add_filter_blur(cv, 2.0f);
            } else {
                canvas_add_filter_blur(cv, 2.0f);
                canvas_add_filter_contrast(cv, 2.0f);
            }
            // A hard-stop gradient: pure red for x < 24, pure green from 24 on.
            canvas_set_fill_linear_gradient(cv, 8.0f, 24.0f, 40.0f, 24.0f);
            canvas_add_fill_color_stop(cv, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
            canvas_add_fill_color_stop(cv, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f);
            canvas_add_fill_color_stop(cv, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f);
            canvas_add_fill_color_stop(cv, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
            canvas_fill_rect(cv, 8.0f, 8.0f, 32.0f, 32.0f);
            canvas_read_rgba(cv, order == 0 ? pxa : pxb, len);
            // Reference: the unblurred premultiplied tile (contrast(2) fixes
            // both pure endpoints, so it is also the contrast-applied tile)...
            memset(ref, 0, (size_t)nf * sizeof(float));
            for (int y = 8; y < 40; y++) {
                for (int x = 8; x < 40; x++) {
                    int i = (y * N + x) * 4;
                    ref[i + (x < 24 ? 0 : 1)] = 1.0f;
                    ref[i + 3] = 1.0f;
                }
            }
            // ...then the two function applications in this order's sequence.
            if (order == 0) {
                ref_contrast2(ref, nf);
                ref_blur3(ref, tmp, N, N, 2);
            } else {
                ref_blur3(ref, tmp, N, N, 2);
                ref_contrast2(ref, nf);
            }
            check_vs_ref(order == 0 ? pxa : pxb, len, ref, N, N, 3);
        }
        // The seam pixel tells the orders apart: contrast-then-blur keeps the
        // soft green spill, blur-then-contrast crushes it back toward red.
        struct px4 a = pixel_at(pxa, len, N, 22, 24);
        struct px4 b = pixel_at(pxb, len, N, 22, 24);
        CHECK((int)a.g - (int)b.g > 20);
    }
    if (cv) {
        canvas_destroy(cv);
    }
    free(pxa);
    free(pxb);
    free(ref);
    free(tmp);
}

// A blurred translucent fill stays premultiplied-consistent: everywhere the
// alpha is meaningfully nonzero the unpremultiplied readback is still the fill
// colour (a broken kernel that let rgb outrun alpha would clamp to white or
// wash out), and the alpha never exceeds the source's.
static void blur_translucent_consistent(void) {
    enum { N = 40 };
    int const len = N * N * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    canvas *__single cv = canvas_create(N, N);
    CHECK(px != NULL && cv != NULL);
    if (px && cv) {
        canvas_add_filter_blur(cv, 2.0f);
        canvas_set_fill_rgba(cv, 0.2f, 0.4f, 0.8f, 0.5f);
        canvas_fill_rect(cv, 12.0f, 12.0f, 16.0f, 16.0f);
        canvas_read_rgba(cv, px, len);
        bool consistent = true;
        for (int y = 0; y < N; y++) {
            for (int x = 0; x < N; x++) {
                struct px4 p = pixel_at(px, len, N, x, y);
                if (p.a > 129) {  // box blur cannot exceed the source alpha
                    consistent = false;
                }
                if (p.a >= 8 &&
                    (abs((int)p.r - 51) > 6 || abs((int)p.g - 102) > 6 ||
                     abs((int)p.b - 204) > 6)) {
                    consistent = false;
                }
            }
        }
        CHECK(consistent);
    }
    if (cv) {
        canvas_destroy(cv);
    }
    free(px);
}

// blur() against a clip boundary: the soft skirt paints inside the clip and is
// masked to exactly 0 outside it -- a pixel test that runs on both backends.
static void blur_clipped(void) {
    enum { N = 48 };
    int const len = N * N * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    canvas *__single cv = canvas_create(N, N);
    CHECK(px != NULL && cv != NULL);
    if (px && cv) {
        canvas_begin_path(cv);
        canvas_rect(cv, 8.0f, 8.0f, 16.0f, 32.0f);
        canvas_clip(cv);
        canvas_add_filter_blur(cv, 3.0f);  // spread 9: the skirt spans x in [3, 29)
        canvas_set_fill_rgba(cv, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_fill_rect(cv, 12.0f, 16.0f, 8.0f, 8.0f);
        canvas_read_rgba(cv, px, len);
        CHECK(pixel_at(px, len, N, 10, 20).a > 5);                    // skirt, inside clip
        CHECK(px_near(pixel_at(px, len, N, 26, 20), 0, 0, 0, 0, 0));  // skirt, clipped away
        CHECK(px_near(pixel_at(px, len, N, 4, 20), 0, 0, 0, 0, 0));
    }
    if (cv) {
        canvas_destroy(cv);
    }
    free(px);
}

// Reset bitmap + state is the caller's job (the filter list is part of state);
// this fills the whole canvas with (r,g,b,a) through whatever filters are set
// and reads back the centre pixel, unpremultiplied RGBA8.
static struct px4 fill_and_read(canvas *__single cv, uint8_t *__counted_by(len) px,
                                int len, float r, float g, float b, float a) {
    canvas_set_fill_rgba(cv, r, g, b, a);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_read_rgba(cv, px, len);
    return pixel_at(px, len, W, 8, 8);
}

int main(void) {
    int const len = W * W * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }
    canvas *__single cv = canvas_create(W, W);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    // Each function on a solid fill, against hand-computed RGBA8.
    // brightness(0.5): red scales to half.
    canvas_add_filter_brightness(cv, 0.5f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  128, 0, 0, 255, 1));

    // brightness(2): quarter red doubles; full red clamps at the alpha.
    canvas_reset(cv);
    canvas_add_filter_brightness(cv, 2.0f);
    CHECK(px_near(fill_and_read(cv, px, len, 0.25f, 0.0f, 0.0f, 1.0f),
                  128, 0, 0, 255, 1));
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  255, 0, 0, 255, 1));

    // contrast(0.5): c' = 0.5c + 0.25, so red -> (0.75, 0.25, 0.25).
    canvas_reset(cv);
    canvas_add_filter_contrast(cv, 0.5f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  191, 64, 64, 255, 1));

    // grayscale(1): red collapses to its 0.2126 luminance.
    canvas_reset(cv);
    canvas_add_filter_grayscale(cv, 1.0f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  54, 54, 54, 255, 1));

    // saturate(0) is the same luminance projection.
    canvas_reset(cv);
    canvas_add_filter_saturate(cv, 0.0f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  54, 54, 54, 255, 1));

    // saturate(2) pushes a desaturated red away from gray: r' = 0.69685,
    // g' = b' = 0.19685 for (0.5, 0.25, 0.25).
    canvas_reset(cv);
    canvas_add_filter_saturate(cv, 2.0f);
    CHECK(px_near(fill_and_read(cv, px, len, 0.5f, 0.25f, 0.25f, 1.0f),
                  178, 50, 50, 255, 2));

    // sepia(1): red maps through the sepia matrix's first column.
    canvas_reset(cv);
    canvas_add_filter_sepia(cv, 1.0f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  100, 89, 69, 255, 1));

    // hue_rotate(pi): M = 2L - I, so red -> (2*0.2126 - 1, 0.4252, 0.4252),
    // the negative red lane clamping to 0.
    canvas_reset(cv);
    canvas_add_filter_hue_rotate(cv, (float)M_PI);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  0, 108, 108, 255, 2));

    // hue_rotate(pi/2) on red: (0, 0.2126 + 0.1427, negative -> 0).
    canvas_reset(cv);
    canvas_add_filter_hue_rotate(cv, (float)M_PI * 0.5f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  0, 91, 0, 255, 2));

    // invert(1) complements; invert(0.5) lands every colour on mid-gray.
    canvas_reset(cv);
    canvas_add_filter_invert(cv, 1.0f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  0, 255, 255, 255, 1));
    canvas_reset(cv);
    canvas_add_filter_invert(cv, 0.5f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  128, 128, 128, 255, 1));

    // opacity(0.5): colour keeps, alpha halves.
    canvas_reset(cv);
    canvas_add_filter_opacity(cv, 0.5f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  255, 0, 0, 128, 1));

    // Identity amounts: every function at its identity leaves a translucent,
    // non-primary colour untouched.
    canvas_reset(cv);
    struct px4 const base = fill_and_read(cv, px, len, 0.4f, 0.6f, 0.8f, 0.5f);
    struct {
        void (*add)(canvas *__single cv, float amount);
        float identity;
    } const id[8] = {
        { canvas_add_filter_brightness, 1.0f },
        { canvas_add_filter_contrast,   1.0f },
        { canvas_add_filter_grayscale,  0.0f },
        { canvas_add_filter_hue_rotate, 0.0f },
        { canvas_add_filter_invert,     0.0f },
        { canvas_add_filter_opacity,    1.0f },
        { canvas_add_filter_saturate,   1.0f },
        { canvas_add_filter_sepia,      0.0f },
    };
    for (int i = 0; i < 8; i++) {
        canvas_reset(cv);
        id[i].add(cv, id[i].identity);
        CHECK(px_near(fill_and_read(cv, px, len, 0.4f, 0.6f, 0.8f, 0.5f),
                      base.r, base.g, base.b, base.a, 1));
    }

    // The list applies in call order: invert then brightness darkens the
    // complement, while brightness then invert complements the darkened red --
    // different, hand-computed results.
    canvas_reset(cv);
    canvas_add_filter_invert(cv, 1.0f);
    canvas_add_filter_brightness(cv, 0.5f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  0, 128, 128, 255, 1));
    canvas_reset(cv);
    canvas_add_filter_brightness(cv, 0.5f);
    canvas_add_filter_invert(cv, 1.0f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  128, 255, 255, 255, 1));

    // Translucent fills (alpha 0.5): the premultiplied forms.  contrast's
    // offset and invert's flip both scale by alpha -- an unpremultiplied
    // formula misapplied to premultiplied pixels gets these wrong.
    canvas_reset(cv);
    canvas_add_filter_contrast(cv, 0.5f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 0.5f),
                  191, 64, 64, 128, 2));
    canvas_reset(cv);
    canvas_add_filter_invert(cv, 1.0f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 0.5f),
                  0, 255, 255, 128, 2));
    canvas_reset(cv);
    canvas_add_filter_opacity(cv, 0.5f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 0.5f),
                  255, 0, 0, 64, 2));

    // drawImage is filtered: an opaque red sprite grays out.
    canvas_reset(cv);
    canvas_add_filter_grayscale(cv, 1.0f);
    uint8_t img[16] = { 255, 0, 0, 255, 255, 0, 0, 255,
                        255, 0, 0, 255, 255, 0, 0, 255 };
    canvas_draw_image_scaled(cv, img, 2, 2, 4.0f, 4.0f, 8.0f, 8.0f);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, W, 8, 8), 54, 54, 54, 255, 2));

    // put_image_data is NOT filtered (it overwrites; not a painted op).
    canvas_reset(cv);
    canvas_add_filter_invert(cv, 1.0f);
    canvas_put_image_data(cv, img, (int)sizeof img, 2, 2, 7, 7);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, W, 8, 8), 255, 0, 0, 255, 1));

    // save/restore brackets the list: the inner invert composes with the outer
    // grayscale (red -> 0.2126 gray -> 0.7874 gray), and restore sheds it.
    canvas_reset(cv);
    canvas_add_filter_grayscale(cv, 1.0f);
    canvas_save(cv);
    canvas_add_filter_invert(cv, 1.0f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  201, 201, 201, 255, 1));
    canvas_restore(cv);
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  54, 54, 54, 255, 1));

    // set_filter_none clears the list; reset clears it too.
    canvas_set_filter_none(cv);
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  255, 0, 0, 255, 1));
    canvas_add_filter_grayscale(cv, 1.0f);
    canvas_reset(cv);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  255, 0, 0, 255, 1));

    // Clamping: negative amounts clamp to 0, the capped functions clamp to 1,
    // and non-finite amounts are ignored outright.
    canvas_reset(cv);
    canvas_add_filter_brightness(cv, -2.0f);  // == brightness(0): black
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  0, 0, 0, 255, 1));
    canvas_reset(cv);
    canvas_add_filter_grayscale(cv, 7.0f);    // == grayscale(1)
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  54, 54, 54, 255, 1));
    canvas_reset(cv);
    canvas_add_filter_opacity(cv, 3.0f);      // == opacity(1): identity
    canvas_add_filter_invert(cv, -1.0f);      // == invert(0): identity
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  255, 0, 0, 255, 1));
    canvas_reset(cv);
    canvas_add_filter_brightness(cv, NAN);          // all ignored: still red
    canvas_add_filter_grayscale(cv, INFINITY);
    canvas_add_filter_hue_rotate(cv, -INFINITY);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  255, 0, 0, 255, 1));

    canvas_destroy(cv);
    free(px);

    // Text is filtered: red glyphs gray out -- and because the filter runs on
    // the painted tile, even the antialiased edge pixels read as neutral gray
    // (r == g == b at every covered pixel).
    enum { TW = 32 };
    int const tlen = TW * TW * 4;
    uint8_t *__counted_by(tlen) tpx = malloc((size_t)tlen);
    canvas *__single tcv = canvas_create(TW, TW);
    CHECK(tpx != NULL && tcv != NULL);
    if (tpx && tcv) {
        canvas_add_filter_grayscale(tcv, 1.0f);
        canvas_set_fill_rgba(tcv, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_set_font_size(tcv, 28.0f);
        canvas_fill_text(tcv, "M", 4.0f, 26.0f);
        canvas_read_rgba(tcv, tpx, tlen);
        bool found_solid = false, all_gray = true;
        for (int y = 0; y < TW; y++) {
            for (int x = 0; x < TW; x++) {
                struct px4 p = pixel_at(tpx, tlen, TW, x, y);
                if (p.a > 128 && abs((int)p.r - 54) <= 3) {
                    found_solid = true;
                }
                if (p.a > 32 && (abs((int)p.r - (int)p.g) > 3 ||
                                 abs((int)p.g - (int)p.b) > 3)) {
                    all_gray = false;
                }
            }
        }
        CHECK(found_solid);
        CHECK(all_gray);
    }
    if (tcv) {
        canvas_destroy(tcv);
    }
    free(tpx);

    // blur(): the kernel vs its brute-force reference, the no-op amounts, the
    // bbox expansion, list-order visibility, premultiplied consistency, and
    // the clip boundary.
    blur_matches_reference();
    blur_noop_amounts();
    blur_expands_bbox();
    blur_order_visible();
    blur_translucent_consistent();
    blur_clipped();
    return TEST_REPORT();
}
