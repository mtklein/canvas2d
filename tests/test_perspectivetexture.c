// Perspective-correct SAMPLING (docs/decisions/perspective.md, P2): under a
// perspective CTM the per-pixel source/user coordinate is the inverse
// homography (u/w, v/w), NOT a value linear across the device row.  These tests
// prove the image, gradient, and pattern paint loops sample at that
// perspective-correct coordinate -- and that it visibly DIFFERS from the
// affine/linear prediction (w ignored), so the divide is really happening --
// while the affine path stays byte-identical, and a perspective image draw
// round-trips through record/replay.

#include "cnvs_math.h"
#include "test_util.h"

#include "canvas.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A representative receding quad: a source rect mapped onto a trapezoid whose
// far (top) edge is narrower than its near (bottom) edge -- the classic
// foreshortening case, where perspective-correct sampling crowds source rows
// toward the far edge.  The matrix it sets is recovered for the predictor by
// recover_ctm (canvas has no 3x3 getter).
//
// Along the quad's centre column (device x ~ 100) the far edge sits at device
// y = QY_FAR (source y = 0) and the near edge at QY_NEAR (source y = S): the
// AXIS the tests probe and the LINEAR reference interpolate over.
enum { QY_FAR = 40, QY_NEAR = 170 };

static void receding_ctm(struct canvas *__single cv, float S) {
    canvas_set_perspective_quad(cv, 0.0f, 0.0f, S, S,
                                70.0f,  (float)QY_FAR,    // TL (far-left)
                                130.0f, (float)QY_FAR,    // TR (far-right)
                                180.0f, (float)QY_NEAR,   // BR (near-right)
                                20.0f,  (float)QY_NEAR);  // BL (near-left)
}

// The LINEAR (non-perspective) prediction of source y at a device-y centre: a
// plain interpolation between the far edge (source 0) and the near edge (source
// S).  This is what an affine warp of the same quad would sample -- the
// reference the perspective-correct uy must DIFFER from.  Returns < 0 when the
// device y is outside the quad's centre-column span.
static float linear_source_y(float devy, float S) {
    if (devy < (float)QY_FAR || devy > (float)QY_NEAR) {
        return -1.0f;
    }
    return S * (devy - (float)QY_FAR) / (float)(QY_NEAR - QY_FAR);
}

// Read one device pixel (RGBA, sRGB) from a canvas.
static void pixel_at(struct canvas *__single cv, int w, int h, int x, int y,
                     uint8_t out[4]) {
    int const len = w * h * 4;
    uint8_t *px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    int const i = (y * w + x) * 4;
    out[0] = px[i + 0];
    out[1] = px[i + 1];
    out[2] = px[i + 2];
    out[3] = px[i + 3];
    free(px);
}

// The 3x3 a quad maps the unit-ish source rect (0,0,S,S) onto the four corners
// used by receding_ctm, recovered by recording the set_transform line --
// mirrors test_perspective.c's quad recovery, so the predictor uses the EXACT
// matrix the renderer used.
static bool recover_ctm(cnvs_mat *out, float S) {
    char const *__null_terminated path = "build/test_perspectivetexture_ctm.canvas";
    struct canvas *__single cv = canvas(200, 200, CANVAS_CS_SRGB);
    if (!cv) {
        return false;
    }
    if (!canvas_record_to(cv, path)) {
        canvas_free(cv);
        return false;
    }
    receding_ctm(cv, S);
    canvas_free(cv);  // flush + close

    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    bool found = false;
    char buf[256];
    for (;;) {
        char *__null_terminated line = fgets(buf, (int)sizeof buf, f);
        if (!line) {
            break;
        }
        float v[9];
        if (sscanf(line, "set_transform %f %f %f %f %f %f %f %f %f",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                   &v[8]) == 9) {
            *out = (cnvs_mat){ .a = v[0], .b = v[1], .c = v[2], .d = v[3],
                               .e = v[4], .f = v[5], .g = v[6], .h = v[7],
                               .i = v[8] };
            found = true;
            break;
        }
    }
    (void)fclose(f);
    return found;
}

// --- image sampling ---------------------------------------------------------

