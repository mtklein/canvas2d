// The CSS filter functions: hand-computed expectations for each colour kernel
// (premultiplied forms, Filter Effects matrices), list order, state plumbing
// (save/restore/reset/set_filter_none), spec clamping, that the list reaches
// every painted op (fills, images, text) but not put_image_data -- blur(),
// held to a brute-force three-pass box reference over a float RGBA tile --
// and drop-shadow(), held to the same reference machinery composed with the
// offset-tint-undercomposite by hand.

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
                    int const xx = x + k;
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
                    int const yy = y + k;
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
        float const a = px[i + 3];
        for (int c = 0; c < 3; c++) {
            float const v = 2.0f * px[i + c] - 0.5f * a;
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
                         float const *__counted_by(w * h * 4) want,
                         int w, int h, int tol) {
    bool ok = true;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int const i = (y * w + x) * 4;
            float const ra = want[i + 3];
            struct rgba p = pixel_at(px, len, w, x, y);
            int const ea = (int)(ra * 255.0f + 0.5f);
            if (abs((int)p.a - ea) > tol) {
                ok = false;
            }
            if (ea >= 8) {
                bool rgb_ok = true;
                for (int c = 0; c < 3; c++) {
                    float u = want[i + c] / ra;
                    u = u > 1.0f ? 1.0f : u;
                    int const ec = (int)(u * 255.0f + 0.5f);
                    int const pc = c == 0 ? p.r : (c == 1 ? p.g : p.b);
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
    float *__counted_by(nf) want = calloc((size_t)nf, sizeof(float));
    float *__counted_by(nf) tmp = calloc((size_t)nf, sizeof(float));
    struct canvas *__single cv = canvas(N, N, CANVAS_CS_SRGB);
    CHECK(px != NULL && want != NULL && tmp != NULL && cv != NULL);
    if (px && want && tmp && cv) {
        canvas_add_filter_blur(cv, 3.0f);  // stdDev 3 -> box radius 3
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.2f, 0.4f, 0.8f, 0.6f);
        canvas_fill_rect(cv, 16.0f, 16.0f, 16.0f, 16.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        for (int y = 16; y < 32; y++) {
            for (int x = 16; x < 32; x++) {
                int const i = (y * N + x) * 4;
                want[i + 0] = 0.2f * 0.6f;
                want[i + 1] = 0.4f * 0.6f;
                want[i + 2] = 0.8f * 0.6f;
                want[i + 3] = 0.6f;
            }
        }
        ref_blur3(want, tmp, N, N, 3);
        check_vs_ref(px, len, want, N, N, 2);
    }
    if (cv) {
        canvas_free(cv);
    }
    free(px);
    free(want);
    free(tmp);
}

// blur(0) is identity (it appends nothing), and negative / non-finite px are
// ignored outright: the fill stays crisp -- opaque inside, exactly 0 one pixel
// outside.
static void blur_noop_amounts(void) {
    enum { N = 32 };
    int const len = N * N * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    struct canvas *__single cv = canvas(N, N, CANVAS_CS_SRGB);
    CHECK(px != NULL && cv != NULL);
    if (px && cv) {
        canvas_add_filter_blur(cv, 0.0f);
        canvas_add_filter_blur(cv, -3.0f);
        canvas_add_filter_blur(cv, NAN);
        canvas_add_filter_blur(cv, INFINITY);
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_fill_rect(cv, 8.0f, 8.0f, 8.0f, 8.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 12, 12), 255, 0, 0, 255, 1));
        CHECK(px_near(pixel_at(px, len, N, 8, 12), 255, 0, 0, 255, 1));   // crisp edge
        CHECK(px_near(pixel_at(px, len, N, 7, 12), 0, 0, 0, 0, 0));       // no skirt
    }
    if (cv) {
        canvas_free(cv);
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
    struct canvas *__single cv = canvas(N, N, CANVAS_CS_SRGB);
    CHECK(px != NULL && cv != NULL);
    if (px && cv) {
        canvas_add_filter_blur(cv, 4.0f);  // box radius 4: spread 12
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_fill_rect(cv, 20.0f, 20.0f, 8.0f, 8.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        struct rgba skirt = pixel_at(px, len, N, 16, 24);  // 4px left of the bbox
        CHECK(skirt.a > 5);
        CHECK(skirt.r > 0 && skirt.g == 0 && skirt.b == 0);  // still red
        // The window (2*4+1) is wider than the 8px square, so even the centre
        // is translucent -- but solidly covered, and pure red.
        struct rgba centre = pixel_at(px, len, N, 24, 24);
        CHECK(centre.a > 80 && centre.a < 200);
        CHECK(centre.r == 255 && centre.g == 0 && centre.b == 0);
        CHECK(px_near(pixel_at(px, len, N, 2, 2), 0, 0, 0, 0, 0));  // beyond the spread
        CHECK(px_near(pixel_at(px, len, N, 44, 24), 0, 0, 0, 0, 0));
    }
    if (cv) {
        canvas_free(cv);
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
    float *__counted_by(nf) want = calloc((size_t)nf, sizeof(float));
    float *__counted_by(nf) tmp = calloc((size_t)nf, sizeof(float));
    struct canvas *__single cv = canvas(N, N, CANVAS_CS_SRGB);
    CHECK(pxa != NULL && pxb != NULL && want != NULL && tmp != NULL && cv != NULL);
    if (pxa && pxb && want && tmp && cv) {
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
            canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
            canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f);
            canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f);
            canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
            canvas_fill_rect(cv, 8.0f, 8.0f, 32.0f, 32.0f);
            canvas_read_rgba(cv, CANVAS_CS_SRGB, order == 0 ? pxa : pxb, len);
            // Reference: the unblurred premultiplied tile (contrast(2) fixes
            // both pure endpoints, so it is also the contrast-applied tile)...
            memset(want, 0, (size_t)nf * sizeof(float));
            for (int y = 8; y < 40; y++) {
                for (int x = 8; x < 40; x++) {
                    int const i = (y * N + x) * 4;
                    want[i + (x < 24 ? 0 : 1)] = 1.0f;
                    want[i + 3] = 1.0f;
                }
            }
            // ...then the two function applications in this order's sequence.
            if (order == 0) {
                ref_contrast2(want, nf);
                ref_blur3(want, tmp, N, N, 2);
            } else {
                ref_blur3(want, tmp, N, N, 2);
                ref_contrast2(want, nf);
            }
            check_vs_ref(order == 0 ? pxa : pxb, len, want, N, N, 3);
        }
        // The seam pixel tells the orders apart: contrast-then-blur keeps the
        // soft green spill, blur-then-contrast crushes it back toward red.
        struct rgba a = pixel_at(pxa, len, N, 22, 24);
        struct rgba b = pixel_at(pxb, len, N, 22, 24);
        CHECK((int)a.g - (int)b.g > 20);
    }
    if (cv) {
        canvas_free(cv);
    }
    free(pxa);
    free(pxb);
    free(want);
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
    struct canvas *__single cv = canvas(N, N, CANVAS_CS_SRGB);
    CHECK(px != NULL && cv != NULL);
    if (px && cv) {
        canvas_add_filter_blur(cv, 2.0f);
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.2f, 0.4f, 0.8f, 0.5f);
        canvas_fill_rect(cv, 12.0f, 12.0f, 16.0f, 16.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        bool consistent = true;
        for (int y = 0; y < N; y++) {
            for (int x = 0; x < N; x++) {
                struct rgba p = pixel_at(px, len, N, x, y);
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
        canvas_free(cv);
    }
    free(px);
}

// blur() against a clip boundary: the soft skirt paints inside the clip and is
// masked to exactly 0 outside it -- a pixel test that runs on both backends.
static void blur_clipped(void) {
    enum { N = 48 };
    int const len = N * N * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    struct canvas *__single cv = canvas(N, N, CANVAS_CS_SRGB);
    CHECK(px != NULL && cv != NULL);
    if (px && cv) {
        canvas_begin_path(cv);
        canvas_rect(cv, 8.0f, 8.0f, 16.0f, 32.0f);
        canvas_clip(cv, CANVAS_NONZERO);
        canvas_add_filter_blur(cv, 3.0f);  // spread 9: the skirt spans x in [3, 29)
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_fill_rect(cv, 12.0f, 16.0f, 8.0f, 8.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(pixel_at(px, len, N, 10, 20).a > 5);                    // skirt, inside clip
        CHECK(px_near(pixel_at(px, len, N, 26, 20), 0, 0, 0, 0, 0));  // skirt, clipped away
        CHECK(px_near(pixel_at(px, len, N, 4, 20), 0, 0, 0, 0, 0));
    }
    if (cv) {
        canvas_free(cv);
    }
    free(px);
}

// --- drop-shadow() -------------------------------------------------------------

// The premultiplied drop-shadow reference over a float RGBA tile: the shadow
// is the tile's alpha read at -(dx, dy) (out-of-tile reads 0), scaled by the
// premultiplied tint, blurred three-pass; the result is the original tile
// composited source-over ON TOP of that shadow.  sh and tmp are scratch.
static void ref_drop_shadow(float *__counted_by(w * h * 4) px,
                            float *__counted_by(w * h * 4) sh,
                            float *__counted_by(w * h * 4) tmp,
                            int w, int h, int dx, int dy, int r,
                            float cr, float cg, float cb, float ca) {
    float const tint[4] = { cr * ca, cg * ca, cb * ca, ca };
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sx = x - dx, sy = y - dy;
            float a = sx >= 0 && sx < w && sy >= 0 && sy < h
                          ? px[(sy * w + sx) * 4 + 3]
                          : 0.0f;
            for (int c = 0; c < 4; c++) {
                sh[(y * w + x) * 4 + c] = tint[c] * a;
            }
        }
    }
    if (r > 0) {
        ref_blur3(sh, tmp, w, h, r);
    }
    for (int i = 0; i < w * h * 4; i += 4) {
        float const k = 1.0f - px[i + 3];
        for (int c = 0; c < 4; c++) {
            px[i + c] += sh[i + c] * k;
        }
    }
}

// Hand-computed sharp case: an opaque blue square through
// drop-shadow(4, 4, 0, red): exactly red where only the shadow lands, the
// drawing's own pixels untouched where it covers, exact 0 outside both.
static void drop_shadow_hard_offset(void) {
    enum { N = 32 };
    int const len = N * N * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    struct canvas *__single cv = canvas(N, N, CANVAS_CS_SRGB);
    CHECK(px != NULL && cv != NULL);
    if (px && cv) {
        canvas_add_filter_drop_shadow(cv, CANVAS_CS_SRGB, 4.0f, 4.0f, 0.0f,
                                      1.0f, 0.0f, 0.0f, 1.0f);
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.0f, 0.0f, 1.0f, 1.0f);
        canvas_fill_rect(cv, 8.0f, 8.0f, 8.0f, 8.0f);  // square [8,16); shadow [12,20)
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 18, 18), 255, 0, 0, 255, 0));  // shadow only
        CHECK(px_near(pixel_at(px, len, N, 19, 13), 255, 0, 0, 255, 0));
        CHECK(px_near(pixel_at(px, len, N, 10, 10), 0, 0, 255, 255, 0));  // drawing only
        CHECK(px_near(pixel_at(px, len, N, 14, 14), 0, 0, 255, 255, 0));  // overlap: opaque on top
        CHECK(px_near(pixel_at(px, len, N, 5, 5), 0, 0, 0, 0, 0));        // outside both
        CHECK(px_near(pixel_at(px, len, N, 20, 20), 0, 0, 0, 0, 0));      // past the shadow
        CHECK(px_near(pixel_at(px, len, N, 18, 10), 0, 0, 0, 0, 0));      // offset is 2D
    }
    if (cv) {
        canvas_free(cv);
    }
    free(px);
}

