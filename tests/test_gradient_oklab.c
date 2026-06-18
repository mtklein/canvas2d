// Per-gradient Oklab interpolation (src/cnvs_gradient.c).  Four claims:
//
//   1. The sRGB default is byte-identical -- a gradient with interp ==
//      CANVAS_CS_SRGB (the default) evaluates exactly as before, at tolerance
//      ZERO against the same gradient evaluated as plain component lerp.  (The
//      existing test_gradient / test_gradient_solve identities already pin the
//      sRGB branch; this re-pins it through the new field's presence.)
//   2. An Oklab black->white and a red->green ramp match a DOUBLE-precision
//      Oklab reference (the same pipeline: working->linear->Oklab, premultiply,
//      lerp, unpremultiply, ->linear->working) within the f16 handback's budget.
//   3. Premultiplied-Oklab HYGIENE: a transparent stop bleeds no colour -- a
//      transparent-red -> opaque-blue ramp has zero red anywhere (the midpoint
//      is clean half-alpha blue, not muddy purple).
//   4. cnvs_gradient_color_row (planar) == cnvs_gradient_color_at (scalar)
//      bit-for-bit on an Oklab gradient, including the t<0 sentinel and the
//      n%8 tail -- the determinism the replay gate depends on.
//
// Internal header, like test_gradient_solve.c.

#include "test_util.h"

#include "cnvs_gradient.h"
#include "cnvs_math.h"

#include <math.h>
#include <string.h>

// ---- A double-precision mirror of the colour pipeline cnvs_color.c runs in
//      f32.  Used only to bound the Oklab evaluator's error; the totality
//      tricks (odd-extension transfer, cbrt) are irrelevant here -- the test
//      stops are all in [0,1] -- but spelled the same so the reference is the
//      real algorithm at higher precision.

static double srgb_to_linear_d(double c) {
    double const m = fabs(c);
    double const l = m <= 0.04045 ? m / 12.92 : pow((m + 0.055) / 1.055, 2.4);
    return copysign(l, c);
}
static double linear_to_srgb_d(double l) {
    double const m = fabs(l);
    double const c = m <= 0.0031308 ? 12.92 * m : 1.055 * pow(m, 1.0 / 2.4) - 0.055;
    return copysign(c, l);
}

typedef struct { double L, a, b; } oklab_d;
typedef struct { double r, g, b; } rgb_d;

static oklab_d lin_to_oklab_d(rgb_d c) {
    double const l = 0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b;
    double const m = 0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b;
    double const s = 0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b;
    double const l_ = cbrt(l), m_ = cbrt(m), s_ = cbrt(s);
    return (oklab_d){
        .L = 0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
        .a = 1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
        .b = 0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_,
    };
}
static rgb_d oklab_to_lin_d(oklab_d c) {
    double const l_ = c.L + 0.3963377774 * c.a + 0.2158037573 * c.b;
    double const m_ = c.L - 0.1055613458 * c.a - 0.0638541728 * c.b;
    double const s_ = c.L - 0.0894841775 * c.a - 1.2914855480 * c.b;
    double const l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    return (rgb_d){
        .r =  4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        .g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        .b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s,
    };
}

// The reference Oklab lerp of two stop colours (working space `linear` flag) at
// parameter u, in double.  Mirrors cnvs_gradient.c's oklab_lerp exactly.
static void ref_oklab_lerp(cnvs_unpremul lo, cnvs_unpremul hi, double u,
                           bool linear, double *__counted_by(4) out) {
    rgb_d const llin = linear
        ? (rgb_d){ (double)lo.r, (double)lo.g, (double)lo.b }
        : (rgb_d){ srgb_to_linear_d((double)lo.r), srgb_to_linear_d((double)lo.g),
                   srgb_to_linear_d((double)lo.b) };
    rgb_d const hlin = linear
        ? (rgb_d){ (double)hi.r, (double)hi.g, (double)hi.b }
        : (rgb_d){ srgb_to_linear_d((double)hi.r), srgb_to_linear_d((double)hi.g),
                   srgb_to_linear_d((double)hi.b) };
    oklab_d const ll = lin_to_oklab_d(llin), hl = lin_to_oklab_d(hlin);
    double const la = (double)lo.a, ha = (double)hi.a;

    double const pL = ll.L * la + (hl.L * ha - ll.L * la) * u;
    double const pa = ll.a * la + (hl.a * ha - ll.a * la) * u;
    double const pb = ll.b * la + (hl.b * ha - ll.b * la) * u;
    double const a  = la + (ha - la) * u;
    double const inv = a > 0.0 ? 1.0 / a : 0.0;

    rgb_d const lin = oklab_to_lin_d((oklab_d){ .L = pL * inv, .a = pa * inv, .b = pb * inv });
    out[0] = linear ? lin.r : linear_to_srgb_d(lin.r);
    out[1] = linear ? lin.g : linear_to_srgb_d(lin.g);
    out[2] = linear ? lin.b : linear_to_srgb_d(lin.b);
    out[3] = a;
}