// A device pixel deep in the projected quad samples the SOURCE texel the inverse
// homography predicts -- not the texel an affine (w-ignored) inverse would.  The
// source is a two-band texture (top half one colour, bottom half another); the
// perspective-correct boundary in device space is pulled toward the far edge,
// so there is a device row band where the two predictions name OPPOSITE bands.
static void test_image_sampling_perspective_correct(void) {
    int const W = 200, H = 200;
    float const S = 8.0f;
    int const T = 16;  // source texels per axis

    // Two-band source: rows [0,T/2) red, rows [T/2,T) blue.  Nearest sampling
    // (image smoothing off) so a probe pixel's colour is exactly one band.
    uint8_t tex[16 * 16 * 4];
    for (int y = 0; y < T; y++) {
        for (int x = 0; x < T; x++) {
            int const i = (y * T + x) * 4;
            bool const top = y < T / 2;
            tex[i + 0] = top ? 230 : 20;
            tex[i + 1] = 20;
            tex[i + 2] = top ? 20 : 230;
            tex[i + 3] = 255;
        }
    }

    cnvs_mat ctm;
    CHECK(recover_ctm(&ctm, S));
    cnvs_mat const inv = cnvs_mat_invert(ctm);  // device -> user (full 3x3)

    struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    receding_ctm(cv, S);
    canvas_set_image_smoothing_enabled(cv, false);  // nearest taps
    canvas_draw_bitmap_scaled(cv, CANVAS_CS_SRGB, tex, T, T, 0.0f, 0.0f, S, S);

    // Scan device rows down the quad's centre column.  At each, compute the
    // perspective-correct source y (u/w v/w) and the LINEAR reference source y
    // (interpolated between the quad's far and near edges), map both to source
    // texels, and find a row where they fall in OPPOSITE bands -- the proof
    // pixel: there the rendered colour reveals which sampling really ran.
    int const cx = W / 2;
    bool found_disagreement = false;
    for (int dy = QY_FAR + 1; dy < QY_NEAR; dy++) {
        cnvs_vec2 const dev = { .x = (float)cx + 0.5f, .y = (float)dy + 0.5f };

        // Perspective-correct: divide by w.
        float const w = inv.g * dev.x + inv.h * dev.y + inv.i;
        float const uy = (inv.b * dev.x + inv.d * dev.y + inv.f) / w;
        // Linear reference (affine warp of the same quad).
        float const ly = linear_source_y(dev.y, S);

        // user y -> source y -> texel row (the draw_image_quad source map with
        // sx=sy=0, sww=shh=S, dw=dh=S is the identity, so source y == user y).
        if (uy < 0.0f || uy >= S || ly < 0.0f || ly >= S) {
            continue;
        }
        int const prow = cnvs_f2i(floorf((uy / S) * (float)T));  // perspective band
        int const lrow = cnvs_f2i(floorf((ly / S) * (float)T));  // linear band
        bool const ptop = prow < T / 2;
        bool const ltop = lrow < T / 2;
        if (ptop == ltop) {
            continue;  // both predictions agree here; keep scanning
        }
        // The predictions disagree: the rendered pixel must match the
        // perspective band (red top, blue bottom), NOT the linear one.
        uint8_t p[4];
        pixel_at(cv, W, H, cx, dy, p);
        if (p[3] < 200) {
            continue;  // outside the quad coverage on this row; skip
        }
        bool const pixel_is_top = p[0] > p[2];  // red dominant => top band
        CHECK(pixel_is_top == ptop);            // matches perspective ...
        CHECK(pixel_is_top != ltop);            // ... and NOT linear
        found_disagreement = true;
        break;
    }
    CHECK(found_disagreement);  // the quad really foreshortens enough to differ
    canvas_free(cv);
}

// --- gradient sampling ------------------------------------------------------

