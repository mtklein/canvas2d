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
#include "cnvs_math.h"
#include "test_pixels.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>

// A probe that reads back differently per working space: opaque mid-grey, then a
// 50%-alpha black painted over it.  Returns the centre pixel's red byte.
static int space_probe(struct canvas *__single cv, int w, int h,
                       uint8_t *__counted_by(len) px, int len) {
    canvas_set_global_composite_operation(cv, CANVAS_OP_SOURCE_OVER);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.5f, 0.5f, 0.5f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.0f, 0.0f, 0.0f, 0.5f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
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
    struct canvas *__single s = canvas_in_space(w, h, CANVAS_CS_SRGB);
    CHECK(s != NULL);
    if (s) {
        CHECK(abs(space_probe(s, w, h, px, len) - srgb_red) <= 2);
        canvas_free(s);
    }

    // A linear canvas behaves linearly -- and stays linear across reset/resize,
    // because the space is not part of the drawing state.
    struct canvas *__single lin = canvas_in_space(w, h, CANVAS_CS_LINEAR_SRGB);
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
    struct canvas *__single cv = canvas_in_space(w, h, CANVAS_CS_LINEAR_SRGB);
    CHECK(cv != NULL);
    if (cv) {
        float const c = 188.0f / 255.0f;
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, c, c * 0.5f, c * 0.25f, 1.0f);
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
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
    struct canvas *__single cv = canvas_in_space(w, h, CANVAS_CS_LINEAR_SRGB);
    CHECK(cv != NULL);
    if (cv) {
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.85f, 0.35f, 0.55f, 0.7f);
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.1f, 0.6f, 0.9f, 1.0f);
        canvas_fill_rect(cv, 4.0f, 4.0f, 8.0f, 8.0f);
        canvas_get_image_data(cv, CANVAS_CS_SRGB, 0, 0, w, h, a, len);

        // Round the read-back bytes back onto a fresh linear canvas, read again.
        struct canvas *__single cv2 = canvas_in_space(w, h, CANVAS_CS_LINEAR_SRGB);
        CHECK(cv2 != NULL);
        if (cv2) {
            canvas_put_image_data(cv2, CANVAS_CS_SRGB, a, len, w, h, 0, 0);
            canvas_get_image_data(cv2, CANVAS_CS_SRGB, 0, 0, w, h, b, len);
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
// sRGB result.  The inputs SPEAK LINEAR: backdrop and source are linear values
// tagged CANVAS_CS_LINEAR_SRGB, so they reach the surface verbatim -- no entry
// transfer to model, the oracle folds them as-is.  src 60%-alpha over a
// 100%-alpha backdrop, one channel.
static void linear_source_over_oracle(void) {
    int const w = 8, h = 8, len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return;
    }
    float const bl_f = 0.8f, sl_f = 0.2f, sa = 0.6f;  // backdrop, source (LINEAR), alpha

    // Double linear reference: the tagged-linear values are already in light, so
    // just premultiply, source-over, read back (unpremultiply, encode), quantize.
    double const bl = (double)bl_f;
    double const sl = (double)sl_f;
    double const co = sl * (double)sa + bl * (1.0 - (double)sa);  // premul rgb, da=1
    double const ao = (double)sa + 1.0 * (1.0 - (double)sa);      // = 1
    double const un = co / ao;
    int const want_lin = (int)((double)cnvs_linear_to_srgb((float)un) * 255.0 + 0.5);

    // Double sRGB reference: the SAME numbers taken as gamma-encoded sRGB and
    // folded entirely in gamma space -- what an sRGB canvas would produce.
    double const co_s = (double)sl_f * (double)sa + (double)bl_f * (1.0 - (double)sa);
    int const want_srgb = (int)(co_s * 255.0 + 0.5);
    CHECK(abs(want_lin - want_srgb) > 8);  // the spaces genuinely diverge here

    struct canvas *__single cv = canvas_in_space(w, h, CANVAS_CS_LINEAR_SRGB);
    CHECK(cv != NULL);
    if (cv) {
        canvas_set_fill_rgba(cv, CANVAS_CS_LINEAR_SRGB, bl_f, bl_f, bl_f, 1.0f);
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_set_fill_rgba(cv, CANVAS_CS_LINEAR_SRGB, sl_f, sl_f, sl_f, sa);
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
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
            w, h, pass == 0 ? CANVAS_CS_SRGB : CANVAS_CS_LINEAR_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            continue;
        }
        canvas_set_global_composite_operation(cv, CANVAS_OP_SOURCE_OVER);
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.9f, 0.9f, 0.9f, 1.0f);  // opaque backdrop
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_set_global_composite_operation(cv, CANVAS_OP_MULTIPLY);
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.2f, 0.2f, 0.2f, 0.5f);  // translucent source
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
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

