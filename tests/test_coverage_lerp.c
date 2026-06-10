// The coverage-lerp oracle (docs/rasterization.md §3.8): partial coverage
// composites as out = lerp(dst, blend(src, dst), cov) -- the uncovered
// fraction of a pixel keeps its destination.  This test pins that ruling for
// EVERY one of the 26 modes against a double-precision reference, two ways,
// and also pins that the ruling beats the pipeline's old semantics (coverage
// folded into source alpha everywhere) exactly where the fold is wrong.
//
// Where the fold is wrong, precisely.  Folding coverage c into the
// premultiplied source (s, sa) -> (c*s, c*sa) equals the lerp when
//   - Porter-Duff co = Fa*s + Fb*d:  Fa free of sa and Fb affine in sa with
//     Fb(0) = 1 -- source-over, destination-over, destination-out,
//     source-atop, xor.  (lighter passes this criterion too, but only in
//     unclamped arithmetic: its output clamp breaks the equivalence, so it
//     takes the lerp -- see EXPECT_CLOSER below.)
//   - Blends co = s*(1-da) + d*(1-sa) + T:  T(c*s, c*sa, d, da) =
//     c*T(s, sa, d, da), i.e. T degree-1 homogeneous in (s, sa).  The W3C
//     premultiplied T = sa*da*B(d/da, s/sa) IS homogeneous (B's arguments are
//     scale-invariant), so for all 15 blend modes fold == lerp in exact
//     arithmetic and the two differ only in f16 rounding.
//   - Copy, source-in, source-out, destination-in, destination-atop fail the
//     criterion outright (Fb(0) = 0): the fold scales the destination away
//     where the shape doesn't cover -- at cov = 0 destination-in cleared
//     every bbox pixel the shape never touched.  These are the modes the
//     lerp genuinely fixes, and EXPECT_CLOSER asserts the fix.
//
// Part A drives the compositor directly: for (mode x src x dst), a 256-pixel
// row sweeping every coverage byte, three ways -- NEW (full-strength tile +
// coverage plane, the shipping path), OLD (coverage folded into the tile by
// the old shade arithmetic, f32 fold then f16 premultiply, composited with no
// plane), REF (the double-precision lerp).  Part B is end-to-end: two small
// AA shapes -- a disc edge (a 64-gon, so the reference shares the renderer's
// exact geometry; the 0.25px arc flattener would otherwise dominate) and a
// thin diagonal quad -- under every mode over an opaque and a translucent
// backdrop, against a 16x16-supersampled reference (each subsample composites
// exactly in double and the pixel is their average; src and dst are constant
// within a pixel, so the average reduces exactly to the count-weighted lerp).
//
// Bounds are measured-and-pinned (set from a verbose run, headroom ~2x):
// aggregate checks so a regression reports once.  Errors are compared in
// PREMULTIPLIED space (readback u8 is re-premultiplied), so low-alpha pixels
// don't amplify quantization noise.

#include "canvas.h"
#include "compositor.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// --- the double-precision reference (mirrors compositor_cpu.c's math) -------

typedef struct {
    double r, g, b, a;
} dpx;

static double dmin(double a, double b) { return a < b ? a : b; }
static double dmax(double a, double b) { return a > b ? a : b; }

// Separable blend B(cb, cs), unpremultiplied.
static double sep_ref(int mode, double cb, double cs) {
    switch (mode) {
        case COMPOSITOR_MULTIPLY:   return cb * cs;
        case COMPOSITOR_SCREEN:     return cb + cs - cb * cs;
        case COMPOSITOR_OVERLAY:
            return 2.0 * cb <= 1.0 ? 2.0 * cb * cs
                                   : 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs);
        case COMPOSITOR_DARKEN:     return dmin(cb, cs);
        case COMPOSITOR_LIGHTEN:    return dmax(cb, cs);
        case COMPOSITOR_COLOR_DODGE:
            return cb <= 0.0 ? 0.0 : (cs >= 1.0 ? 1.0 : dmin(1.0, cb / (1.0 - cs)));
        case COMPOSITOR_COLOR_BURN:
            return cb >= 1.0 ? 1.0
                             : (cs <= 0.0 ? 0.0 : 1.0 - dmin(1.0, (1.0 - cb) / cs));
        case COMPOSITOR_HARD_LIGHT:
            return 2.0 * cs <= 1.0 ? 2.0 * cb * cs
                                   : 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs);
        case COMPOSITOR_SOFT_LIGHT: {
            double dd = cb <= 0.25 ? ((16.0 * cb - 12.0) * cb + 4.0) * cb : sqrt(cb);
            return cs <= 0.5 ? cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb)
                             : cb + (2.0 * cs - 1.0) * (dd - cb);
        }
        case COMPOSITOR_DIFFERENCE: return fabs(cb - cs);
        default:                    return cb + cs - 2.0 * cb * cs;  // exclusion
    }
}

