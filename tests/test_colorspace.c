// The working-space parameter (canvas.h) and the linear-light pipeline.  Three
// claims:
//   1. The default space is sRGB, and it survives reset()/resize() -- the space
//      is on the canvas, not the save/restore state.
//   2. A linear canvas round-trips an untagged sRGB colour: the decode on entry
//      and the encode on readback cancel to within 8-bit tolerance, and a
//      get_image_data -> put_image_data round trip on a linear canvas does too.
//   3. Linear compositing is REALLY happening: a translucent source-over on a
//      linear canvas matches a double-precision LINEAR reference (not the sRGB
//      one), and multiply on a linear canvas differs from the sRGB result.
//
// The space has no public getter, so it is observed behaviourally: a 50%-alpha
// black source-over an opaque mid-grey reads back ~64 on an sRGB canvas (the
// blend is in gamma space) but ~95 on a linear canvas (the blend is in light,
// then re-encoded) -- a gap of ~30/255, far outside any rounding tolerance.

#include "canvas.h"
#include "cnvs_color.h"
#include "test_pixels.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>

// A probe that reads back differently per working space: opaque mid-grey, then a
// 50%-alpha black painted over it.  Returns the centre pixel's red byte.
static int space_probe(struct canvas *__single cv, int w, int h,
                       uint8_t *__counted_by(len) px, int len) {
    canvas_set_global_composite_operation(cv, CANVAS_OP_SOURCE_OVER);
    canvas_set_fill_rgba(cv, 0.5f, 0.5f, 0.5f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_rgba(cv, 0.0f, 0.0f, 0.0f, 0.5f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_read_rgba(cv, px, len);
    return (int)pixel_at(px, len, w, w / 2, h / 2).r;
}

// The probe's expected red byte, computed in double precision for one space.
// 0.5 sRGB -> the working-space value, source-over 50% black, then read back
// (encode on a linear canvas, identity on an sRGB one), quantized.
static int probe_expected(bool linear) {
    double base = 0.5;  // the mid-grey, encoded sRGB
    if (linear) {
        base = (double)cnvs_srgb_to_linear(0.5f);  // decode on entry
    }
    double const composited = base * 0.5;  // co = s + (1-sa)*d, s = 0 (black)
    double out = composited;
    if (linear) {
        out = (double)cnvs_linear_to_srgb((float)composited);  // encode on exit
    }
    int const q = (int)(out * 255.0 + 0.5);
    return q;
}

static void space_default_and_persistence(void) {
    int const w = 8, h = 8, len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return;
    }
    int const srgb_red = probe_expected(false);
    int const lin_red  = probe_expected(true);
    CHECK(abs(srgb_red - lin_red) > 20);  // the two spaces are clearly apart

    // canvas() is sRGB by default.
    struct canvas *__single def = canvas(w, h);
    CHECK(def != NULL);
    if (def) {
        CHECK(abs(space_probe(def, w, h, px, len) - srgb_red) <= 2);
        canvas_free(def);
    }
    // canvas_in_space(.., SRGB) is the same.
    struct canvas *__single s = canvas_in_space(w, h, CANVAS_WS_SRGB);
    CHECK(s != NULL);
    if (s) {
        CHECK(abs(space_probe(s, w, h, px, len) - srgb_red) <= 2);
        canvas_free(s);
    }

    // A linear canvas behaves linearly -- and stays linear across reset/resize,
    // because the space is not part of the drawing state.
    struct canvas *__single lin = canvas_in_space(w, h, CANVAS_WS_LINEAR);
    CHECK(lin != NULL);
    if (lin) {
        CHECK(abs(space_probe(lin, w, h, px, len) - lin_red) <= 2);
        canvas_reset(lin);
        CHECK(abs(space_probe(lin, w, h, px, len) - lin_red) <= 2);
        CHECK(canvas_resize(lin, w + 4, h + 4));
        int const len2 = (w + 4) * (h + 4) * 4;
        uint8_t *__counted_by(len2) px2 = malloc((size_t)len2);
        CHECK(px2 != NULL);
        if (px2) {
            CHECK(abs(space_probe(lin, w + 4, h + 4, px2, len2) - lin_red) <= 2);
            free(px2);
        }
        canvas_free(lin);
    }
    free(px);
}

// A known untagged sRGB byte (188) painted opaque on a linear canvas reads back
// to ~the same byte: the entry decode and exit encode cancel within 8-bit
// tolerance.  Opaque, so no premultiply rounding beyond the transfer's own.
static void linear_color_round_trip(void) {
    int const w = 8, h = 8, len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return;
    }
    struct canvas *__single cv = canvas_in_space(w, h, CANVAS_WS_LINEAR);
    CHECK(cv != NULL);
    if (cv) {
        float const c = 188.0f / 255.0f;
        canvas_set_fill_rgba(cv, c, c * 0.5f, c * 0.25f, 1.0f);
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_read_rgba(cv, px, len);
        struct rgba const p = pixel_at(px, len, w, w / 2, h / 2);
        CHECK(abs((int)p.r - 188) <= 2);
        CHECK(abs((int)p.g - (int)(c * 0.5f * 255.0f + 0.5f)) <= 2);
        CHECK(abs((int)p.b - (int)(c * 0.25f * 255.0f + 0.5f)) <= 2);
        CHECK(p.a == 255);
        canvas_free(cv);
    }
    free(px);
}