// Subpixel offset: drop-shadow(4.5, 4, 0, red) reads the source bilinearly,
// so the shadow's leading and trailing columns land at half strength where a
// whole-pixel snap would put 0 or 255.
static void drop_shadow_subpixel_offset(void) {
    enum { N = 32 };
    int const len = N * N * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    struct canvas *__single cv = canvas(N, N, CANVAS_CS_SRGB);
    CHECK(px != NULL && cv != NULL);
    if (px && cv) {
        canvas_add_filter_drop_shadow(cv, CANVAS_CS_SRGB, 4.5f, 4.0f, 0.0f,
                                      1.0f, 0.0f, 0.0f, 1.0f);
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.0f, 0.0f, 1.0f, 1.0f);
        canvas_fill_rect(cv, 8.0f, 8.0f, 8.0f, 8.0f);  // square [8,16); shadow x [12.5,20.5)
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 12, 18), 255, 0, 0, 128, 1));  // leading half
        CHECK(px_near(pixel_at(px, len, N, 18, 18), 255, 0, 0, 255, 1));  // interior
        CHECK(px_near(pixel_at(px, len, N, 20, 18), 255, 0, 0, 128, 1));  // trailing half
        CHECK(px_near(pixel_at(px, len, N, 21, 18), 0, 0, 0, 0, 1));      // past it
    }
    if (cv) {
        canvas_free(cv);
    }
    free(px);
}