// --- linear-canvas blend modes: the encode-roundtrip arm ---------------------
//
// On a LINEAR canvas the spec-in-sRGB modes (overlay, darken, lighten, the
// dodge/burn pair, hard/soft-light, additive lighter, and the non-separable
// hue/saturation/color/luminosity) run through canvas.c's encode->sRGB / blend /
// decode->linear wrapper.  This sweep paints each over a known backdrop on a
// CANVAS_CS_LINEAR_SRGB canvas and checks the centre pixel against a
// double-precision oracle computed in the SAME frame the wrapper uses.
//
// Inputs are tagged CANVAS_CS_LINEAR_SRGB so they reach the surface verbatim
// (no entry transfer); both are OPAQUE so the premultiplied fold collapses to
// the bare blend term and the unpremul/repremul rounding through f16 stays
// small.  The wrapper encodes the stored linear operands to sRGB, blends there,
// decodes to linear; the CANVAS_CS_SRGB readback re-encodes -- so the observable
// sRGB byte is exactly the sRGB-space blend of the sRGB-encoded operands.  That
// is the oracle: encode each linear operand, blend per the spec formula, quantize.

static double clampd(double x) { return x < 0.0 ? 0.0 : x > 1.0 ? 1.0 : x; }

// Separable spec blend B(cb, cs) for one channel, in encoded [0,1].
static double sep_blend(enum canvas_composite_op m, double cb, double cs) {
    switch ((int)m) {
        case CANVAS_OP_MULTIPLY:   return cb * cs;
        case CANVAS_OP_SCREEN:     return cb + cs - cb * cs;
        case CANVAS_OP_OVERLAY:    return cb <= 0.5 ? 2.0 * cb * cs
                                                    : 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs);
        case CANVAS_OP_DARKEN:     return cb < cs ? cb : cs;
        case CANVAS_OP_LIGHTEN:    return cb > cs ? cb : cs;
        case CANVAS_OP_COLOR_DODGE:
            return cb <= 0.0 ? 0.0 : cs >= 1.0 ? 1.0
                 : (cb / (1.0 - cs) < 1.0 ? cb / (1.0 - cs) : 1.0);
        case CANVAS_OP_COLOR_BURN:
            return cb >= 1.0 ? 1.0 : cs <= 0.0 ? 0.0
                 : 1.0 - ((1.0 - cb) / cs < 1.0 ? (1.0 - cb) / cs : 1.0);
        case CANVAS_OP_HARD_LIGHT: return cs <= 0.5 ? 2.0 * cb * cs
                                                    : 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs);
        case CANVAS_OP_SOFT_LIGHT: {
            double const d = cb <= 0.25 ? ((16.0 * cb - 12.0) * cb + 4.0) * cb : sqrt(cb);
            return cs <= 0.5 ? cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb)
                             : cb + (2.0 * cs - 1.0) * (d - cb);
        }
        default:                   return cs;  // (unreached for the modes swept)
    }
}

// The double-precision spec luminosity / saturation helpers (W3C compositing),
// matching blend_nonsep8's set_lum/clip_color/set_saturation in float frame.
typedef struct { double r, g, b; } rgbd;
static double lumd(rgbd c) { return 0.3 * c.r + 0.59 * c.g + 0.11 * c.b; }
static double mind3(rgbd c) { double m = c.r < c.g ? c.r : c.g; return m < c.b ? m : c.b; }
static double maxd3(rgbd c) { double m = c.r > c.g ? c.r : c.g; return m > c.b ? m : c.b; }
static rgbd clip_color(rgbd c) {
    double const l = lumd(c), n = mind3(c), x = maxd3(c);
    if (n < 0.0) {
        double const k = l / (l - n);
        c.r = l + (c.r - l) * k; c.g = l + (c.g - l) * k; c.b = l + (c.b - l) * k;
    }
    if (x > 1.0) {
        double const k = (1.0 - l) / (x - l);
        c.r = l + (c.r - l) * k; c.g = l + (c.g - l) * k; c.b = l + (c.b - l) * k;
    }
    return c;
}
static rgbd set_lum(rgbd c, double l) {
    double const d = l - lumd(c);
    c.r += d; c.g += d; c.b += d;
    return clip_color(c);
}
static double satd(rgbd c) { return maxd3(c) - mind3(c); }
static rgbd set_sat(rgbd c, double s) {
    double const mn = mind3(c), mx = maxd3(c);
    if (mx <= mn) { return (rgbd){ 0.0, 0.0, 0.0 }; }
    double const k = s / (mx - mn);
    return (rgbd){ (c.r - mn) * k, (c.g - mn) * k, (c.b - mn) * k };
}
static rgbd nonsep_blend(enum canvas_composite_op m, rgbd cb, rgbd cs) {
    switch ((int)m) {
        case CANVAS_OP_HUE:        return set_lum(set_sat(cs, satd(cb)), lumd(cb));
        case CANVAS_OP_SATURATION: return set_lum(set_sat(cb, satd(cs)), lumd(cb));
        case CANVAS_OP_COLOR:      return set_lum(cs, lumd(cb));
        default:                   return set_lum(cb, lumd(cs));  // luminosity
    }
}