typedef struct {
    double r, g, b;
} drgb;

static double lum_ref(drgb c) { return 0.3 * c.r + 0.59 * c.g + 0.11 * c.b; }

static drgb clip_color_ref(drgb c) {
    double l = lum_ref(c);
    double n = dmin(c.r, dmin(c.g, c.b));
    double x = dmax(c.r, dmax(c.g, c.b));
    if (n < 0.0) {
        double k = l / (l - n);
        c.r = l + (c.r - l) * k;
        c.g = l + (c.g - l) * k;
        c.b = l + (c.b - l) * k;
    }
    x = dmax(c.r, dmax(c.g, c.b));  // on the updated c, as the kernel does
    if (x > 1.0) {
        double k = (1.0 - l) / (x - l);
        c.r = l + (c.r - l) * k;
        c.g = l + (c.g - l) * k;
        c.b = l + (c.b - l) * k;
    }
    return c;
}

static drgb set_lum_ref(drgb c, double l) {
    double dl = l - lum_ref(c);
    return clip_color_ref((drgb){ c.r + dl, c.g + dl, c.b + dl });
}

static double sat_ref(drgb c) {
    return dmax(c.r, dmax(c.g, c.b)) - dmin(c.r, dmin(c.g, c.b));
}

static drgb set_sat_ref(drgb c, double s) {
    double mn = dmin(c.r, dmin(c.g, c.b));
    double mx = dmax(c.r, dmax(c.g, c.b));
    if (mx <= mn) {
        return (drgb){ 0.0, 0.0, 0.0 };
    }
    double k = s / (mx - mn);
    return (drgb){ (c.r - mn) * k, (c.g - mn) * k, (c.b - mn) * k };
}

static drgb nonsep_ref(int mode, drgb cb, drgb cs) {
    switch (mode) {
        case COMPOSITOR_HUE:        return set_lum_ref(set_sat_ref(cs, sat_ref(cb)), lum_ref(cb));
        case COMPOSITOR_SATURATION: return set_lum_ref(set_sat_ref(cb, sat_ref(cs)), lum_ref(cb));
        case COMPOSITOR_COLOR:      return set_lum_ref(cs, lum_ref(cb));
        default:                    return set_lum_ref(cb, lum_ref(cs));  // luminosity
    }
}

// The output clamp, lane-wise cnvs_px8_clamp_premul.
static dpx clamp_premul_ref(dpx c) {
    double ao = dmin(c.a, 1.0);
    return (dpx){ dmax(0.0, dmin(ao, c.r)), dmax(0.0, dmin(ao, c.g)),
                  dmax(0.0, dmin(ao, c.b)), dmax(0.0, dmin(ao, c.a)) };
}