// A translucent fill through drop-shadow(5, 3, 2, translucent tint) must match
// the brute-force reference: blur skirt softness (the blur() machinery), the
// tint scaling both shadow rgb and alpha, the shadow alpha proportional to the
// source's, and the under-composite -- all in one premultiplied oracle.
static void drop_shadow_matches_reference(void) {
    enum { N = 48 };
    int const len = N * N * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    int const nf = N * N * 4;
    float *__counted_by(nf) want = calloc((size_t)nf, sizeof(float));
    float *__counted_by(nf) sh = calloc((size_t)nf, sizeof(float));
    float *__counted_by(nf) tmp = calloc((size_t)nf, sizeof(float));
    struct canvas *__single cv = canvas(N, N, CANVAS_CS_SRGB);
    CHECK(px != NULL && want != NULL && sh != NULL && tmp != NULL && cv != NULL);
    if (px && want && sh && tmp && cv) {
        canvas_add_filter_drop_shadow(cv, CANVAS_CS_SRGB, 5.0f, 3.0f, 2.0f,
                                      0.1f, 0.3f, 0.9f, 0.8f);
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.2f, 0.4f, 0.8f, 0.6f);
        canvas_fill_rect(cv, 16.0f, 16.0f, 16.0f, 16.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        for (int y = 16; y < 32; y++) {
            for (int x = 16; x < 32; x++) {
                int const i = (y * N + x) * 4;
                want[i + 0] = 0.2f * 0.6f;
                want[i + 1] = 0.4f * 0.6f;
                want[i + 2] = 0.8f * 0.6f;
                want[i + 3] = 0.6f;
            }
        }
        ref_drop_shadow(want, sh, tmp, N, N, 5, 3, 2, 0.1f, 0.3f, 0.9f, 0.8f);
        check_vs_ref(px, len, want, N, N, 2);
    }
    if (cv) {
        canvas_free(cv);
    }
    free(px);
    free(want);
    free(sh);
    free(tmp);
}

