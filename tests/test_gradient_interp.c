// Gradient interpolation as TWO orthogonal knobs (src/canvas2d_gradient.c): the
// colour SPACE {sRGB, linear sRGB, Oklab} and the colour-coordinate ALPHA
// {unpremul, premul}.  Claims:
//
//   1. The DEFAULT (sRGB + unpremul) is byte-identical -- a gradient left at
//      the default evaluates exactly as the legacy straight stored-value lerp,
//      at tolerance ZERO.  This is the byte-identity path every existing scene
//      depends on.
//   2. Each interpolation SPACE matches a DOUBLE-precision reference of the
//      same pipeline (working -> space -> [premul] lerp -> [unpremul] -> back)
//      within the f16 handback's budget, in both alpha modes.
//   3. premul vs unpremul DIFFER on an alpha-varying ramp: an opaque-blue ->
//      transparent-red fade carries red into the mid-ramp under unpremul (a
//      muddy tint) but never under premul (clean -- blue dominates throughout,
//      the red weighted to nothing).
//   4. canvas2d_gradient_color_row (planar) == canvas2d_gradient_color_at (scalar)
//      bit-for-bit on every non-default combo, including the t<0 sentinel and
//      the n%8 tail -- the determinism the replay gate depends on.
//
// Internal header, like test_gradient_solve.c.

#include "test_util.h"

#include "canvas2d_gradient.h"
#include "canvas2d_math.h"

#include <math.h>
#include <string.h>

// ---- A double-precision mirror of the colour pipeline canvas2d_color.c runs in
//      f32.  Used only to bound the evaluator's error; the totality tricks
//      (odd-extension transfer, cbrt) are irrelevant here -- the test stops are
//      all in [0,1] -- but spelled the same so the reference is the real
//      algorithm at higher precision.

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

// One stop colour (WORKING space `linear` flag) to the chosen interpolation
// SPACE's coordinates, in double.  Mirrors canvas2d_gradient.c's stop_to_interp.
static rgb_d stop_to_interp_d(canvas2d_unpremul c, enum canvas2d_color_space interp, bool linear) {
    if (interp == CANVAS2D_CS_SRGB) {
        return (rgb_d){ (double)c.r, (double)c.g, (double)c.b };
    }
    rgb_d const lin = linear
        ? (rgb_d){ (double)c.r, (double)c.g, (double)c.b }
        : (rgb_d){ srgb_to_linear_d((double)c.r), srgb_to_linear_d((double)c.g),
                   srgb_to_linear_d((double)c.b) };
    if (interp == CANVAS2D_CS_LINEAR_SRGB) {
        return (rgb_d){ lin.r, lin.g, lin.b };
    }
    oklab_d const lab = lin_to_oklab_d(lin);
    return (rgb_d){ lab.L, lab.a, lab.b };
}

// The chosen interp coords + alpha back to the working space, in double.
// Mirrors canvas2d_gradient.c's interp_to_stop.
static rgb_d interp_to_stop_d(rgb_d c, enum canvas2d_color_space interp, bool linear) {
    if (interp == CANVAS2D_CS_SRGB) {
        return c;
    }
    rgb_d const lin = interp == CANVAS2D_CS_LINEAR_SRGB
        ? c
        : oklab_to_lin_d((oklab_d){ c.r, c.g, c.b });
    if (linear) {
        return (rgb_d){ lin.r, lin.g, lin.b };
    }
    return (rgb_d){ linear_to_srgb_d(lin.r), linear_to_srgb_d(lin.g),
                    linear_to_srgb_d(lin.b) };
}