// Full-strength blend of premultiplied s over premultiplied d, in double.
static dpx blend_ref(int mode, dpx s, dpx d) {
    double sa = s.a, da = d.a;
    dpx co;
    if (mode <= COMPOSITOR_COPY) {
        double fa, fb;
        switch (mode) {
            case COMPOSITOR_SRC_IN:   fa = da;       fb = 0.0;      break;
            case COMPOSITOR_SRC_OUT:  fa = 1.0 - da; fb = 0.0;      break;
            case COMPOSITOR_SRC_ATOP: fa = da;       fb = 1.0 - sa; break;
            case COMPOSITOR_DST_OVER: fa = 1.0 - da; fb = 1.0;      break;
            case COMPOSITOR_DST_IN:   fa = 0.0;      fb = sa;       break;
            case COMPOSITOR_DST_OUT:  fa = 0.0;      fb = 1.0 - sa; break;
            case COMPOSITOR_DST_ATOP: fa = 1.0 - da; fb = sa;       break;
            case COMPOSITOR_XOR:      fa = 1.0 - da; fb = 1.0 - sa; break;
            case COMPOSITOR_LIGHTER:  fa = 1.0;      fb = 1.0;      break;
            case COMPOSITOR_COPY:     fa = 1.0;      fb = 0.0;      break;
            default:                  fa = 1.0;      fb = 1.0 - sa; break;  // src-over
        }
        co = (dpx){ fa * s.r + fb * d.r, fa * s.g + fb * d.g,
                    fa * s.b + fb * d.b, fa * sa + fb * da };
    } else {
        drgb cs = sa > 0.0 ? (drgb){ s.r / sa, s.g / sa, s.b / sa }
                           : (drgb){ 0.0, 0.0, 0.0 };
        drgb cb = da > 0.0 ? (drgb){ d.r / da, d.g / da, d.b / da }
                           : (drgb){ 0.0, 0.0, 0.0 };
        drgb t;
        if (mode >= COMPOSITOR_HUE) {
            t = nonsep_ref(mode, cb, cs);
        } else {
            t = (drgb){ sep_ref(mode, cb.r, cs.r), sep_ref(mode, cb.g, cs.g),
                        sep_ref(mode, cb.b, cs.b) };
        }
        co.r = s.r * (1.0 - da) + d.r * (1.0 - sa) + sa * da * t.r;
        co.g = s.g * (1.0 - da) + d.g * (1.0 - sa) + sa * da * t.g;
        co.b = s.b * (1.0 - da) + d.b * (1.0 - sa) + sa * da * t.b;
        co.a = sa * (1.0 - da) + da * (1.0 - sa) + sa * da;
    }
    return clamp_premul_ref(co);
}

// The ruling: cov of the blend, 1-cov of the destination.
static dpx cov_lerp_ref(dpx d, dpx co, double k) {
    return (dpx){ co.r * k + d.r * (1.0 - k), co.g * k + d.g * (1.0 - k),
                  co.b * k + d.b * (1.0 - k), co.a * k + d.a * (1.0 - k) };
}

static dpx dpx_of(cnvs_premul p) {
    return (dpx){ (double)p.r, (double)p.g, (double)p.b, (double)p.a };
}

// max / sum of |got - ref| over the four premultiplied channels.
typedef struct {
    double max, sum;
} errs;

static void err_acc(errs *e, dpx got, dpx ref) {
    double d[4] = { fabs(got.r - ref.r), fabs(got.g - ref.g),
                    fabs(got.b - ref.b), fabs(got.a - ref.a) };
    for (int i = 0; i < 4; i++) {
        e->max = dmax(e->max, d[i]);
        e->sum += d[i];
    }
}

// The five modes the lerp genuinely fixes (fold fails the criterion), plus
// lighter (passes it unclamped; the clamp breaks the equivalence).  For these
// the test asserts the new semantics land STRICTLY closer to the reference
// than the old fold.
static bool expect_closer(int mode) {
    switch (mode) {
        case COMPOSITOR_SRC_IN:
        case COMPOSITOR_SRC_OUT:
        case COMPOSITOR_DST_IN:
        case COMPOSITOR_DST_ATOP:
        case COMPOSITOR_COPY:
        case COMPOSITOR_LIGHTER:
            return true;
        default:
            return false;
    }
}

// --- Part A: the kernel sweep ------------------------------------------------

// Unpremultiplied (r, g, b, a) value grids: translucent, opaque, zero-alpha,
// and saturating combinations (lighter's clamp must engage).
static float const SRCS[][4] = {
    { 1.0f, 0.0f, 0.0f, 1.0f },     { 0.25f, 0.5f, 0.75f, 0.8f },
    { 0.9f, 0.1f, 0.4f, 0.25f },    { 0.0f, 1.0f, 0.0f, 0.5f },
    { 0.6f, 0.6f, 0.6f, 0.0f },     { 1.0f, 1.0f, 0.2f, 0.6f },
};
static float const DSTS[][4] = {
    { 0.0f, 0.0f, 1.0f, 1.0f },     { 0.8f, 0.4f, 0.2f, 0.6f },
    { 0.3f, 0.9f, 0.5f, 0.3f },     { 0.0f, 0.0f, 0.0f, 0.0f },
    { 1.0f, 1.0f, 1.0f, 1.0f },
};
enum { NSRC = (int)(sizeof SRCS / sizeof SRCS[0]),
       NDST = (int)(sizeof DSTS / sizeof DSTS[0]) };