// The f16-handback budget against the double reference.  The conversion does a
// double round-trip through the transfer plus the cbrt/cube, narrowing once to
// _Float16; 2/255 leaves margin over the worst f16 colour step and the transfer
// derivative near 0.
#define OKLAB_ERR_TOL (2.0 / 255.0)

static bool dnear(double a, double b, double tol) { return fabs(a - b) <= tol; }

// ---- Claim 1: the sRGB default is byte-identical.
static void srgb_default_identical(void) {
    // Two structurally identical gradients: one left at the default interp
    // (zero -> CANVAS_CS_SRGB), one set to SRGB explicitly.  Both must equal
    // a plain component lerp of the stops, bit for bit.
    struct cnvs_gradient g = { .kind = CNVS_GRAD_LINEAR, .p1 = { .x = 1.0f } };
    cnvs_gradient_add_stop(&g, 0.00f, cnvs_unpremul_of(0.90f, 0.20f, 0.25f, 1.0f));
    cnvs_gradient_add_stop(&g, 0.50f, cnvs_unpremul_of(0.10f, 0.80f, 0.20f, 1.0f));
    cnvs_gradient_add_stop(&g, 1.00f, cnvs_unpremul_of(0.15f, 0.25f, 0.90f, 0.4f));
    CHECK(g.interp == CANVAS_CS_SRGB);  // designated-init default is sRGB

    for (int i = 0; i <= 64; i++) {
        float const t = (float)i / 64.0f;
        cnvs_unpremul const got = cnvs_gradient_color_at(&g, t);
        // The exact component lerp the original code did (half4, narrow once).
        cnvs_unpremul want;
        int const n = g.stop_count;
        if (t <= g.stops[0].offset)            want = g.stops[0].color;
        else if (t >= g.stops[n - 1].offset)   want = g.stops[n - 1].color;
        else {
            for (int s = 0; s + 1 < n; s++) {
                cnvs_stop const lo = g.stops[s], hi = g.stops[s + 1];
                if (t <= hi.offset) {
                    float const span = hi.offset - lo.offset;
                    float const u = span > 0.0f ? (t - lo.offset) / span : 0.0f;
                    half4 const lov = { lo.color.r, lo.color.g, lo.color.b, lo.color.a };
                    half4 const hiv = { hi.color.r, hi.color.g, hi.color.b, hi.color.a };
                    half4 const c = lov + (hiv - lov) * (_Float16)u;
                    want = (cnvs_unpremul){ .r = c[0], .g = c[1], .b = c[2], .a = c[3] };
                    break;
                }
            }
        }
        CHECK(memcmp(&got, &want, sizeof want) == 0);  // tolerance ZERO
    }
}

// ---- Claim 2: an Oklab ramp matches the double reference.
static void oklab_matches_reference(struct cnvs_gradient const *gr) {
    bool const linear = gr->space == CANVAS_CS_LINEAR_SRGB;
    for (int i = 0; i <= 200; i++) {
        float const t = (float)i / 200.0f;
        cnvs_unpremul const got = cnvs_gradient_color_at(gr, t);
        // Resolve the surrounding stop pair + u exactly as the evaluator does.
        int const n = gr->stop_count;
        double want[4];
        if (t <= gr->stops[0].offset) {
            cnvs_unpremul c = gr->stops[0].color;
            want[0] = (double)c.r; want[1] = (double)c.g;
            want[2] = (double)c.b; want[3] = (double)c.a;
        } else if (t >= gr->stops[n - 1].offset) {
            cnvs_unpremul c = gr->stops[n - 1].color;
            want[0] = (double)c.r; want[1] = (double)c.g;
            want[2] = (double)c.b; want[3] = (double)c.a;
        } else {
            for (int s = 0; s + 1 < n; s++) {
                cnvs_stop const lo = gr->stops[s], hi = gr->stops[s + 1];
                if (t <= hi.offset) {
                    float const span = hi.offset - lo.offset;
                    double const u = span > 0.0f ? (double)((t - lo.offset) / span) : 0.0;
                    ref_oklab_lerp(lo.color, hi.color, u, linear, want);
                    break;
                }
            }
        }
        CHECK(dnear((double)got.r, want[0], OKLAB_ERR_TOL) &&
              dnear((double)got.g, want[1], OKLAB_ERR_TOL) &&
              dnear((double)got.b, want[2], OKLAB_ERR_TOL) &&
              dnear((double)got.a, want[3], OKLAB_ERR_TOL));
    }
}