// A translucent shadow colour scales both the shadow's rgb (premultiplied) and
// its alpha: an opaque source through drop-shadow(8, 0, 0, magenta at 0.5)
// reads back magenta at alpha 128 where only the shadow lands.
static void drop_shadow_tint_translucent(void) {
    enum { N = 32 };
    int const len = N * N * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    struct canvas *__single cv = canvas(N, N, CANVAS_CS_SRGB);
    CHECK(px != NULL && cv != NULL);
    if (px && cv) {
        canvas_add_filter_drop_shadow(cv, CANVAS_CS_SRGB, 8.0f, 0.0f, 0.0f,
                                      1.0f, 0.0f, 1.0f, 0.5f);
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.0f, 1.0f);
        canvas_fill_rect(cv, 8.0f, 8.0f, 8.0f, 8.0f);  // shadow x in [16,24)
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 20, 12), 255, 0, 255, 128, 1));
        CHECK(px_near(pixel_at(px, len, N, 12, 12), 0, 255, 0, 255, 0));
    }
    if (cv) {
        canvas_free(cv);
    }
    free(px);
}

// The shadow's alpha is proportional to the source's (premul consistency): a
// half-alpha source casts a half-alpha shadow, and where the translucent
// drawing overlaps its own shadow the under-composite shows through it.
static void drop_shadow_translucent_source(void) {
    enum { N = 32 };
    int const len = N * N * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    struct canvas *__single cv = canvas(N, N, CANVAS_CS_SRGB);
    CHECK(px != NULL && cv != NULL);
    if (px && cv) {
        canvas_add_filter_drop_shadow(cv, CANVAS_CS_SRGB, 6.0f, 0.0f, 0.0f,
                                      0.0f, 0.0f, 0.0f, 1.0f);
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.0f, 0.0f, 1.0f, 0.5f);
        canvas_fill_rect(cv, 8.0f, 8.0f, 8.0f, 8.0f);  // square [8,16); shadow [14,22)
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        // Shadow only: black at the source's alpha.
        CHECK(px_near(pixel_at(px, len, N, 18, 12), 0, 0, 0, 128, 1));
        // Drawing only: the translucent blue itself.
        CHECK(px_near(pixel_at(px, len, N, 10, 12), 0, 0, 255, 128, 1));
        // Overlap: T over S = a 0.5 + 0.5*0.5 = 0.75; premul b stays 0.5, so
        // unpremultiplied blue is 0.5/0.75 = 2/3.
        CHECK(px_near(pixel_at(px, len, N, 15, 12), 0, 0, 170, 191, 2));
    }
    if (cv) {
        canvas_free(cv);
    }
    free(px);
}