static void part_a(void) {
    enum { N = 256 };  // one pixel per coverage byte
    compositor *__single c = compositor_create(N, 1);
    cnvs_premul *dstt = malloc((size_t)N * sizeof *dstt);
    cnvs_premul *full = malloc((size_t)N * sizeof *full);
    cnvs_premul *fold = malloc((size_t)N * sizeof *fold);
    cnvs_premul *out = malloc((size_t)N * sizeof *out);
    uint8_t *covp = malloc((size_t)N);
    CHECK(c && dstt && full && fold && out && covp);
    if (!c || !dstt || !full || !fold || !out || !covp) {
        goto done;
    }
    for (int i = 0; i < N; i++) {
        covp[i] = (uint8_t)i;
    }

    for (int mode = 0; mode < COMPOSITOR_MODE_COUNT; mode++) {
        errs en = { 0 }, eo = { 0 };
        for (int si = 0; si < NSRC; si++) {
            cnvs_unpremul su = cnvs_unpremul_of(SRCS[si][0], SRCS[si][1],
                                                SRCS[si][2], SRCS[si][3]);
            for (int di = 0; di < NDST; di++) {
                cnvs_unpremul du = cnvs_unpremul_of(DSTS[di][0], DSTS[di][1],
                                                    DSTS[di][2], DSTS[di][3]);
                cnvs_premul dp = cnvs_premultiply(du);
                cnvs_premul sp = cnvs_premultiply(su);
                for (int i = 0; i < N; i++) {
                    dstt[i] = dp;
                    full[i] = sp;
                    // The old shade fold, arithmetic-for-arithmetic: coverage
                    // widened and divided in f32, folded into the paint's
                    // alpha, one narrow to f16, then the f16 premultiply.
                    float af = (float)su.a * ((float)i / 255.0f);
                    fold[i] = cnvs_premultiply((cnvs_unpremul){
                        .r = su.r, .g = su.g, .b = su.b, .a = (_Float16)af });
                }
                dpx s_d = dpx_of(sp), d_d = dpx_of(dp);

                // NEW: full-strength tile + the coverage plane.
                compositor_blend(c, 0, 0, N, 1, dstt, NULL, COMPOSITOR_COPY);
                compositor_blend(c, 0, 0, N, 1, full, covp,
                                 (compositor_blend_mode)mode);
                compositor_read(c, out, N);
                for (int i = 0; i < N; i++) {
                    dpx ref = cov_lerp_ref(d_d, blend_ref(mode, s_d, d_d),
                                           (double)i / 255.0);
                    err_acc(&en, dpx_of(out[i]), ref);
                }

                // OLD: pre-folded tile, no plane.
                compositor_blend(c, 0, 0, N, 1, dstt, NULL, COMPOSITOR_COPY);
                compositor_blend(c, 0, 0, N, 1, fold, NULL,
                                 (compositor_blend_mode)mode);
                compositor_read(c, out, N);
                for (int i = 0; i < N; i++) {
                    dpx ref = cov_lerp_ref(d_d, blend_ref(mode, s_d, d_d),
                                           (double)i / 255.0);
                    err_acc(&eo, dpx_of(out[i]), ref);
                }
            }
        }
        if (getenv("COVERAGE_LERP_VERBOSE")) {
            (void)fprintf(stderr,
                          "A mode %2d  new max %.6f sum %10.4f   old max %.6f sum %10.4f\n",
                          mode, en.max, en.sum, eo.max, eo.sum);
        }
        // The kernel is f16 and the reference is double: per-pixel error is
        // a few f16 ULP (measured max 0.0015 across all modes; the
        // non-separable modes own the worst lanes).
        CHECK(en.max <= 0.005);
        if (expect_closer(mode)) {
            // The genuine fixes: the old fold's error is the full scale of
            // the destination it wrongly removed (measured max >= 0.37, sums
            // hundreds of times larger).  Strictly closer, with a chasm.
            CHECK(en.sum < eo.sum);
            CHECK(eo.max > 0.05);  // the bug was real, not rounding
        } else {
            // Fold-equivalent modes (over-family by the criterion, blends by
            // T's homogeneity): old and new agree to f16 rounding.  Neither
            // beats the other beyond noise; pin both to the same bound.
            // (measured: the lerp's summed error is a few percent lower for
            // most blend modes -- full-strength un-premultiplies are cleaner
            // -- but that is rounding, not semantics.)
            CHECK(eo.max <= 0.005);
        }
    }
done:
    compositor_destroy(c);
    free(dstt);
    free(full);
    free(fold);
    free(out);
    free(covp);
}

