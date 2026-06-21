#include "canvas2d_gradient.h"
#include "canvas2d_color.h"
#include "canvas2d_planar.h"

#include <math.h>
#include <string.h>

void canvas2d_gradient_add_stop(struct canvas2d_gradient *gr, float offset, canvas2d_unpremul color) {
    if (gr->stop_count >= CANVAS2D_STOPS_MAX) {
        return;
    }
    float const o = canvas2d_clamp01(offset);
    // Insertion sort keeps stops ordered; ties land after equal offsets, so a
    // later addColorStop at the same offset wins on the high side (as in CSS).
    int i = gr->stop_count;
    while (i > 0 && gr->stops[i - 1].offset > o) {
        gr->stops[i] = gr->stops[i - 1];
        i -= 1;
    }
    gr->stops[i] = (canvas2d_stop){ .offset = o, .color = color };
    gr->stop_count += 1;
}

// The three interpolation-space colour coordinates a stop colour can be lerped
// in.  Generic name `x,y,z`: (r,g,b) for sRGB / linear sRGB, (L,a,b) for Oklab.
typedef struct {
    float x, y, z;
} grad_coord;

// Take one stored stop colour (in the gradient's WORKING space) to LINEAR sRGB.
// On a linear canvas the channels already ARE linear (identity); on an sRGB
// canvas they are encoded sRGB and decode through the odd-extension transfer
// (total over R, so an extended stop survives).  Alpha is never a colour and
// passes through untouched.  f32 throughout -- this is the cbrt-precision math
// docs/decisions/color-axis.md reserves f32 for.
static canvas2d_rgb stop_to_linear(canvas2d_unpremul c, enum canvas2d_color_space space) {
    if (space == CANVAS2D_CS_LINEAR_SRGB) {
        return (canvas2d_rgb){ .r = (float)c.r, .g = (float)c.g, .b = (float)c.b };
    }
    return canvas2d_rgb_srgb_to_linear(
        (canvas2d_rgb){ .r = (float)c.r, .g = (float)c.g, .b = (float)c.b });
}

// One stored stop colour (WORKING space) to the chosen INTERPOLATION space's
// coordinates.  sRGB interpolation lerps the stored encoded channels directly
// (no transfer round trip); linear-sRGB and Oklab decode through linear first.
static grad_coord stop_to_interp(canvas2d_unpremul c, enum canvas2d_color_space interp,
                                 enum canvas2d_color_space space) {
    if (interp == CANVAS2D_CS_SRGB) {
        return (grad_coord){ .x = (float)c.r, .y = (float)c.g, .z = (float)c.b };
    }
    canvas2d_rgb const lin = stop_to_linear(c, space);
    if (interp == CANVAS2D_CS_LINEAR_SRGB) {
        return (grad_coord){ .x = lin.r, .y = lin.g, .z = lin.b };
    }
    canvas2d_oklab const lab = canvas2d_linear_srgb_to_oklab(lin);
    return (grad_coord){ .x = lab.L, .y = lab.a, .z = lab.b };
}

// The inverse: interpolation-space coordinates + alpha back to the gradient's
// WORKING space, narrowed to the _Float16 the shade/blend path expects.
static canvas2d_unpremul interp_to_stop(grad_coord c, float a, enum canvas2d_color_space interp,
                                    enum canvas2d_color_space space) {
    if (interp == CANVAS2D_CS_SRGB) {
        return canvas2d_unpremul_of(c.x, c.y, c.z, a);
    }
    canvas2d_rgb lin;
    if (interp == CANVAS2D_CS_LINEAR_SRGB) {
        lin = (canvas2d_rgb){ .r = c.x, .g = c.y, .b = c.z };
    } else {
        lin = canvas2d_oklab_to_linear_srgb((canvas2d_oklab){ .L = c.x, .a = c.y, .b = c.z });
    }
    canvas2d_rgb const out = space == CANVAS2D_CS_LINEAR_SRGB ? lin : canvas2d_rgb_linear_to_srgb(lin);
    return canvas2d_unpremul_of(out.r, out.g, out.b, a);
}