static void linear_blend_modes_oracle(void) {
    int const w = 8, h = 8, len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return;
    }
    // Backdrop and source as LINEAR values (tagged LINEAR_SRGB, stored verbatim),
    // distinct per channel so the non-separable HSL math has a real axis to work.
    rgbd const bl = { 0.62, 0.30, 0.78 };  // backdrop, linear
    rgbd const sl = { 0.18, 0.71, 0.42 };  // source, linear

    enum canvas_composite_op const modes[] = {
        CANVAS_OP_OVERLAY, CANVAS_OP_DARKEN, CANVAS_OP_LIGHTEN,
        CANVAS_OP_COLOR_DODGE, CANVAS_OP_COLOR_BURN, CANVAS_OP_HARD_LIGHT,
        CANVAS_OP_SOFT_LIGHT, CANVAS_OP_LIGHTER,
        CANVAS_OP_HUE, CANVAS_OP_SATURATION, CANVAS_OP_COLOR, CANVAS_OP_LUMINOSITY,
    };
    for (int mi = 0; mi < (int)(sizeof modes / sizeof modes[0]); mi++) {
        enum canvas_composite_op const m = modes[mi];

        // Encode the stored linear operands to sRGB -- the wrapper's blend frame.
        rgbd const bs = { (double)cnvs_linear_to_srgb((float)bl.r),
                          (double)cnvs_linear_to_srgb((float)bl.g),
                          (double)cnvs_linear_to_srgb((float)bl.b) };
        rgbd const ss = { (double)cnvs_linear_to_srgb((float)sl.r),
                          (double)cnvs_linear_to_srgb((float)sl.g),
                          (double)cnvs_linear_to_srgb((float)sl.b) };
        rgbd blended;
        if (m == CANVAS_OP_LIGHTER) {  // additive: co = s + d, clamped per channel
            blended = (rgbd){ ss.r + bs.r, ss.g + bs.g, ss.b + bs.b };
        } else if ((int)m >= CANVAS_OP_HUE) {
            blended = nonsep_blend(m, bs, ss);
        } else {
            blended = (rgbd){ sep_blend(m, bs.r, ss.r),
                              sep_blend(m, bs.g, ss.g),
                              sep_blend(m, bs.b, ss.b) };
        }
        // The wrapper decodes the sRGB blend back to linear; the SRGB readback
        // re-encodes -- the two cancel, so the observable byte is the encoded
        // blend, quantized.  (For lighter, the per-channel clamp lands the byte.)
        int const want_r = (int)(clampd(blended.r) * 255.0 + 0.5);
        int const want_g = (int)(clampd(blended.g) * 255.0 + 0.5);
        int const want_b = (int)(clampd(blended.b) * 255.0 + 0.5);

        struct canvas *__single cv = canvas_in_space(w, h, CANVAS_CS_LINEAR_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            continue;
        }
        canvas_set_global_composite_operation(cv, CANVAS_OP_SOURCE_OVER);
        canvas_set_fill_rgba(cv, CANVAS_CS_LINEAR_SRGB,
                             (float)bl.r, (float)bl.g, (float)bl.b, 1.0f);
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_set_global_composite_operation(cv, m);
        canvas_set_fill_rgba(cv, CANVAS_CS_LINEAR_SRGB,
                             (float)sl.r, (float)sl.g, (float)sl.b, 1.0f);
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        struct rgba const p = pixel_at(px, len, w, w / 2, h / 2);
        // f16 surface + two transfer round trips: a few-LSB tolerance.
        CHECK(abs((int)p.r - want_r) <= 4);
        CHECK(abs((int)p.g - want_g) <= 4);
        CHECK(abs((int)p.b - want_b) <= 4);
        CHECK(p.a == 255);
        canvas_free(cv);
    }
    free(px);
}