// --- Part B: geometric end-to-end against the supersampled oracle -----------

enum { W = 28, H = 24, SS = 16 };  // canvas and the supersample grid

// Backdrop halves (left opaque, right translucent) and the two source paints.
static float const DL[4] = { 0.1f, 0.3f, 0.9f, 1.0f };
static float const DR[4] = { 0.9f, 0.2f, 0.1f, 0.5f };
static float const SDISC[4] = { 0.97f, 0.55f, 0.18f, 1.0f };
static float const SQUAD[4] = { 0.2f, 0.8f, 0.4f, 0.8f };

// Point-in-polygon by ray-crossing parity (both shapes are convex, so parity
// == nonzero).  Vertices are the float values handed to the canvas, widened.
static bool inside_poly(float const *__counted_by(n) vx,
                        float const *__counted_by(n) vy, int n,
                        double px, double py) {
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        double xi = (double)vx[i], yi = (double)vy[i];
        double xj = (double)vx[j], yj = (double)vy[j];
        if ((yi > py) != (yj > py) &&
            px < (xj - xi) * (py - yi) / (yj - yi) + xi) {
            in = !in;
        }
    }
    return in;
}

// 16x16 subsample hit count per pixel: the supersampled coverage.  src and
// dst are constant within a pixel, so averaging the per-subsample composites
// (inside -> blend(s, d), outside -> d) reduces exactly to the count-weighted
// lerp the per-pixel checks below apply.
static void coverage_ss(float const *__counted_by(n) vx,
                        float const *__counted_by(n) vy, int n,
                        double *__counted_by(W * H) cov) {
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int hit = 0;
            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    double px = (double)x + ((double)sx + 0.5) / (double)SS;
                    double py = (double)y + ((double)sy + 0.5) / (double)SS;
                    hit += inside_poly(vx, vy, n, px, py) ? 1 : 0;
                }
            }
            cov[y * W + x] = (double)hit / (double)(SS * SS);
        }
    }
}

// Render the polygon under `mode` over the two-half backdrop and accumulate
// rendered-vs-reference and old-fold-vs-reference error.
static void check_scene(int mode, float const *__counted_by(n) vx,
                        float const *__counted_by(n) vy, int n,
                        float const *__counted_by(4) sc,
                        double const *__counted_by(W * H) cov,
                        errs *en, errs *eo) {
    canvas *__single cv = canvas_create(W, H);
    uint8_t *px = malloc((size_t)(W * H * 4));
    CHECK(cv && px);
    if (!cv || !px) {
        canvas_destroy(cv);
        free(px);
        return;
    }
    canvas_set_fill_rgba(cv, DL[0], DL[1], DL[2], DL[3]);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W / 2.0f, (float)H);
    canvas_set_fill_rgba(cv, DR[0], DR[1], DR[2], DR[3]);
    canvas_fill_rect(cv, (float)W / 2.0f, 0.0f, (float)W / 2.0f, (float)H);
    canvas_set_global_composite_operation(cv, (canvas_composite_op)mode);
    canvas_set_fill_rgba(cv, sc[0], sc[1], sc[2], sc[3]);
    canvas_begin_path(cv);
    canvas_move_to(cv, vx[0], vy[0]);
    for (int i = 1; i < n; i++) {
        canvas_line_to(cv, vx[i], vy[i]);
    }
    canvas_close_path(cv);
    canvas_fill(cv);
    canvas_read_rgba(cv, px, W * H * 4);

    // Pixel-aligned source-over rect fills onto transparent land the exact
    // premultiplied paint; mirror that for the reference's destination.
    dpx dl = dpx_of(cnvs_premultiply(cnvs_unpremul_of(DL[0], DL[1], DL[2], DL[3])));
    dpx dr = dpx_of(cnvs_premultiply(cnvs_unpremul_of(DR[0], DR[1], DR[2], DR[3])));
    // The non-folding shade tile is the premultiplied paint at full strength.
    dpx s_d = dpx_of(cnvs_premultiply(cnvs_unpremul_of(sc[0], sc[1], sc[2], sc[3])));

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            dpx d = x < W / 2 ? dl : dr;
            double k = cov[y * W + x];
            dpx ref = cov_lerp_ref(d, blend_ref(mode, s_d, d), k);
            // The old semantics, idealized in double: coverage folded into
            // the premultiplied source, blended at the folded strength.
            dpx sf = { s_d.r * k, s_d.g * k, s_d.b * k, s_d.a * k };
            dpx old = blend_ref(mode, sf, d);
            // Compare premultiplied: readback is unpremultiplied u8, so
            // re-premultiply (low-alpha pixels would otherwise amplify the
            // readback quantization).
            int o = (y * W + x) * 4;
            double a8 = (double)px[o + 3] / 255.0;
            dpx got = { (double)px[o] / 255.0 * a8, (double)px[o + 1] / 255.0 * a8,
                        (double)px[o + 2] / 255.0 * a8, a8 };
            err_acc(en, got, ref);
            err_acc(eo, old, ref);
        }
    }
    canvas_destroy(cv);
    free(px);
}