// A linear gradient down the source depth axis, filled into a perspective quad,
// has its parameter solved per pixel in USER space (perspective-correct).  A
// probe pixel's colour matches the inverse-homography parameter, not the linear
// one.
static void test_gradient_perspective_correct(void) {
    int const W = 200, H = 200;
    float const S = 8.0f;

    cnvs_mat ctm;
    CHECK(recover_ctm(&ctm, S));
    cnvs_mat const inv = cnvs_mat_invert(ctm);

    struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    receding_ctm(cv, S);
    // Two-stop linear gradient along source y: green at y=0, magenta at y=S.
    // The parameter t = uy/S, so the colour reads out the source y directly.
    canvas_set_fill_linear_gradient(cv, CANVAS_CS_SRGB, CANVAS_ALPHA_UNPREMUL,
                                    0.0f, 0.0f, 0.0f, S);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, S, S);

    // Probe a row roughly two-thirds down the quad; the perspective t and the
    // linear t differ there.  Predict the green/magenta mix from each and
    // confirm the pixel matches the perspective one.
    int const cx = W / 2;
    bool checked = false;
    for (int dy = (QY_FAR + QY_NEAR) / 2; dy < QY_NEAR; dy++) {
        cnvs_vec2 const dev = { .x = (float)cx + 0.5f, .y = (float)dy + 0.5f };
        float const w = inv.g * dev.x + inv.h * dev.y + inv.i;
        float const uy = (inv.b * dev.x + inv.d * dev.y + inv.f) / w;
        float const ly = linear_source_y(dev.y, S);  // affine-warp reference
        if (uy < 0.0f || uy >= S || ly < 0.0f || ly >= S) {
            continue;
        }
        float const tp = uy / S;   // perspective parameter
        float const tl = ly / S;   // linear parameter
        if (fabsf(tp - tl) < 0.10f) {
            continue;  // too close to tell apart; keep scanning
        }
        uint8_t p[4];
        pixel_at(cv, W, H, cx, dy, p);
        if (p[3] < 200) {
            continue;
        }
        // Green channel falls from ~255 (t=0) to 0 (t=1); the green level
        // encodes t.  Compare the pixel's t to each prediction.
        float const t_pixel = 1.0f - (float)p[1] / 255.0f;
        float const err_p = fabsf(t_pixel - tp);
        float const err_l = fabsf(t_pixel - tl);
        CHECK(err_p < 0.06f);     // matches the perspective-correct parameter
        CHECK(err_p < err_l);     // and is closer to it than to the linear one
        checked = true;
        break;
    }
    CHECK(checked);
    canvas_free(cv);
}

// --- pattern sampling -------------------------------------------------------

// A pattern fill under a perspective CTM samples the pattern image at the
// perspective-correct device->pattern coordinate.  Same two-band proof as the
// image test, but the texture arrives as a fill pattern rather than a drawImage.
static void test_pattern_perspective_correct(void) {
    int const W = 200, H = 200;
    int const T = 16;
    float const S = (float)T;  // source rect == pattern dims: one texel per user unit

    // Pattern source spanning the S x S source rect at one image pixel per user
    // unit, so to_pattern (= device->user) maps straight to pattern texels.  Two
    // bands again (top red, bottom blue).
    uint8_t tex[16 * 16 * 4];
    for (int y = 0; y < T; y++) {
        for (int x = 0; x < T; x++) {
            int const i = (y * T + x) * 4;
            bool const top = y < T / 2;
            tex[i + 0] = top ? 230 : 20;
            tex[i + 1] = 20;
            tex[i + 2] = top ? 20 : 230;
            tex[i + 3] = 255;
        }
    }

    cnvs_mat ctm;
    CHECK(recover_ctm(&ctm, S));
    cnvs_mat const inv = cnvs_mat_invert(ctm);  // device -> user == device -> pattern

    struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    receding_ctm(cv, S);
    canvas_set_image_smoothing_enabled(cv, false);
    // Set the pattern under the perspective CTM directly: to_pattern is its
    // inverse (device->user), and with S == T one user unit is one pattern
    // texel, so the device->pattern map IS inv.
    canvas_set_fill_pattern(cv, CANVAS_CS_SRGB, tex, T, T, CANVAS_REPEAT);
    canvas_fill_rect(cv, 0.0f, 0.0f, S, S);

    // Find a row where the perspective band and the linear-reference band differ.
    int const cx = W / 2;
    bool found = false;
    for (int dy = QY_FAR + 1; dy < QY_NEAR; dy++) {
        cnvs_vec2 const dev = { .x = (float)cx + 0.5f, .y = (float)dy + 0.5f };
        float const w = inv.g * dev.x + inv.h * dev.y + inv.i;
        float const uy = (inv.b * dev.x + inv.d * dev.y + inv.f) / w;
        float const ly = linear_source_y(dev.y, S);  // affine-warp reference
        if (uy < 0.0f || uy >= S || ly < 0.0f || ly >= S) {
            continue;
        }
        int const prow = cnvs_f2i(floorf((uy / S) * (float)T));
        int const lrow = cnvs_f2i(floorf((ly / S) * (float)T));
        bool const ptop = prow < T / 2;
        bool const ltop = lrow < T / 2;
        if (ptop == ltop) {
            continue;
        }
        uint8_t p[4];
        pixel_at(cv, W, H, cx, dy, p);
        if (p[3] < 200) {
            continue;
        }
        bool const pixel_is_top = p[0] > p[2];
        CHECK(pixel_is_top == ptop);
        CHECK(pixel_is_top != ltop);
        found = true;
        break;
    }
    CHECK(found);
    canvas_free(cv);
}

// --- affine unchanged -------------------------------------------------------