// The NEW tag branches on an sRGB working canvas.  A CANVAS_CS_LINEAR_SRGB fill
// of a known LINEAR value encodes linear->sRGB then clamps: linear 0.5 reads
// back as the sRGB-encoded byte (~188), NOT 128 (which is what an SRGB-tagged
// 0.5 would store).  And a CANVAS_CS_OKLAB fill of a known (L,a,b) triple reads
// back as that colour converted Oklab->linear->sRGB.  References come from the
// same cnvs_color kernels the setter uses, so this is a behavioural check on the
// intern_color wiring, not on the kernels' own numbers.
static void srgb_canvas_tag_branches(void) {
    int const w = 8, h = 8, len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return;
    }
    struct canvas *__single cv = canvas_in_space(w, h, CANVAS_CS_SRGB);
    CHECK(cv != NULL);
    if (cv) {
        // LINEAR_SRGB input on an sRGB canvas: encode linear->sRGB, clamp01.
        float const lr = 0.5f, lg = 0.25f, lb = 0.75f;
        int const want_r = (int)(cnvs_clamp01(cnvs_linear_to_srgb(lr)) * 255.0f + 0.5f);
        int const want_g = (int)(cnvs_clamp01(cnvs_linear_to_srgb(lg)) * 255.0f + 0.5f);
        int const want_b = (int)(cnvs_clamp01(cnvs_linear_to_srgb(lb)) * 255.0f + 0.5f);
        canvas_set_fill_rgba(cv, CANVAS_CS_LINEAR_SRGB, lr, lg, lb, 1.0f);
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        struct rgba const p = pixel_at(px, len, w, w / 2, h / 2);
        CHECK(abs((int)p.r - want_r) <= 2);  // ~188, the encoded byte
        CHECK(abs((int)p.g - want_g) <= 2);
        CHECK(abs((int)p.b - want_b) <= 2);
        CHECK((int)p.r != 128);              // and emphatically NOT the raw 0.5
        CHECK(p.a == 255);

        // OKLAB input on an sRGB canvas: Oklab -> linear -> sRGB, clamp01.
        cnvs_oklab const lab = { .L = 0.7f, .a = 0.1f, .b = 0.05f };
        cnvs_rgb const enc = cnvs_rgb_linear_to_srgb(cnvs_oklab_to_linear_srgb(lab));
        int const ok_r = (int)(cnvs_clamp01(enc.r) * 255.0f + 0.5f);  // ~219
        int const ok_g = (int)(cnvs_clamp01(enc.g) * 255.0f + 0.5f);  // ~130
        int const ok_b = (int)(cnvs_clamp01(enc.b) * 255.0f + 0.5f);  // ~121
        canvas_set_fill_rgba(cv, CANVAS_CS_OKLAB, lab.L, lab.a, lab.b, 1.0f);
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        struct rgba const q = pixel_at(px, len, w, w / 2, h / 2);
        CHECK(abs((int)q.r - ok_r) <= 2);
        CHECK(abs((int)q.g - ok_g) <= 2);
        CHECK(abs((int)q.b - ok_b) <= 2);
        CHECK(q.a == 255);
        canvas_free(cv);
    }
    free(px);
}

// The NEW tag branches on a LINEAR working canvas.  A CANVAS_CS_LINEAR_SRGB fill
// stores the linear value DIRECTLY (no transfer), so reading back -- which
// encodes linear->sRGB -- yields the sRGB-encoded byte of that linear value.
// The same numbers tagged CANVAS_CS_SRGB instead decode on entry, so the two
// tags land on visibly different bytes: that gap is the proof the tag routes the
// input, not the canvas.
static void linear_canvas_tag_branches(void) {
    int const w = 8, h = 8, len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return;
    }
    float const v = 0.5f;
    // LINEAR_SRGB tag: stored linear 0.5, read back as encode(0.5) ~ 188.
    int const want_linear_tag = (int)(cnvs_linear_to_srgb(v) * 255.0f + 0.5f);
    // SRGB tag (today's path): decode 0.5 -> linear, store, then encode on
    // readback cancels -> ~128.  Byte-identical to the legacy intern_color.
    int const want_srgb_tag = (int)(v * 255.0f + 0.5f);
    CHECK(abs(want_linear_tag - want_srgb_tag) > 20);  // the tags diverge

    struct canvas *__single cv = canvas_in_space(w, h, CANVAS_CS_LINEAR_SRGB);
    CHECK(cv != NULL);
    if (cv) {
        canvas_set_fill_rgba(cv, CANVAS_CS_LINEAR_SRGB, v, v, v, 1.0f);
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(abs((int)pixel_at(px, len, w, w / 2, h / 2).r - want_linear_tag) <= 2);

        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, v, v, v, 1.0f);
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        CHECK(abs((int)pixel_at(px, len, w, w / 2, h / 2).r - want_srgb_tag) <= 2);
        canvas_free(cv);
    }
    free(px);
}