// The general colour lerp for ONE evaluated point on a NON-default gradient,
// lo/hi the surrounding stop pair (working space) and lerp_t the geometry
// solve's already-computed parameter in [0,1].  This is THE reference for every
// non-default space x alpha combination: the planar row kernel calls it per
// lane, so scalar == planar is bit-identical by construction (one function, not
// two parallel ones).  All arithmetic is f32, narrowing once at the handback.
//
// SPACE: the colour coords are taken to the interpolation space (sRGB / linear
// sRGB / Oklab) and back.  ALPHA: if PREMUL, each stop's coords are scaled by
// its alpha before the lerp and unpremultiplied after -- interpolation hygiene,
// so a transparent stop contributes no colour (transparent-red -> opaque-blue
// is pure blue at the midpoint, not muddy purple).  If UNPREMUL, the coords
// lerp directly.  Alpha itself always lerps linearly on its own (never
// premultiplied by itself).  Under PREMUL, alpha == 0 makes the colour a
// don't-care, so the unpremultiply divide is guarded (the eventual premultiply
// zeroes it anyway).
static canvas2d_unpremul general_lerp(canvas2d_unpremul lo, canvas2d_unpremul hi, float lerp_t,
                                  enum canvas2d_color_space interp,
                                  enum canvas2d_alpha_type alpha,
                                  enum canvas2d_color_space space) {
    grad_coord const lc = stop_to_interp(lo, interp, space);
    grad_coord const hc = stop_to_interp(hi, interp, space);

    float const la = (float)lo.a, ha = (float)hi.a;
    float const a  = la + (ha - la) * lerp_t;  // alpha interpolates linearly, unpremul

    grad_coord out;
    if (alpha == CANVAS2D_ALPHA_PREMUL) {
        // Premultiplied coords (each scaled by that stop's alpha), lerped, then
        // unpremultiplied by the interpolated alpha.
        float const px = lc.x * la + (hc.x * ha - lc.x * la) * lerp_t;
        float const py = lc.y * la + (hc.y * ha - lc.y * la) * lerp_t;
        float const pz = lc.z * la + (hc.z * ha - lc.z * la) * lerp_t;
        float const inv = a > 0.0f ? 1.0f / a : 0.0f;
        out = (grad_coord){ .x = px * inv, .y = py * inv, .z = pz * inv };
    } else {
        // Coords lerp directly.
        out = (grad_coord){ .x = lc.x + (hc.x - lc.x) * lerp_t,
                            .y = lc.y + (hc.y - lc.y) * lerp_t,
                            .z = lc.z + (hc.z - lc.z) * lerp_t };
    }
    return interp_to_stop(out, a, interp, space);
}

// The default (sRGB + unpremul) interpolation, true or false.  This combination
// is the EXACT legacy straight stored-value lerp and MUST route through that
// byte-identical path, not the generalized convert-round-trip above.
static bool grad_is_default(struct canvas2d_gradient const *gr) {
    return gr->interp == CANVAS2D_CS_SRGB && gr->interp_alpha == CANVAS2D_ALPHA_UNPREMUL;
}

canvas2d_unpremul canvas2d_gradient_color_at(struct canvas2d_gradient const *gr, float t) {
    int const n = gr->stop_count;
    if (n == 0) {
        return canvas2d_unpremul_of(0.0f, 0.0f, 0.0f, 0.0f);
    }
    t = canvas2d_clamp01(t);
    if (t <= gr->stops[0].offset) {
        return gr->stops[0].color;
    }
    if (t >= gr->stops[n - 1].offset) {
        return gr->stops[n - 1].color;
    }
    for (int i = 0; i + 1 < n; i++) {
        canvas2d_stop const lo = gr->stops[i];
        canvas2d_stop const hi = gr->stops[i + 1];
        if (t <= hi.offset) {
            // The parameter and stop offsets are geometry and stay f32; the
            // colour lerp itself runs in _Float16.  The guard is the minimal
            // one: it keeps the divide defined at coincident stops (span == 0
            // takes lo).
            float const span = hi.offset - lo.offset;
            float const lerp_t = span > 0.0f ? (t - lo.offset) / span : 0.0f;
            if (!grad_is_default(gr)) {
                return general_lerp(lo.color, hi.color, lerp_t,
                                    gr->interp, gr->interp_alpha, gr->space);
            }
            half4 const lov = { lo.color.r, lo.color.g, lo.color.b, lo.color.a };
            half4 const hiv = { hi.color.r, hi.color.g, hi.color.b, hi.color.a };
            half4 const c = lov + (hiv - lov) * (_Float16)lerp_t;
            return (canvas2d_unpremul){ .r = c[0], .g = c[1], .b = c[2], .a = c[3] };
        }
    }
    return gr->stops[n - 1].color;  // unreachable: t < last offset handled above
}