// get_image_data -> put_image_data on a linear canvas round-trips within 8-bit
// tolerance: get encodes the surface to sRGB bytes, put decodes them back, and
// the two cancel.  An assorted (translucent) source so premultiply rounding is
// in the loop too.
static void linear_image_data_round_trip(void) {
    int const w = 16, h = 16, len = w * h * 4;
    uint8_t *__counted_by(len) a = malloc((size_t)len);
    uint8_t *__counted_by(len) b = malloc((size_t)len);
    CHECK(a != NULL && b != NULL);
    if (!a || !b) {
        free(a);
        free(b);
        return;
    }
    struct canvas *__single cv = canvas_in_space(w, h, CANVAS_WS_LINEAR);
    CHECK(cv != NULL);
    if (cv) {
        canvas_set_fill_rgba(cv, 0.85f, 0.35f, 0.55f, 0.7f);
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_set_fill_rgba(cv, 0.1f, 0.6f, 0.9f, 1.0f);
        canvas_fill_rect(cv, 4.0f, 4.0f, 8.0f, 8.0f);
        canvas_get_image_data(cv, 0, 0, w, h, a, len);

        // Round the read-back bytes back onto a fresh linear canvas, read again.
        struct canvas *__single cv2 = canvas_in_space(w, h, CANVAS_WS_LINEAR);
        CHECK(cv2 != NULL);
        if (cv2) {
            canvas_put_image_data(cv2, a, len, w, h, 0, 0);
            canvas_get_image_data(cv2, 0, 0, w, h, b, len);
            int maxd = 0;
            for (int i = 0; i < len; i++) {
                int const d = abs((int)a[i] - (int)b[i]);
                if (d > maxd) { maxd = d; }
            }
            CHECK(maxd <= 2);  // 8-bit tolerance through decode+premul+encode
            canvas_free(cv2);
        }
        canvas_free(cv);
    }
    free(a);
    free(b);
}

// Linear source-over of two translucent colours matches a DOUBLE linear
// reference (the proof the blend really happens in light), and differs from the
// sRGB result.  src 60%-alpha over a 100%-alpha backdrop, one channel.
static void linear_source_over_oracle(void) {
    int const w = 8, h = 8, len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return;
    }
    float const bd = 0.8f, sd = 0.2f, sa = 0.6f;  // backdrop, source (sRGB), alpha

    // Double linear reference: decode, premultiply, source-over in light, read
    // back (unpremultiply, encode), quantize.
    double const bl = (double)cnvs_srgb_to_linear(bd);
    double const sl = (double)cnvs_srgb_to_linear(sd);
    double const co = sl * (double)sa + bl * (1.0 - (double)sa);  // premul rgb, da=1
    double const ao = (double)sa + 1.0 * (1.0 - (double)sa);      // = 1
    double const un = co / ao;
    int const want_lin = (int)((double)cnvs_linear_to_srgb((float)un) * 255.0 + 0.5);

    // Double sRGB reference: the same fold, but entirely in gamma space.
    double const co_s = (double)sd * (double)sa + (double)bd * (1.0 - (double)sa);
    int const want_srgb = (int)(co_s * 255.0 + 0.5);
    CHECK(abs(want_lin - want_srgb) > 8);  // the spaces genuinely diverge here

    struct canvas *__single cv = canvas_in_space(w, h, CANVAS_WS_LINEAR);
    CHECK(cv != NULL);
    if (cv) {
        canvas_set_fill_rgba(cv, bd, bd, bd, 1.0f);
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_set_fill_rgba(cv, sd, sd, sd, sa);
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_read_rgba(cv, px, len);
        int const got = (int)pixel_at(px, len, w, w / 2, h / 2).r;
        CHECK(abs(got - want_lin) <= 2);   // matches the LINEAR oracle
        CHECK(abs(got - want_srgb) > 4);   // and is NOT the sRGB answer
        canvas_free(cv);
    }
    free(px);
}

// multiply on a linear canvas differs from multiply on an sRGB canvas: proof the
// working space reaches the generic (non source-over) kernel too.  A TRANSLUCENT
// source (50% alpha over an opaque backdrop) engages the premultiplied
// s*(1-da)+d*(1-sa)+T fold, where the two spaces diverge clearly -- an opaque
// multiply, by contrast, has encode(decode(x)^2) ~ x^2 numerically and barely
// moves, which is itself correct, just not a useful detector.
static void linear_multiply_differs(void) {
    int const w = 8, h = 8, len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return;
    }
    int srgb_red = -1, lin_red = -1;
    for (int pass = 0; pass < 2; pass++) {
        struct canvas *__single cv = canvas_in_space(
            w, h, pass == 0 ? CANVAS_WS_SRGB : CANVAS_WS_LINEAR);
        CHECK(cv != NULL);
        if (!cv) {
            continue;
        }
        canvas_set_global_composite_operation(cv, CANVAS_OP_SOURCE_OVER);
        canvas_set_fill_rgba(cv, 0.9f, 0.9f, 0.9f, 1.0f);  // opaque backdrop
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_set_global_composite_operation(cv, CANVAS_OP_MULTIPLY);
        canvas_set_fill_rgba(cv, 0.2f, 0.2f, 0.2f, 0.5f);  // translucent source
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_read_rgba(cv, px, len);
        int const r = (int)pixel_at(px, len, w, w / 2, h / 2).r;
        if (pass == 0) { srgb_red = r; } else { lin_red = r; }
        canvas_free(cv);
    }
    // ~138 (sRGB) vs ~171 (linear): the premultiplied multiply fold lands far
    // apart once the backdrop is decoded into light.
    CHECK(srgb_red >= 0 && lin_red >= 0);
    CHECK(abs(srgb_red - lin_red) > 8);
    free(px);
}

int main(void) {
    space_default_and_persistence();
    linear_color_round_trip();
    linear_image_data_round_trip();
    linear_source_over_oracle();
    linear_multiply_differs();
    return TEST_REPORT();
}