// List order is observable around a drop-shadow: grayscale(1) AFTER the
// drop-shadow recolours the green shadow gray, while grayscale(1) BEFORE it
// grays the drawing but leaves the shadow's own green tint alone.
static void drop_shadow_order_visible(void) {
    enum { N = 32 };
    int const len = N * N * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    struct canvas *__single cv = canvas(N, N, CANVAS_CS_SRGB);
    CHECK(px != NULL && cv != NULL);
    if (px && cv) {
        // drop-shadow then grayscale: the pure-green shadow lands on its
        // 0.7152 luminance (182), and the red drawing on 54 gray.
        canvas_add_filter_drop_shadow(cv, CANVAS_CS_SRGB, 6.0f, 6.0f, 0.0f,
                                      0.0f, 1.0f, 0.0f, 1.0f);
        canvas_add_filter_grayscale(cv, 1.0f);
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_fill_rect(cv, 8.0f, 8.0f, 8.0f, 8.0f);  // square [8,16); shadow [14,22)
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 18, 18), 182, 182, 182, 255, 2));
        CHECK(px_near(pixel_at(px, len, N, 10, 10), 54, 54, 54, 255, 2));

        // grayscale then drop-shadow: the drawing is gray, but the shadow --
        // cast from the recoloured drawing's alpha, tinted afterwards -- stays
        // pure green.
        canvas_reset(cv);
        canvas_add_filter_grayscale(cv, 1.0f);
        canvas_add_filter_drop_shadow(cv, CANVAS_CS_SRGB, 6.0f, 6.0f, 0.0f,
                                      0.0f, 1.0f, 0.0f, 1.0f);
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_fill_rect(cv, 8.0f, 8.0f, 8.0f, 8.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 18, 18), 0, 255, 0, 255, 0));
        CHECK(px_near(pixel_at(px, len, N, 10, 10), 54, 54, 54, 255, 2));
    }
    if (cv) {
        canvas_free(cv);
    }
    free(px);
}