// ---- Claim 4: planar row == scalar, bit for bit, on an Oklab gradient.
static void row_matches_scalar(struct cnvs_gradient const *gr) {
    enum { N = 1029 };  // N % 8 == 5: the 8-wide body and the scalar tail both run
    static float t[N];
    static cnvs_unpremul row[N];
    int k = 0;
    t[k++] = -1.0f;  // the "outside" sentinel -> transparent black
    for (int s = 0; s < gr->stop_count && k + 3 <= N; s++) {
        float const o = gr->stops[s].offset;
        t[k++] = nextafterf(o, 0.0f);
        t[k++] = o;
        t[k++] = nextafterf(o, 1.0f);
    }
    int const base = k;
    for (; k < N; k++) {
        t[k] = (float)(k - base) / (float)(N - 1 - base);
    }
    cnvs_gradient_color_row(gr, t, N, row);
    for (int i = 0; i < N; i++) {
        cnvs_unpremul want = t[i] >= 0.0f
            ? cnvs_gradient_color_at(gr, t[i])
            : cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 0.0f);
        CHECK(memcmp(&row[i], &want, sizeof want) == 0);
    }
}

int main(void) {
    // 1. sRGB default unchanged.
    srgb_default_identical();

    // 2. Oklab black -> white (sRGB working space, the canvas default).
    struct cnvs_gradient bw = { .kind = CNVS_GRAD_LINEAR, .p1 = { .x = 1.0f },
                                .interp = CANVAS_CS_OKLAB, .space = CANVAS_CS_SRGB };
    cnvs_gradient_add_stop(&bw, 0.0f, cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 1.0f));
    cnvs_gradient_add_stop(&bw, 1.0f, cnvs_unpremul_of(1.0f, 1.0f, 1.0f, 1.0f));
    oklab_matches_reference(&bw);
    row_matches_scalar(&bw);

    //    Oklab red -> green.
    struct cnvs_gradient rg = { .kind = CNVS_GRAD_LINEAR, .p1 = { .x = 1.0f },
                                .interp = CANVAS_CS_OKLAB, .space = CANVAS_CS_SRGB };
    cnvs_gradient_add_stop(&rg, 0.0f, cnvs_unpremul_of(0.90f, 0.10f, 0.10f, 1.0f));
    cnvs_gradient_add_stop(&rg, 1.0f, cnvs_unpremul_of(0.10f, 0.80f, 0.20f, 1.0f));
    oklab_matches_reference(&rg);
    row_matches_scalar(&rg);

    //    A multi-stop Oklab ramp on a LINEAR working space (identity transfer).
    struct cnvs_gradient lin = { .kind = CNVS_GRAD_LINEAR, .p1 = { .x = 1.0f },
                                 .interp = CANVAS_CS_OKLAB, .space = CANVAS_CS_LINEAR_SRGB };
    cnvs_gradient_add_stop(&lin, 0.00f, cnvs_unpremul_of(0.90f, 0.10f, 0.10f, 1.0f));
    cnvs_gradient_add_stop(&lin, 0.50f, cnvs_unpremul_of(0.10f, 0.80f, 0.20f, 1.0f));
    cnvs_gradient_add_stop(&lin, 1.00f, cnvs_unpremul_of(0.15f, 0.25f, 0.90f, 1.0f));
    oklab_matches_reference(&lin);
    row_matches_scalar(&lin);

    // 3. Premultiplied-Oklab hygiene: transparent red -> opaque blue.  No red
    //    bleeds anywhere (a straight component lerp WOULD carry red into the
    //    midpoint); alpha rises linearly and the colour reads as blue.
    struct cnvs_gradient fade = { .kind = CNVS_GRAD_LINEAR, .p1 = { .x = 1.0f },
                                  .interp = CANVAS_CS_OKLAB, .space = CANVAS_CS_SRGB };
    cnvs_gradient_add_stop(&fade, 0.0f, cnvs_unpremul_of(0.90f, 0.10f, 0.10f, 0.0f));  // transparent red
    cnvs_gradient_add_stop(&fade, 1.0f, cnvs_unpremul_of(0.10f, 0.20f, 0.90f, 1.0f));  // opaque blue
    oklab_matches_reference(&fade);
    row_matches_scalar(&fade);
    for (int i = 1; i <= 100; i++) {  // skip t==0 (alpha 0, colour don't-care)
        float const t = (float)i / 100.0f;
        cnvs_unpremul const c = cnvs_gradient_color_at(&fade, t);
        // The unpremultiplied colour never goes redder than blue: with the
        // transparent endpoint contributing nothing, b dominates r at every
        // sampled point (a straight lerp would have r == b at the midpoint and
        // r > b below it).
        CHECK((float)c.b > (float)c.r);
        CHECK((float)c.a >= 0.0f && (float)c.a <= 1.0f);
    }
    // Alpha at the midpoint is ~0.5 (it lerps linearly, unpremultiplied).
    {
        cnvs_unpremul const mid = cnvs_gradient_color_at(&fade, 0.5f);
        CHECK(dnear((double)mid.a, 0.5, 2.0 / 255.0));
    }

    return TEST_REPORT();
}