// An image and a gradient drawn under an AFFINE CTM set via set_transform_3x3
// with (0,0,1) match the same draws under the affine set_transform, byte for
// byte: the perspective branch must never perturb the affine fast path.
static void test_affine_sampling_unchanged(void) {
    int const W = 96, H = 96, NPX = W * H * 4;
    uint8_t a[96 * 96 * 4];
    uint8_t b[96 * 96 * 4];

    int const T = 16;
    uint8_t tex[16 * 16 * 4];
    for (int y = 0; y < T; y++) {
        for (int x = 0; x < T; x++) {
            int const i = (y * T + x) * 4;
            tex[i + 0] = (uint8_t)(x * 16);
            tex[i + 1] = (uint8_t)(y * 16);
            tex[i + 2] = (uint8_t)((x ^ y) * 8);
            tex[i + 3] = 255;
        }
    }

    struct canvas *__single ca = canvas(W, H, CANVAS_CS_SRGB);
    struct canvas *__single cb = canvas(W, H, CANVAS_CS_SRGB);
    CHECK(ca != NULL && cb != NULL);
    if (!ca || !cb) {
        canvas_free(ca);
        canvas_free(cb);
        return;
    }
    canvas_set_transform(ca, 1.4f, 0.25f, -0.35f, 1.2f, 10.0f, 6.0f);
    canvas_set_transform_3x3(cb, 1.4f, 0.25f, -0.35f, 1.2f, 10.0f, 6.0f,
                             0.0f, 0.0f, 1.0f);
    for (int k = 0; k < 2; k++) {
        struct canvas *__single c = k == 0 ? ca : cb;
        // An image draw (bilinear), then a linear gradient fill.
        canvas_draw_bitmap_scaled(c, CANVAS_CS_SRGB, tex, T, T,
                                  4.0f, 4.0f, 40.0f, 40.0f);
        canvas_set_fill_linear_gradient(c, CANVAS_CS_SRGB, CANVAS_ALPHA_UNPREMUL,
                                        0.0f, 48.0f, 48.0f, 88.0f);
        canvas_add_fill_color_stop(c, CANVAS_CS_SRGB, 0.0f, 0.9f, 0.2f, 0.3f, 1.0f);
        canvas_add_fill_color_stop(c, CANVAS_CS_SRGB, 1.0f, 0.1f, 0.4f, 0.9f, 1.0f);
        canvas_fill_rect(c, 8.0f, 50.0f, 60.0f, 36.0f);
    }
    canvas_read_rgba(ca, CANVAS_CS_SRGB, a, NPX);
    canvas_read_rgba(cb, CANVAS_CS_SRGB, b, NPX);
    CHECK(memcmp(a, b, (size_t)NPX) == 0);
    canvas_free(ca);
    canvas_free(cb);
}

// --- record / replay --------------------------------------------------------

// In-memory record -> replay round trip with a perspective image draw reproduces
// the surface byte for byte.
static void test_record_replay_image(void) {
    int const W = 200, H = 200, NPX = W * H * 4;
    float const S = 8.0f;
    int const T = 16;
    char const *__null_terminated path = "build/test_perspectivetexture.canvas";
    uint8_t recorded[200 * 200 * 4];

    uint8_t tex[16 * 16 * 4];
    for (int y = 0; y < T; y++) {
        for (int x = 0; x < T; x++) {
            int const i = (y * T + x) * 4;
            bool const dark = (((x / 4) + (y / 4)) & 1) != 0;
            tex[i + 0] = dark ? 40 : 220;
            tex[i + 1] = dark ? 80 : 200;
            tex[i + 2] = dark ? 140 : 120;
            tex[i + 3] = 255;
        }
    }

    {
        struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas_record_to(cv, path));
        receding_ctm(cv, S);
        canvas_draw_bitmap_scaled(cv, CANVAS_CS_SRGB, tex, T, T, 0.0f, 0.0f, S, S);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, recorded, NPX);
        canvas_free(cv);  // flush + close
    }
    {
        struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas_replay_from(cv, path));
        uint8_t replayed[200 * 200 * 4];
        canvas_read_rgba(cv, CANVAS_CS_SRGB, replayed, NPX);
        CHECK(memcmp(recorded, replayed, (size_t)NPX) == 0);
        canvas_free(cv);
    }
}

int main(void) {
    test_image_sampling_perspective_correct();
    test_gradient_perspective_correct();
    test_pattern_perspective_correct();
    test_affine_sampling_unchanged();
    test_record_replay_image();
    return TEST_REPORT();
}