// The reference general lerp of two stop colours at parameter u, in double.
// Mirrors canvas2d_gradient.c's general_lerp exactly.
static void ref_general_lerp(canvas2d_unpremul lo, canvas2d_unpremul hi, double u,
                             enum canvas2d_color_space interp, enum canvas2d_alpha_type alpha,
                             bool linear, double *__counted_by(4) out) {
    rgb_d const lc = stop_to_interp_d(lo, interp, linear);
    rgb_d const hc = stop_to_interp_d(hi, interp, linear);
    double const la = (double)lo.a, ha = (double)hi.a;
    double const a  = la + (ha - la) * u;

    rgb_d res;
    if (alpha == CANVAS2D_ALPHA_PREMUL) {
        double const px = lc.r * la + (hc.r * ha - lc.r * la) * u;
        double const py = lc.g * la + (hc.g * ha - lc.g * la) * u;
        double const pz = lc.b * la + (hc.b * ha - lc.b * la) * u;
        double const inv = a > 0.0 ? 1.0 / a : 0.0;
        res = (rgb_d){ px * inv, py * inv, pz * inv };
    } else {
        res = (rgb_d){ lc.r + (hc.r - lc.r) * u, lc.g + (hc.g - lc.g) * u,
                       lc.b + (hc.b - lc.b) * u };
    }
    rgb_d const back = interp_to_stop_d(res, interp, linear);
    out[0] = back.r; out[1] = back.g; out[2] = back.b; out[3] = a;
}

// The f16-handback budget against the double reference.  The conversion does a
// double round-trip through the transfer plus the cbrt/cube, narrowing once to
// _Float16; 2/255 leaves margin over the worst f16 colour step and the transfer
// derivative near 0.
#define ERR_TOL (2.0 / 255.0)

static bool dnear(double a, double b, double tol) { return fabs(a - b) <= tol; }

// ---- Claim 1: the default (sRGB + unpremul) is byte-identical.
static void default_identical(void) {
    // A gradient left at the default interp (zero -> sRGB + unpremul) must equal
    // a plain component lerp of the stops, bit for bit.
    struct canvas2d_gradient g = { .kind = CANVAS2D_GRAD_LINEAR, .p1 = { .x = 1.0f } };
    canvas2d_gradient_add_stop(&g, 0.00f, canvas2d_unpremul_of(0.90f, 0.20f, 0.25f, 1.0f));
    canvas2d_gradient_add_stop(&g, 0.50f, canvas2d_unpremul_of(0.10f, 0.80f, 0.20f, 1.0f));
    canvas2d_gradient_add_stop(&g, 1.00f, canvas2d_unpremul_of(0.15f, 0.25f, 0.90f, 0.4f));
    CHECK(g.interp == CANVAS2D_CS_SRGB);                 // designated-init defaults
    CHECK(g.interp_alpha == CANVAS2D_ALPHA_UNPREMUL);

    for (int i = 0; i <= 64; i++) {
        float const t = (float)i / 64.0f;
        canvas2d_unpremul const got = canvas2d_gradient_color_at(&g, t);
        // The exact component lerp the original code did (half4, narrow once).
        canvas2d_unpremul want;
        int const n = g.stop_count;
        if (t <= g.stops[0].offset)            want = g.stops[0].color;
        else if (t >= g.stops[n - 1].offset)   want = g.stops[n - 1].color;
        else {
            for (int s = 0; s + 1 < n; s++) {
                canvas2d_stop const lo = g.stops[s], hi = g.stops[s + 1];
                if (t <= hi.offset) {
                    float const span = hi.offset - lo.offset;
                    float const u = span > 0.0f ? (t - lo.offset) / span : 0.0f;
                    half4 const lov = { lo.color.r, lo.color.g, lo.color.b, lo.color.a };
                    half4 const hiv = { hi.color.r, hi.color.g, hi.color.b, hi.color.a };
                    half4 const c = lov + (hiv - lov) * (_Float16)u;
                    want = (canvas2d_unpremul){ .r = c[0], .g = c[1], .b = c[2], .a = c[3] };
                    break;
                }
            }
        }
        CHECK(memcmp(&got, &want, sizeof want) == 0);  // tolerance ZERO
    }

    // And the planar row is byte-identical to the scalar on the default too.
    enum { N = 67 };
    static float t[N];
    static canvas2d_unpremul row[N];
    for (int i = 0; i < N; i++) t[i] = (float)i / (float)(N - 1);
    canvas2d_gradient_color_row(&g, t, N, row);
    for (int i = 0; i < N; i++) {
        canvas2d_unpremul const want = canvas2d_gradient_color_at(&g, t[i]);
        CHECK(memcmp(&row[i], &want, sizeof want) == 0);
    }
}