// The spec's degenerate test is EXACT equality (a tiny-but-nonzero gradient
// still paints), so float == is the required semantic here, not an accident.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
bool canvas2d_gradient_paints_nothing(struct canvas2d_gradient const *gr) {
    bool const coincident = gr->p0.x == gr->p1.x && gr->p0.y == gr->p1.y;
    switch (gr->kind) {
        case CANVAS2D_GRAD_LINEAR: return coincident;
        case CANVAS2D_GRAD_RADIAL: return coincident && gr->r0 == gr->r1;
        case CANVAS2D_GRAD_CONIC:  return false;
    }
    return false;
}
#pragma clang diagnostic pop

bool canvas2d_gradient_param(struct canvas2d_gradient const *gr, canvas2d_vec2 p, float *__single t) {
    if (gr->kind == CANVAS2D_GRAD_CONIC) {
        // Angle of p about the centre, measured clockwise from +x (device space is
        // y-down, so atan2 already increases clockwise), minus the start angle;
        // wrapped into [0,1).  Every point has a parameter, so this never misses.
        float const ang = atan2f(p.y - gr->p0.y, p.x - gr->p0.x) - gr->angle;
        float v = ang * 0.15915494309189535f;  // * 1/(2*pi)
        *t = v - floorf(v);
        return true;
    }
    if (gr->kind == CANVAS2D_GRAD_LINEAR) {
        float const dx = gr->p1.x - gr->p0.x;
        float const dy = gr->p1.y - gr->p0.y;
        float const denom = dx * dx + dy * dy;
        float v = denom > 1e-12f
                      ? ((p.x - gr->p0.x) * dx + (p.y - gr->p0.y) * dy) / denom
                      : 0.0f;
        *t = canvas2d_clamp01(v);
        return true;
    }
    // Radial: find the largest t with (r0 + t*dr) >= 0 such that p lies on the
    // circle centred at lerp(p0,p1,t) with that radius.  Expanding
    // |p - C(t)| = R(t) gives the quadratic a t^2 + b t + c = 0.
    float const cdx = gr->p1.x - gr->p0.x;
    float const cdy = gr->p1.y - gr->p0.y;
    float dr  = gr->r1   - gr->r0  ;
    float const pdx = p.x - gr->p0.x;
    float const pdy = p.y - gr->p0.y;
    float const a = cdx * cdx + cdy * cdy - dr * dr;
    float const b = -2.0f * (cdx * pdx + cdy * pdy + gr->r0 * dr);
    float const c = pdx * pdx + pdy * pdy - gr->r0 * gr->r0;
    float root;
    if (fabsf(a) < 1e-9f) {
        if (fabsf(b) < 1e-12f) {
            return false;
        }
        root = -c / b;
        if (gr->r0 + root * dr < 0.0f) {
            return false;
        }
    } else {
        float const disc = b * b - 4.0f * a * c;
        if (disc < 0.0f) {
            return false;
        }
        float const sq = sqrtf(disc);
        float hi = (-b + sq) / (2.0f * a);
        float lo = (-b - sq) / (2.0f * a);
        if (hi < lo) {
            float const tmp = hi;
            hi = lo;
            lo = tmp;
        }
        if (gr->r0 + hi * dr >= 0.0f) {
            root = hi;
        } else if (gr->r0 + lo * dr >= 0.0f) {
            root = lo;
        } else {
            return false;
        }
    }
    *t = canvas2d_clamp01(root);
    return true;
}