// The tile margin accommodates the shadow: offset + blur push it well past the
// shape's unblurred bbox, where it must still be visible -- and beyond the
// offset plus the three passes' spread it is exactly 0.
static void drop_shadow_margin(void) {
    enum { N = 48 };
    int const len = N * N * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    struct canvas *__single cv = canvas(N, N, CANVAS_CS_SRGB);
    CHECK(px != NULL && cv != NULL);
    if (px && cv) {
        // Square [8,16); shadow square [14,22), blurred skirt reaching [8,28).
        canvas_add_filter_drop_shadow(cv, CANVAS_CS_SRGB, 6.0f, 6.0f, 2.0f,
                                      0.0f, 0.0f, 0.0f, 1.0f);
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_fill_rect(cv, 8.0f, 8.0f, 8.0f, 8.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        // Well outside the unblurred bbox (x >= 16), still solidly shadowed --
        // a too-small tile would have clipped this to 0.
        CHECK(pixel_at(px, len, N, 18, 18).a > 200);
        CHECK(pixel_at(px, len, N, 24, 18).a > 20);  // 2px into the skirt
        // Beyond offset + 3*r the shadow is exactly 0.
        CHECK(px_near(pixel_at(px, len, N, 28, 18), 0, 0, 0, 0, 0));
        CHECK(px_near(pixel_at(px, len, N, 18, 28), 0, 0, 0, 0, 0));
        CHECK(px_near(pixel_at(px, len, N, 40, 40), 0, 0, 0, 0, 0));
    }
    if (cv) {
        canvas_free(cv);
    }
    free(px);
}

// The canvas shadowColor machinery and a filter drop-shadow are independent:
// with both active the op paints its drawing, its filter shadow (inside the
// tile), and its canvas shadow (cast from the op's coverage) without
// corrupting one another -- sane compositing, no trap.  The drawing pixel is
// held exactly; the two shadow regions just have to be present.
static void drop_shadow_with_canvas_shadow(void) {
    enum { N = 48 };
    int const len = N * N * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    struct canvas *__single cv = canvas(N, N, CANVAS_CS_SRGB);
    CHECK(px != NULL && cv != NULL);
    if (px && cv) {
        canvas_set_shadow_color_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_set_shadow_offset_x(cv, 14.0f);  // canvas shadow: x in [30,38)
        canvas_add_filter_drop_shadow(cv, CANVAS_CS_SRGB, -8.0f, 0.0f, 1.0f,
                                      0.0f, 0.0f, 1.0f, 1.0f);  // filter: x in [8,16)
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.0f, 1.0f);
        canvas_fill_rect(cv, 16.0f, 16.0f, 8.0f, 8.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 20, 20), 0, 255, 0, 255, 0));  // drawing
        struct rgba fs = pixel_at(px, len, N, 12, 20);  // filter shadow: blue
        CHECK(fs.a > 100 && fs.b > 100 && fs.g < 50);
        struct rgba cs = pixel_at(px, len, N, 33, 20);  // canvas shadow: red
        CHECK(cs.a > 200 && cs.r > 200 && cs.g < 50);
        CHECK(px_near(pixel_at(px, len, N, 44, 44), 0, 0, 0, 0, 0));
    }
    if (cv) {
        canvas_free(cv);
    }
    free(px);
}

// Non-finite dx/dy/blur and negative blur ignore the call, and a fully
// transparent shadow colour appends nothing: the fill stays crisp with no
// shadow anywhere.  Out-of-range colour channels clamp like set_fill_rgba.
static void drop_shadow_clamps(void) {
    enum { N = 32 };
    int const len = N * N * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    struct canvas *__single cv = canvas(N, N, CANVAS_CS_SRGB);
    CHECK(px != NULL && cv != NULL);
    if (px && cv) {
        canvas_add_filter_drop_shadow(cv, CANVAS_CS_SRGB, NAN, 4.0f, 0.0f, 1, 0, 0, 1);
        canvas_add_filter_drop_shadow(cv, CANVAS_CS_SRGB, 4.0f, INFINITY, 0.0f, 1, 0, 0, 1);
        canvas_add_filter_drop_shadow(cv, CANVAS_CS_SRGB, 4.0f, 4.0f, -1.0f, 1, 0, 0, 1);
        canvas_add_filter_drop_shadow(cv, CANVAS_CS_SRGB, 4.0f, 4.0f, NAN, 1, 0, 0, 1);
        canvas_add_filter_drop_shadow(cv, CANVAS_CS_SRGB, 4.0f, 4.0f, 0.0f, 1, 0, 0, 0.0f);
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.0f, 1.0f);
        canvas_fill_rect(cv, 8.0f, 8.0f, 8.0f, 8.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 12, 12), 0, 255, 0, 255, 0));
        CHECK(px_near(pixel_at(px, len, N, 18, 18), 0, 0, 0, 0, 0));  // no shadow
        CHECK(px_near(pixel_at(px, len, N, 7, 12), 0, 0, 0, 0, 0));   // crisp edge

        // Colour channels clamp to [0,1]: (2, -1, 0.5, 5) tints as (1, 0, 0.5, 1).
        canvas_reset(cv);
        canvas_add_filter_drop_shadow(cv, CANVAS_CS_SRGB, 8.0f, 0.0f, 0.0f,
                                      2.0f, -1.0f, 0.5f, 5.0f);
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.0f, 1.0f);
        canvas_fill_rect(cv, 8.0f, 8.0f, 8.0f, 8.0f);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(px_near(pixel_at(px, len, N, 20, 12), 255, 0, 128, 255, 1));
    }
    if (cv) {
        canvas_free(cv);
    }
    free(px);
}