// ---- Claim 2: a ramp matches the double reference (any space x alpha).
static void matches_reference(struct canvas2d_gradient const *gr) {
    bool const linear = gr->space == CANVAS2D_CS_LINEAR_SRGB;
    for (int i = 0; i <= 200; i++) {
        float const t = (float)i / 200.0f;
        canvas2d_unpremul const got = canvas2d_gradient_color_at(gr, t);
        // Resolve the surrounding stop pair + u exactly as the evaluator does.
        int const n = gr->stop_count;
        double want[4];
        if (t <= gr->stops[0].offset) {
            canvas2d_unpremul c = gr->stops[0].color;
            want[0] = (double)c.r; want[1] = (double)c.g;
            want[2] = (double)c.b; want[3] = (double)c.a;
        } else if (t >= gr->stops[n - 1].offset) {
            canvas2d_unpremul c = gr->stops[n - 1].color;
            want[0] = (double)c.r; want[1] = (double)c.g;
            want[2] = (double)c.b; want[3] = (double)c.a;
        } else {
            for (int s = 0; s + 1 < n; s++) {
                canvas2d_stop const lo = gr->stops[s], hi = gr->stops[s + 1];
                if (t <= hi.offset) {
                    float const span = hi.offset - lo.offset;
                    double const u = span > 0.0f ? (double)((t - lo.offset) / span) : 0.0;
                    ref_general_lerp(lo.color, hi.color, u, gr->interp, gr->interp_alpha,
                                     linear, want);
                    break;
                }
            }
        }
        CHECK(dnear((double)got.r, want[0], ERR_TOL) &&
              dnear((double)got.g, want[1], ERR_TOL) &&
              dnear((double)got.b, want[2], ERR_TOL) &&
              dnear((double)got.a, want[3], ERR_TOL));
    }
}

// ---- Claim 4: planar row == scalar, bit for bit, on a non-default gradient.
static void row_matches_scalar(struct canvas2d_gradient const *gr) {
    enum { N = 1029 };  // N % 8 == 5: the 8-wide body and the scalar tail both run
    static float t[N];
    static canvas2d_unpremul row[N];
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
    canvas2d_gradient_color_row(gr, t, N, row);
    for (int i = 0; i < N; i++) {
        canvas2d_unpremul want = t[i] >= 0.0f
            ? canvas2d_gradient_color_at(gr, t[i])
            : canvas2d_unpremul_of(0.0f, 0.0f, 0.0f, 0.0f);
        CHECK(memcmp(&row[i], &want, sizeof want) == 0);
    }
}

// Build a three-stop red->green->blue gradient with the given interp config.
static struct canvas2d_gradient make_rgb_ramp(enum canvas2d_color_space interp,
                                          enum canvas2d_alpha_type alpha,
                                          enum canvas2d_color_space space) {
    struct canvas2d_gradient g = { .kind = CANVAS2D_GRAD_LINEAR, .p1 = { .x = 1.0f },
                               .interp = interp, .interp_alpha = alpha, .space = space };
    canvas2d_gradient_add_stop(&g, 0.00f, canvas2d_unpremul_of(0.90f, 0.10f, 0.10f, 1.0f));
    canvas2d_gradient_add_stop(&g, 0.50f, canvas2d_unpremul_of(0.10f, 0.80f, 0.20f, 1.0f));
    canvas2d_gradient_add_stop(&g, 1.00f, canvas2d_unpremul_of(0.15f, 0.25f, 0.90f, 1.0f));
    return g;
}