bool canvas2d_gradient_param_user(struct canvas2d_gradient const *gr, canvas2d_vec2 p,
                              float *__single t) {
    // The perspective path solves the parameter in USER space.  Reuse the
    // device-space solver verbatim by handing it a shallow copy whose geometry
    // fields are the user-space def (up0/up1/ur0/ur1/uangle): one solver, so the
    // user-space and device-space parameter math are identical by construction.
    struct canvas2d_gradient u = *gr;
    u.p0 = gr->up0;
    u.p1 = gr->up1;
    u.r0 = gr->ur0;
    u.r1 = gr->ur1;
    u.angle = gr->uangle;
    return canvas2d_gradient_param(&u, p, t);
}

canvas2d_unpremul canvas2d_gradient_sample(struct canvas2d_gradient const *gr, canvas2d_vec2 p, float alpha) {
    float t;
    canvas2d_unpremul c;
    if (canvas2d_gradient_param(gr, p, &t)) {
        c = canvas2d_gradient_color_at(gr, t);
    } else {
        c = canvas2d_unpremul_of(0.0f, 0.0f, 0.0f, 0.0f);
    }
    c.a = (_Float16)((float)c.a * alpha);
    return c;
}

// Bit-exact f32 lane select, the 32-bit twin of half8_if_then_else: a where the mask
// lane is set (-1, from a vector comparison), else b.  Bitwise: the selected
// lane passes through untouched (an arithmetic b + (a-b)*m re-rounds it), and
// an unselected lane's inf/NaN is discarded exactly.
static float8 float8_if_then_else(int8 m, float8 a, float8 b) {
    return (float8)(((int8)a & m) | ((int8)b & ~m));
}

// Parameter solve for a horizontal run of `n` pixel centres
// (x0 + i + 0.5, y), i in [0, n).  Writes t in [0,1] per pixel, or -1 where the
// point has no gradient parameter (the radial "outside" case) so the caller
// paints transparent.  Along a row only x varies, so the per-pixel work
// vectorizes 8 wide.
void canvas2d_gradient_param_row(struct canvas2d_gradient const *gr, int x0, float y, int n,
                             float *__counted_by(n) t_out) {
    float8 const lane = { 0, 1, 2, 3, 4, 5, 6, 7 };
    float const base = (float)x0 + 0.5f - gr->p0.x;
    int i = 0;
    if (gr->kind == CANVAS2D_GRAD_LINEAR) {
        float const dx = gr->p1.x - gr->p0.x;
        float const dy = gr->p1.y - gr->p0.y;
        float const denom = dx * dx + dy * dy;
        float const inv = denom > 1e-12f ? 1.0f / denom : 0.0f;  // inv == 0 -> all-zero t
        float const cy = (y - gr->p0.y) * dy;                    // y term constant per row
        for (; i + 8 <= n; i += 8) {
            float8 const px = base + ((float)i + lane);
            float8 v = float8_clamp01((px * dx + cy) * inv);  // canvas2d_clamp01(((p-p0).d)/|d|^2)
            memcpy(t_out + i, &v, sizeof v);            // bounds-checked vector store
        }
    } else if (gr->kind == CANVAS2D_GRAD_RADIAL) {
        float const cdx = gr->p1.x - gr->p0.x;
        float const cdy = gr->p1.y - gr->p0.y;
        float dr  = gr->r1   - gr->r0  ;
        float const r0 = gr->r0;
        float const a = cdx * cdx + cdy * cdy - dr * dr;
        float const pdy = y - gr->p0.y;
        float const bconst = cdy * pdy + r0 * dr;  // b = -2*(cdx*pdx + bconst)
        float const cconst = pdy * pdy - r0 * r0;  // c = pdx*pdx + cconst
        bool const a_lin = fabsf(a) < 1e-9f;       // a is constant along the row
        float8 const inv2a = (float8)(a_lin ? 0.0f : 1.0f / (2.0f * a));
        for (; i + 8 <= n; i += 8) {
            float8 const pdx = base + ((float)i + lane);
            float8 const b = -2.0f * (cdx * pdx + bconst);
            float8 c = pdx * pdx + cconst;
            float8 root;
            int8 valid;
            if (a_lin) {  // degenerate: the t^2 term vanishes -> b t + c = 0
                root = -c / b;
                valid = ((b > 1e-12f) | (b < -1e-12f)) & (r0 + root * dr >= 0.0f);
            } else {
                float8 const disc = b * b - 4.0f * a * c;
                float8 sq = __builtin_elementwise_sqrt(
                    __builtin_elementwise_max((float8)0.0f, disc));
                float8 const r1_ = (-b + sq) * inv2a;
                float8 const r2_ = (-b - sq) * inv2a;
                float8 const hi = __builtin_elementwise_max(r1_, r2_);
                float8 const lo = __builtin_elementwise_min(r1_, r2_);
                int8 const hiok = r0 + hi * dr >= 0.0f;  // prefer the larger valid root
                int8 const look = r0 + lo * dr >= 0.0f;
                root = float8_if_then_else(hiok, hi, float8_if_then_else(look, lo, (float8)0.0f));
                valid = (disc >= 0.0f) & (hiok | look);
            }
            float8 out = float8_if_then_else(valid, float8_clamp01(root), (float8)-1.0f);
            memcpy(t_out + i, &out, sizeof out);  // bounds-checked vector store
        }
    }
    // Tail: the scalar solver for the n % 8 remainder -- and, since the conic kind
    // takes neither vector branch above (i stays 0), the whole row for conic.
    for (; i < n; i++) {
        float t;
        canvas2d_vec2 const p = { .x = (float)x0 + (float)i + 0.5f, .y = y };
        t_out[i] = canvas2d_gradient_param(gr, p, &t) ? t : -1.0f;
    }
}