// Reset bitmap + state is the caller's job (the filter list is part of state);
// this fills the whole canvas with (r,g,b,a) through whatever filters are set
// and reads back the centre pixel, unpremultiplied RGBA8.
static struct rgba fill_and_read(struct canvas *__single cv, uint8_t *__counted_by(len) px,
                                int len, float r, float g, float b, float a) {
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, r, g, b, a);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    return pixel_at(px, len, W, 8, 8);
}

int main(void) {
    int const len = W * W * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }
    struct canvas *__single cv = canvas(W, W, CANVAS_CS_SRGB);
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
    struct rgba const base = fill_and_read(cv, px, len, 0.4f, 0.6f, 0.8f, 0.5f);
    struct {
        void (*add)(struct canvas *__single cv, float amount);
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
    canvas_draw_bitmap_scaled(cv, CANVAS_CS_SRGB, img, 2, 2, 4.0f, 4.0f, 8.0f, 8.0f);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, W, 8, 8), 54, 54, 54, 255, 2));

    // put_image_data is NOT filtered (it overwrites; not a painted op).
    canvas_reset(cv);
    canvas_add_filter_invert(cv, 1.0f);
    canvas_put_image_data(cv, CANVAS_CS_SRGB, img, (int)sizeof img, 2, 2, 7, 7);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
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

    canvas_free(cv);
    free(px);

    // Text is filtered: red glyphs gray out -- and because the filter runs on
    // the painted tile, even the antialiased edge pixels read as neutral gray
    // (r == g == b at every covered pixel).
    enum { TW = 32 };
    int const tlen = TW * TW * 4;
    uint8_t *__counted_by(tlen) tpx = malloc((size_t)tlen);
    struct canvas *__single tcv = canvas(TW, TW, CANVAS_CS_SRGB);
    CHECK(tpx != NULL && tcv != NULL);
    if (tpx && tcv) {
        canvas_add_filter_grayscale(tcv, 1.0f);
        canvas_set_fill_rgba(tcv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_set_font_size(tcv, 28.0f);
        canvas_fill_text(tcv, "M", 4.0f, 26.0f);
        canvas_read_rgba(tcv, CANVAS_CS_SRGB, tpx, tlen);
        bool found_solid = false, all_gray = true;
        for (int y = 0; y < TW; y++) {
            for (int x = 0; x < TW; x++) {
                struct rgba p = pixel_at(tpx, tlen, TW, x, y);
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
        canvas_free(tcv);
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

    // drop-shadow(): the hand-computed sharp case, the kernel vs its
    // brute-force reference, tint and source-alpha proportionality, list-order
    // visibility, the tile margin, coexistence with the canvas shadow, and the
    // ignored/clamped parameters.
    drop_shadow_hard_offset();
    drop_shadow_subpixel_offset();
    drop_shadow_matches_reference();
    drop_shadow_tint_translucent();
    drop_shadow_translucent_source();
    drop_shadow_order_visible();
    drop_shadow_margin();
    drop_shadow_with_canvas_shadow();
    drop_shadow_clamps();
    return TEST_REPORT();
}