// read_rgba's OUTPUT-space tag on a LINEAR working canvas.  The stored colour is
// linear, so CANVAS_CS_LINEAR_SRGB emits it RAW (quantized, no encode) while
// CANVAS_CS_SRGB emits the encoded byte -- the two diverge, and the gap is the
// proof the tag routes the readback, not the canvas.  A known linear value goes
// in (tagged LINEAR_SRGB so it stores verbatim), read back both ways.
static void read_rgba_output_space(void) {
    int const w = 8, h = 8, len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return;
    }
    struct canvas *__single cv = canvas_in_space(w, h, CANVAS_CS_LINEAR_SRGB);
    CHECK(cv != NULL);
    if (cv) {
        float const v = 0.5f;  // a linear value, stored verbatim (LINEAR_SRGB in)
        canvas_set_fill_rgba(cv, CANVAS_CS_LINEAR_SRGB, v, v, v, 1.0f);
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);

        // LINEAR_SRGB out: the raw stored linear value quantized -> ~128.
        canvas_read_rgba(cv, CANVAS_CS_LINEAR_SRGB, px, len);
        int const want_raw = (int)(v * 255.0f + 0.5f);
        CHECK(abs((int)pixel_at(px, len, w, w / 2, h / 2).r - want_raw) <= 2);

        // SRGB out: encode(0.5) ~ 188, the legacy linear-canvas readback.
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        int const want_enc = (int)(cnvs_linear_to_srgb(v) * 255.0f + 0.5f);
        CHECK(abs((int)pixel_at(px, len, w, w / 2, h / 2).r - want_enc) <= 2);

        CHECK(abs(want_raw - want_enc) > 20);  // the two output tags diverge
        canvas_free(cv);
    }
    free(px);
}

// put(CANVAS_CS_LINEAR_SRGB) -> read(CANVAS_CS_LINEAR_SRGB) on a linear canvas
// preserves the linear values within 8-bit tolerance: the put interprets the
// incoming bytes as linear and stores them verbatim (no transfer on a linear
// canvas), and the read emits them raw -- so the bytes survive the trip but for
// premultiply/quantize rounding.  A translucent source keeps premultiply in the
// loop.
static void linear_put_read_round_trip(void) {
    int const w = 16, h = 16, len = w * h * 4;
    uint8_t *__counted_by(len) src = malloc((size_t)len);
    uint8_t *__counted_by(len) out = malloc((size_t)len);
    CHECK(src != NULL && out != NULL);
    if (!src || !out) {
        free(src);
        free(out);
        return;
    }
    // A deterministic assortment of (linear-byte) colours, alpha included.
    for (int i = 0; i < w * h; i++) {
        src[i * 4 + 0] = (uint8_t)((i * 7) & 0xff);
        src[i * 4 + 1] = (uint8_t)((i * 13 + 40) & 0xff);
        src[i * 4 + 2] = (uint8_t)((i * 3 + 90) & 0xff);
        src[i * 4 + 3] = (uint8_t)(128 + ((i * 5) & 0x7f));  // 128..255, opaque-ish
    }
    struct canvas *__single cv = canvas_in_space(w, h, CANVAS_CS_LINEAR_SRGB);
    CHECK(cv != NULL);
    if (cv) {
        canvas_put_image_data(cv, CANVAS_CS_LINEAR_SRGB, src, len, w, h, 0, 0);
        canvas_read_rgba(cv, CANVAS_CS_LINEAR_SRGB, out, len);
        int maxd = 0;
        for (int i = 0; i < len; i++) {
            int const d = abs((int)src[i] - (int)out[i]);
            if (d > maxd) { maxd = d; }
        }
        CHECK(maxd <= 2);  // linear in, linear out: no transfer, only rounding
        canvas_free(cv);
    }
    free(src);
    free(out);
}