// Eight unpremultiplied pixels as four channel planes -- the planar shape of
// canvas2d_px8 (canvas2d_planar.h), kept a distinct type for the same reason
// canvas2d_unpremul is distinct from canvas2d_premul.
typedef struct {
    half8 r, g, b, a;
} gradpx8;

static gradpx8 gradpx8_sel(short8 m, gradpx8 x, gradpx8 y) {
    return (gradpx8){ half8_if_then_else(m, x.r, y.r), half8_if_then_else(m, x.g, y.g),
                      half8_if_then_else(m, x.b, y.b), half8_if_then_else(m, x.a, y.a) };
}

// The planar->AoS seam for unpremultiplied colours, mirroring canvas2d_px8_store:
// the __counted_by(8) parameter makes the implicit conversion at the call site
// the bounds check, one per 8-pixel slab.
static void gradpx8_store(canvas2d_unpremul *__counted_by(8) p, gradpx8 px) {
    float16x8x4_t v = { { (float16x8_t)px.r, (float16x8_t)px.g,
                          (float16x8_t)px.b, (float16x8_t)px.a } };
    vst4q_f16((float16_t *)p, v);
}

// Colour for a row of solved parameters, eight pixels per step: each lane finds
// its surrounding stop pair by a compare+select scan over the (sorted) stops --
// every lane visits every stop, no gathers -- then runs the scalar lerp's exact
// arithmetic eight wide (lerp_t in f32 with the same true divide and span
// guard, narrowed once to _Float16 for the colour lerp).  Lane for lane
// bit-identical to canvas2d_gradient_color_at, the semantic reference; t < 0 --
// the row solver's "outside" sentinel -- paints transparent black.
//
// Every NON-default interpolation (any space x alpha but sRGB + unpremul) takes
// the WHOLE row through the scalar evaluator (the same path as the n % 8 tail).
// The general colour lerp is a per-lane transfer/cbrt/cube behind the same data-
// dependent stop search, with no portable vector spelling (libm has no half8
// cbrt -- exactly canvas2d_color.c's situation), and it is not on a profiled hot
// path; routing it through canvas2d_gradient_color_at makes planar == scalar bit-
// identical BY CONSTRUCTION (one evaluator, called per lane) and leaves the
// default sRGB+unpremul planar kernel below textually unchanged and byte-stable.
void canvas2d_gradient_color_row(struct canvas2d_gradient const *gr,
                             float const *__counted_by(n) t, int n,
                             canvas2d_unpremul *__counted_by(n) out) {
    if (!grad_is_default(gr)) {
        for (int k = 0; k < n; k++) {
            out[k] = t[k] >= 0.0f ? canvas2d_gradient_color_at(gr, t[k])
                                  : canvas2d_unpremul_of(0.0f, 0.0f, 0.0f, 0.0f);
        }
        return;
    }
    int const sc = gr->stop_count;
    int i = 0;
    if (sc > 0) {
        // Per-stop lane constants, splatted once per row: offsets in f32 (they
        // are geometry, like the parameter), channels in _Float16 (the colour
        // compute type, docs/decisions/color-axis.md).
        float8 off[CANVAS2D_STOPS_MAX];
        gradpx8 col[CANVAS2D_STOPS_MAX];
        for (int s = 0; s < sc; s++) {
            canvas2d_stop const st = gr->stops[s];
            off[s] = (float8)st.offset;
            col[s] = (gradpx8){ (half8)st.color.r, (half8)st.color.g,
                                (half8)st.color.b, (half8)st.color.a };
        }
        int const last = sc - 1;
        half8 const zero = (half8)(_Float16)0.0f;
        for (; i + 8 <= n; i += 8) {
            float8 tv;
            memcpy(&tv, t + i, sizeof tv);  // bounds-checked vector load
            // Stop search: lo starts at stop 0 and advances to the last stop
            // whose offset is strictly below t, hi to the stop after it --
            // strict, so a lane between coincident offsets resolves to the
            // same pair as the scalar scan.
            float8  lo_off = off[0], hi_off = off[last > 0 ? 1 : 0];
            gradpx8 lo     = col[0], hi     = col[last > 0 ? 1 : 0];
            for (int s = 1; s + 1 < sc; s++) {
                int8 const m = tv > off[s];
                short8 const mh = __builtin_convertvector(m, short8);
                lo_off = float8_if_then_else(m, off[s],     lo_off);
                hi_off = float8_if_then_else(m, off[s + 1], hi_off);
                lo = gradpx8_sel(mh, col[s],     lo);
                hi = gradpx8_sel(mh, col[s + 1], hi);
            }
            // The scalar lerp, eight lanes wide.  Lanes the edge selects below
            // overwrite may divide by a zero span; the bitwise selects discard
            // the resulting inf/NaN exactly.
            float8 const span = hi_off - lo_off;
            float8 const lt32 = float8_if_then_else(span > 0.0f,
                                                    (tv - lo_off) / span,
                                                    (float8)0.0f);
            half8 const lerp_t = __builtin_convertvector(lt32, half8);
            gradpx8 c = { lo.r + (hi.r - lo.r) * lerp_t,
                          lo.g + (hi.g - lo.g) * lerp_t,
                          lo.b + (hi.b - lo.b) * lerp_t,
                          lo.a + (hi.a - lo.a) * lerp_t };
            // Edge and sentinel lanes, in the scalar path's precedence: t at or
            // past the last stop takes the last colour, t at or before the
            // first takes the first (applied second, so it wins ties exactly
            // like the scalar's early-out order), and t < 0 is transparent.
            short8 mlast  = __builtin_convertvector(tv >= off[last],    short8);
            short8 const mfirst = __builtin_convertvector(tv <= off[0],       short8);
            short8 mout   = __builtin_convertvector(tv <  (float8)0.0f, short8);
            c = gradpx8_sel(mlast,  col[last], c);
            c = gradpx8_sel(mfirst, col[0],    c);
            c = gradpx8_sel(mout,   (gradpx8){ zero, zero, zero, zero }, c);
            gradpx8_store(out + i, c);
        }
    }
    // Tail: the scalar evaluator for the n % 8 remainder (and, with no stops,
    // the whole row -- color_at is the transparent-black constant then).
    for (; i < n; i++) {
        out[i] = t[i] >= 0.0f ? canvas2d_gradient_color_at(gr, t[i])
                              : canvas2d_unpremul_of(0.0f, 0.0f, 0.0f, 0.0f);
    }
}