int main(void) {
    // 1. Default (sRGB + unpremul) byte-identical to the legacy straight lerp.
    default_identical();

    // 2. Each interpolation SPACE matches the double reference, in BOTH alpha
    //    modes, on both sRGB and linear working spaces.  (At alpha == 1 here
    //    premul == unpremul, but the premul branch still runs its own code, so
    //    exercising it pins that arithmetic too.)
    enum canvas2d_color_space const spaces[3] = {
        CANVAS2D_CS_SRGB, CANVAS2D_CS_LINEAR_SRGB, CANVAS2D_CS_OKLAB,
    };
    enum canvas2d_alpha_type const alphas[2] = {
        CANVAS2D_ALPHA_UNPREMUL, CANVAS2D_ALPHA_PREMUL,
    };
    enum canvas2d_color_space const works[2] = {
        CANVAS2D_CS_SRGB, CANVAS2D_CS_LINEAR_SRGB,
    };
    for (int si = 0; si < 3; si++) {
        for (int ai = 0; ai < 2; ai++) {
            for (int wi = 0; wi < 2; wi++) {
                struct canvas2d_gradient g = make_rgb_ramp(spaces[si], alphas[ai], works[wi]);
                matches_reference(&g);
                // The non-default combos route the row through the scalar path;
                // pin that bit-identity (the default's row is checked in claim 1).
                if (!(spaces[si] == CANVAS2D_CS_SRGB && alphas[ai] == CANVAS2D_ALPHA_UNPREMUL)) {
                    row_matches_scalar(&g);
                }
            }
        }
    }

    // 3. premul vs unpremul DIFFER on an alpha-varying ramp, premul clean.
    //    Opaque blue -> transparent red: under unpremul the red rides the
    //    coords into the mid-ramp; under premul the vanishing red contributes
    //    nothing, so blue dominates everywhere.
    for (int si = 0; si < 3; si++) {
        struct canvas2d_gradient un = { .kind = CANVAS2D_GRAD_LINEAR, .p1 = { .x = 1.0f },
                                    .interp = spaces[si], .interp_alpha = CANVAS2D_ALPHA_UNPREMUL,
                                    .space = CANVAS2D_CS_SRGB };
        struct canvas2d_gradient pm = un;
        pm.interp_alpha = CANVAS2D_ALPHA_PREMUL;
        canvas2d_gradient_add_stop(&un, 0.0f, canvas2d_unpremul_of(0.15f, 0.25f, 0.90f, 1.0f));  // opaque blue
        canvas2d_gradient_add_stop(&un, 1.0f, canvas2d_unpremul_of(0.90f, 0.15f, 0.15f, 0.0f));  // transparent red
        canvas2d_gradient_add_stop(&pm, 0.0f, canvas2d_unpremul_of(0.15f, 0.25f, 0.90f, 1.0f));
        canvas2d_gradient_add_stop(&pm, 1.0f, canvas2d_unpremul_of(0.90f, 0.15f, 0.15f, 0.0f));

        // The reference matches both, and the planar row matches the scalar.
        matches_reference(&un);
        matches_reference(&pm);
        row_matches_scalar(&un);
        row_matches_scalar(&pm);

        bool any_diff = false;
        for (int i = 1; i < 100; i++) {  // skip t==0 (alpha 1, equal) and t==1 (alpha 0)
            float const t = (float)i / 100.0f;
            canvas2d_unpremul const u = canvas2d_gradient_color_at(&un, t);
            canvas2d_unpremul const p = canvas2d_gradient_color_at(&pm, t);
            // Premul keeps the colour clean blue: b dominates r at every point.
            CHECK((float)p.b > (float)p.r);
            // Alpha is identical (always lerps linearly, unpremultiplied).
            CHECK(dnear((double)u.a, (double)p.a, 1.0 / 255.0));
            if (memcmp(&u, &p, sizeof u) != 0) any_diff = true;
        }
        CHECK(any_diff);  // the two modes really diverge on the colour

        // The unpremul midpoint drags red in: its r is materially larger than
        // the premul midpoint's r (the bleed made concrete).
        canvas2d_unpremul const um = canvas2d_gradient_color_at(&un, 0.5f);
        canvas2d_unpremul const pmid = canvas2d_gradient_color_at(&pm, 0.5f);
        CHECK((float)um.r > (float)pmid.r + 0.05f);
        // Alpha at the midpoint is ~0.5 in both (linear, unpremultiplied).
        CHECK(dnear((double)um.a, 0.5, 2.0 / 255.0));
        CHECK(dnear((double)pmid.a, 0.5, 2.0 / 255.0));
    }

    return TEST_REPORT();
}