// The OKLAB pixel-I/O round trip: a known opaque sRGB colour put as sRGB, read
// back as OKLAB, then those Oklab bytes put back (as OKLAB) and read once more as
// sRGB -- the colour survives within 8-bit tolerance, exercising both new OKLAB
// branches (read working->linear->Oklab, put Oklab->linear->working) against the
// shared CNVS_OKLAB_AB_BIAS byte convention.  In-gamut so a,b stay inside the
// centred window the byte transport can represent.
static void oklab_pixel_io_round_trip(void) {
    int const w = 8, h = 8, len = w * h * 4;
    uint8_t *__counted_by(len) lab = malloc((size_t)len);
    uint8_t *__counted_by(len) back = malloc((size_t)len);
    CHECK(lab != NULL && back != NULL);
    if (!lab || !back) {
        free(lab);
        free(back);
        return;
    }
    int const R = 180, G = 96, B = 130;  // an in-gamut opaque sRGB colour
    struct canvas *__single cv = canvas_in_space(w, h, CANVAS_CS_SRGB);
    CHECK(cv != NULL);
    if (cv) {
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB,
                             (float)R / 255.0f, (float)G / 255.0f,
                             (float)B / 255.0f, 1.0f);
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);

        // Read back as Oklab bytes, push them onto a fresh canvas as Oklab, and
        // read THAT as sRGB -- the original colour should reappear.
        canvas_read_rgba(cv, CANVAS_CS_OKLAB, lab, len);
        struct canvas *__single cv2 = canvas_in_space(w, h, CANVAS_CS_SRGB);
        CHECK(cv2 != NULL);
        if (cv2) {
            canvas_put_image_data(cv2, CANVAS_CS_OKLAB, lab, len, w, h, 0, 0);
            canvas_read_rgba(cv2, CANVAS_CS_SRGB, back, len);
            struct rgba const p = pixel_at(back, len, w, w / 2, h / 2);
            CHECK(abs((int)p.r - R) <= 3);  // Oklab byte quantization tolerance
            CHECK(abs((int)p.g - G) <= 3);
            CHECK(abs((int)p.b - B) <= 3);
            CHECK(p.a == 255);
            canvas_free(cv2);
        }
        canvas_free(cv);
    }
    free(lab);
    free(back);
}

// The CANVAS_CS_SRGB pixel-I/O paths still match today, byte for byte: a put of
// known sRGB bytes then a read in sRGB returns them (on an sRGB canvas the store
// is a pass-through and the readback the SIMD bypass, the legacy identity).  This
// is the byte-identity anchor for the migrated call sites.
static void srgb_pixel_io_identity(void) {
    int const w = 16, h = 16, len = w * h * 4;
    uint8_t *__counted_by(len) src = malloc((size_t)len);
    uint8_t *__counted_by(len) out = malloc((size_t)len);
    CHECK(src != NULL && out != NULL);
    if (!src || !out) {
        free(src);
        free(out);
        return;
    }
    for (int i = 0; i < w * h; i++) {
        src[i * 4 + 0] = (uint8_t)((i * 11) & 0xff);
        src[i * 4 + 1] = (uint8_t)((i * 17 + 30) & 0xff);
        src[i * 4 + 2] = (uint8_t)((i * 5 + 70) & 0xff);
        src[i * 4 + 3] = 255;  // opaque: an unpremultiplied byte survives exactly
    }
    struct canvas *__single cv = canvas_in_space(w, h, CANVAS_CS_SRGB);
    CHECK(cv != NULL);
    if (cv) {
        canvas_put_image_data(cv, CANVAS_CS_SRGB, src, len, w, h, 0, 0);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, out, len);
        bool exact = true;
        for (int i = 0; i < len; i++) {
            if (src[i] != out[i]) { exact = false; break; }
        }
        CHECK(exact);  // sRGB canvas, sRGB I/O: byte-identical, no transfer
        canvas_free(cv);
    }
    free(src);
    free(out);
}