static void part_b(void) {
    enum { NV = 64 };
    // The disc edge: a 64-gon (radius 8.3 about (13.3, 11.7)), every edge
    // orientation and coverage value in one shape.
    float dx[NV], dy[NV];
    for (int i = 0; i < NV; i++) {
        double t = 2.0 * 3.14159265358979323846 * i / (double)NV;
        dx[i] = (float)(13.3 + 8.3 * cos(t));
        dy[i] = (float)(11.7 + 8.3 * sin(t));
    }
    // The thin diagonal: a 1.3px-wide quad from (4.2, 3.1) to (23.8, 20.6) --
    // every pixel it touches is partial coverage.
    double x0 = 4.2, y0 = 3.1, x1 = 23.8, y1 = 20.6, hw = 0.65;
    double len = hypot(x1 - x0, y1 - y0);
    double nx = -(y1 - y0) / len * hw, ny = (x1 - x0) / len * hw;
    float qx[4] = { (float)(x0 + nx), (float)(x1 + nx),
                    (float)(x1 - nx), (float)(x0 - nx) };
    float qy[4] = { (float)(y0 + ny), (float)(y1 + ny),
                    (float)(y1 - ny), (float)(y0 - ny) };

    double *covd = malloc((size_t)(W * H) * sizeof *covd);
    double *covq = malloc((size_t)(W * H) * sizeof *covq);
    CHECK(covd && covq);
    if (covd && covq) {
        coverage_ss(dx, dy, NV, covd);
        coverage_ss(qx, qy, 4, covq);
        for (int mode = 0; mode < COMPOSITOR_MODE_COUNT; mode++) {
            errs en = { 0 }, eo = { 0 };
            check_scene(mode, dx, dy, NV, SDISC, covd, &en, &eo);
            check_scene(mode, qx, qy, 4, SQUAD, covq, &en, &eo);
            if (getenv("COVERAGE_LERP_VERBOSE")) {
                (void)fprintf(stderr,
                              "B mode %2d  new max %.6f sum %10.4f   old max %g sum %g\n",
                              mode, en.max, en.sum, eo.max, eo.sum);
            }
            // End-to-end budget: the 16x16 supersample estimates the exact-
            // area coverage to ~1/32 worst-case on an edge pixel, plus f16
            // kernel rounding and u8 readback (measured max 0.0122; the
            // budget dominates, the kernel is invisible under it).
            CHECK(en.max <= 0.025);
            if (expect_closer(mode)) {
                // The semantic fix, end to end: the old fold scaled the
                // destination away across whole uncovered regions (the five
                // failing Porter-Duff modes sum to ~1845 here, 240x the new
                // semantics' supersample noise; lighter's clamp deviation
                // sums to ~11).
                CHECK(en.sum < eo.sum);
                CHECK(eo.max > 0.05);
            } else {
                // The fold-equivalence claim, pinned: for the over-family
                // AND every blend mode the old fold IS the ruling in exact
                // arithmetic (T = sa*da*B(d/da, s/sa) is degree-1 homogeneous
                // in (s, sa)), so the two double models agree to accumulated
                // double rounding (measured <= 3e-15 summed).  If a future
                // mode breaks this, it belongs in expect_closer.
                CHECK(eo.sum <= 1e-12);
            }
        }
    }
    free(covd);
    free(covq);
}

int main(void) {
    part_a();
    part_b();
    return TEST_REPORT();
}