// Image sampling converts the resolved sample from the source's space into the
// working space -- the sampling counterpart to intern_color's colour-entry
// transfer.  Each case draws 1:1 (integer aligned, so the sample lands on a
// texel centre and the filter is exact), isolating the transfer.  An sRGB
// source on a linear canvas decodes on deposit, and the sRGB read-back encodes,
// so the byte returns to the source value; without the convert it would read
// far brighter (stored as if already linear).  A linear source on an sRGB canvas
// encodes.  A matched source is bit-exact.  A premultiplied translucent source
// exercises the unpremultiply / convert / re-premultiply path.
static void image_sample_space_convert(void) {
    int const w = 8, h = 8, len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return;
    }

    // 1. Opaque sRGB grey 128 on a LINEAR canvas: read back ~128, decisively not
    //    the ~188 an unconverted store would give.
    uint8_t src[8 * 8 * 4];
    for (int i = 0; i < w * h; i++) {
        src[i * 4 + 0] = 128;
        src[i * 4 + 1] = 128;
        src[i * 4 + 2] = 128;
        src[i * 4 + 3] = 255;
    }
    struct canvas *__single lin = canvas_in_space(w, h, CANVAS_CS_LINEAR_SRGB);
    CHECK(lin != NULL);
    if (lin) {
        canvas_draw_bitmap(lin, CANVAS_CS_SRGB, src, w, h, 0.0f, 0.0f);
        canvas_read_rgba(lin, CANVAS_CS_SRGB, px, len);
        struct rgba const p = pixel_at(px, len, w, w / 2, h / 2);
        CHECK(abs((int)p.r - 128) <= 2);
        CHECK(p.r < 170);  // not the unconverted ~188
        canvas_free(lin);
    }

    // 2. Matched: the SAME sRGB source on an sRGB canvas is byte-exact.
    struct canvas *__single srgb = canvas_in_space(w, h, CANVAS_CS_SRGB);
    CHECK(srgb != NULL);
    if (srgb) {
        canvas_draw_bitmap(srgb, CANVAS_CS_SRGB, src, w, h, 0.0f, 0.0f);
        canvas_read_rgba(srgb, CANVAS_CS_SRGB, px, len);
        struct rgba const p = pixel_at(px, len, w, w / 2, h / 2);
        CHECK(p.r == 128 && p.g == 128 && p.b == 128 && p.a == 255);
        canvas_free(srgb);
    }

    // 3. Opaque linear-grey 0.5 f16 image on an sRGB canvas: encode on deposit,
    //    read back ~188 (cnvs_linear_to_srgb(0.5)); unconverted it would be 128.
    _Float16 fpx[8 * 8 * 4];
    for (int i = 0; i < w * h; i++) {
        fpx[i * 4 + 0] = (_Float16)0.5f;
        fpx[i * 4 + 1] = (_Float16)0.5f;
        fpx[i * 4 + 2] = (_Float16)0.5f;
        fpx[i * 4 + 3] = (_Float16)1.0f;
    }
    int const want_enc = (int)(cnvs_linear_to_srgb(0.5f) * 255.0f + 0.5f);
    struct canvas_image *__single fimg =
        canvas_image_f16(CANVAS_CS_LINEAR_SRGB, fpx, w, h, CANVAS_ALPHA_UNPREMUL);
    struct canvas *__single sc = canvas_in_space(w, h, CANVAS_CS_SRGB);
    CHECK(fimg != NULL && sc != NULL);
    if (fimg && sc) {
        canvas_draw_image(sc, fimg, 0.0f, 0.0f);
        canvas_read_rgba(sc, CANVAS_CS_SRGB, px, len);
        struct rgba const p = pixel_at(px, len, w, w / 2, h / 2);
        CHECK(abs((int)p.r - want_enc) <= 2);
        CHECK(p.r > 160);  // not the unconverted 128
    }
    canvas_image_free(fimg);
    canvas_free(sc);

    // 4. Premultiplied translucent sRGB image (colour 204, alpha 128) on a
    //    LINEAR canvas over the transparent default: the unpremul/convert/
    //    re-premul path round-trips the colour back to ~204.
    uint8_t pm[8 * 8 * 4];
    int const col = 204, a = 128;
    int const pc = (int)((double)col * (double)a / 255.0 + 0.5);  // premultiplied store
    for (int i = 0; i < w * h; i++) {
        pm[i * 4 + 0] = (uint8_t)pc;
        pm[i * 4 + 1] = (uint8_t)pc;
        pm[i * 4 + 2] = (uint8_t)pc;
        pm[i * 4 + 3] = (uint8_t)a;
    }
    struct canvas_image *__single pimg =
        canvas_image_unorm8(CANVAS_CS_SRGB, pm, w, h, CANVAS_ALPHA_PREMUL);
    struct canvas *__single lc = canvas_in_space(w, h, CANVAS_CS_LINEAR_SRGB);
    CHECK(pimg != NULL && lc != NULL);
    if (pimg && lc) {
        canvas_draw_image(lc, pimg, 0.0f, 0.0f);
        canvas_read_rgba(lc, CANVAS_CS_SRGB, px, len);
        struct rgba const p = pixel_at(px, len, w, w / 2, h / 2);
        CHECK(abs((int)p.r - col) <= 4);
        CHECK(abs((int)p.a - a) <= 2);
    }
    canvas_image_free(pimg);
    canvas_free(lc);

    free(px);
}

// Known-answer checks for the BT.2100 output kernels.  PQ is checked against an
// independent double-precision evaluation using the spec's canonical fractional
// constants (a transcription error in the float constants would show up), at the
// endpoints, the reference-white region, and for clamping + monotonicity.  The
// Rec.2020 matrix is checked on white (must stay white -- rows sum to 1) and the
// pure linear primaries.
static void pq_rec2020_known_answers(void) {
    double const m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
    double const c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0,
                 c3 = 2392.0 / 4096.0 * 32.0;
    double const ys[] = { 0.0, 0.01, 0.0203, 0.1, 0.5, 1.0 };
    for (int i = 0; i < (int)(sizeof ys / sizeof ys[0]); i++) {
        double const y = ys[i];
        double const p = pow(y, m1);
        double const ref = pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
        CHECK(fabs((double)cnvs_pq_oetf((float)y) - ref) < 1e-3);
    }
    // PQ(0) is c1^m2 (~7e-7, rounds to code 0), not exactly zero; PQ(1) == 1.
    CHECK(cnvs_pq_oetf(0.0f) >= 0.0f && cnvs_pq_oetf(0.0f) < 1e-5f);
    CHECK(fabsf(cnvs_pq_oetf(1.0f) - 1.0f) < 1e-4f);
    CHECK(cnvs_pq_oetf(-0.5f) >= 0.0f && cnvs_pq_oetf(-0.5f) < 1e-5f);  // clamps below
    CHECK(fabsf(cnvs_pq_oetf(2.0f) - 1.0f) < 1e-4f);   // clamps above
    CHECK(cnvs_pq_oetf(0.1f) > cnvs_pq_oetf(0.01f));   // monotone
    CHECK(cnvs_pq_oetf(0.01f) > cnvs_pq_oetf(0.001f));

    cnvs_rgb const wht = cnvs_linear_srgb_to_rec2020((cnvs_rgb){ 1.0f, 1.0f, 1.0f });
    CHECK(fabsf(wht.r - 1.0f) < 1e-3f && fabsf(wht.g - 1.0f) < 1e-3f &&
          fabsf(wht.b - 1.0f) < 1e-3f);
    cnvs_rgb const red = cnvs_linear_srgb_to_rec2020((cnvs_rgb){ 1.0f, 0.0f, 0.0f });
    CHECK(fabsf(red.r - 0.6274039f) < 1e-3f && fabsf(red.g - 0.0690973f) < 1e-3f &&
          fabsf(red.b - 0.0163914f) < 1e-3f);
    cnvs_rgb const grn = cnvs_linear_srgb_to_rec2020((cnvs_rgb){ 0.0f, 1.0f, 0.0f });
    CHECK(fabsf(grn.r - 0.3292830f) < 1e-3f && fabsf(grn.g - 0.9195404f) < 1e-3f &&
          fabsf(grn.b - 0.0880133f) < 1e-3f);
    CHECK(red.g >= 0.0f && red.b >= 0.0f);  // sRGB gamut is inside Rec.2020

    // The 2020->709 inverse round-trips, and a Rec.2020 primary lands outside
    // [0,1] in linear sRGB (a negative off-axis component -- wide gamut).
    cnvs_rgb const cols[] = { { 0.8f, 0.3f, 0.1f }, { 0.1f, 0.7f, 0.4f }, { 0.2f, 0.2f, 0.9f } };
    for (int i = 0; i < (int)(sizeof cols / sizeof cols[0]); i++) {
        cnvs_rgb const rt = cnvs_linear_srgb_to_rec2020(cnvs_rec2020_to_linear_srgb(cols[i]));
        CHECK(fabsf(rt.r - cols[i].r) < 1e-3f && fabsf(rt.g - cols[i].g) < 1e-3f &&
              fabsf(rt.b - cols[i].b) < 1e-3f);
    }
    cnvs_rgb const g2020 = cnvs_rec2020_to_linear_srgb((cnvs_rgb){ 0.0f, 1.0f, 0.0f });
    CHECK(g2020.r < 0.0f && g2020.b < 0.0f);  // outside the sRGB gamut
}

int main(void) {
    space_default_and_persistence();
    pq_rec2020_known_answers();
    image_sample_space_convert();
    linear_color_round_trip();
    linear_image_data_round_trip();
    linear_source_over_oracle();
    linear_multiply_differs();
    linear_blend_modes_oracle();
    srgb_canvas_tag_branches();
    linear_canvas_tag_branches();
    read_rgba_output_space();
    linear_put_read_round_trip();
    oklab_pixel_io_round_trip();
    srgb_pixel_io_identity();
    return TEST_REPORT();
}
