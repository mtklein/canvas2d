#include "cnvs_gradient.h"
#include "cnvs_planar.h"

#include <math.h>
#include <string.h>

static float clamp01(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

void cnvs_gradient_add_stop(cnvs_gradient *gr, float offset, cnvs_unpremul color) {
    if (gr->stop_count >= CNVS_MAX_STOPS) {
        return;
    }
    float o = clamp01(offset);
    // Insertion sort keeps stops ordered; ties land after equal offsets, so a
    // later addColorStop at the same offset wins on the high side (as in CSS).
    int i = gr->stop_count;
    while (i > 0 && gr->stops[i - 1].offset > o) {
        gr->stops[i] = gr->stops[i - 1];
        i -= 1;
    }
    gr->stops[i] = (cnvs_stop){ .offset = o, .color = color };
    gr->stop_count += 1;
}

cnvs_unpremul cnvs_gradient_color_at(cnvs_gradient const *gr, float t) {
    int n = gr->stop_count;
    if (n == 0) {
        return cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 0.0f);
    }
    t = clamp01(t);
    if (t <= gr->stops[0].offset) {
        return gr->stops[0].color;
    }
    if (t >= gr->stops[n - 1].offset) {
        return gr->stops[n - 1].color;
    }
    for (int i = 0; i + 1 < n; i++) {
        cnvs_stop lo = gr->stops[i];
        cnvs_stop hi = gr->stops[i + 1];
        if (t <= hi.offset) {
            // The parameter and stop offsets are geometry and stay f32; the
            // colour lerp itself runs in _Float16.  The guard is the minimal
            // one: it keeps the divide defined at coincident stops (span == 0
            // takes lo).
            float span = hi.offset - lo.offset;
            float u = span > 0.0f ? (t - lo.offset) / span : 0.0f;
            half4 lov = { lo.color.r, lo.color.g, lo.color.b, lo.color.a };
            half4 hiv = { hi.color.r, hi.color.g, hi.color.b, hi.color.a };
            half4 c = lov + (hiv - lov) * (_Float16)u;
            return (cnvs_unpremul){ .r = c[0], .g = c[1], .b = c[2], .a = c[3] };
        }
    }
    return gr->stops[n - 1].color;  // unreachable: t < last offset handled above
}

bool cnvs_gradient_param(cnvs_gradient const *gr, cnvs_vec2 p, float *__single t) {
    if (gr->kind == CNVS_GRAD_CONIC) {
        // Angle of p about the centre, measured clockwise from +x (device space is
        // y-down, so atan2 already increases clockwise), minus the start angle;
        // wrapped into [0,1).  Every point has a parameter, so this never misses.
        float ang = atan2f(p.y - gr->p0.y, p.x - gr->p0.x) - gr->angle;
        float v = ang * 0.15915494309189535f;  // * 1/(2*pi)
        *t = v - floorf(v);
        return true;
    }
    if (gr->kind == CNVS_GRAD_LINEAR) {
        float dx = gr->p1.x - gr->p0.x;
        float dy = gr->p1.y - gr->p0.y;
        float denom = dx * dx + dy * dy;
        float v = denom > 1e-12f
                      ? ((p.x - gr->p0.x) * dx + (p.y - gr->p0.y) * dy) / denom
                      : 0.0f;
        *t = clamp01(v);
        return true;
    }
    // Radial: find the largest t with (r0 + t*dr) >= 0 such that p lies on the
    // circle centred at lerp(p0,p1,t) with that radius.  Expanding
    // |p - C(t)| = R(t) gives the quadratic a t^2 + b t + c = 0.
    float cdx = gr->p1.x - gr->p0.x;
    float cdy = gr->p1.y - gr->p0.y;
    float dr  = gr->r1   - gr->r0  ;
    float pdx = p.x - gr->p0.x;
    float pdy = p.y - gr->p0.y;
    float a = cdx * cdx + cdy * cdy - dr * dr;
    float b = -2.0f * (cdx * pdx + cdy * pdy + gr->r0 * dr);
    float c = pdx * pdx + pdy * pdy - gr->r0 * gr->r0;
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
        float disc = b * b - 4.0f * a * c;
        if (disc < 0.0f) {
            return false;
        }
        float sq = sqrtf(disc);
        float hi = (-b + sq) / (2.0f * a);
        float lo = (-b - sq) / (2.0f * a);
        if (hi < lo) {
            float tmp = hi;
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
    *t = clamp01(root);
    return true;
}

cnvs_unpremul cnvs_gradient_sample(cnvs_gradient const *gr, cnvs_vec2 p, float alpha) {
    float t;
    cnvs_unpremul c;
    if (cnvs_gradient_param(gr, p, &t)) {
        c = cnvs_gradient_color_at(gr, t);
    } else {
        c = cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 0.0f);
    }
    c.a = (_Float16)((float)c.a * alpha);
    return c;
}

typedef float gradf8 __attribute__((ext_vector_type(8)));
typedef int gradi8 __attribute__((ext_vector_type(8)));

// Bit-exact f32 lane select, the 32-bit twin of half8_sel: a where the mask
// lane is set (-1, from a vector comparison), else b.  Bitwise: the selected
// lane passes through untouched (an arithmetic b + (a-b)*m re-rounds it), and
// an unselected lane's inf/NaN is discarded exactly.
static gradf8 vsel_bits(gradi8 m, gradf8 a, gradf8 b) {
    return (gradf8)(((gradi8)a & m) | ((gradi8)b & ~m));
}

static gradf8 vclamp01(gradf8 v) {
    v = __builtin_elementwise_max((gradf8)0.0f, v);
    return __builtin_elementwise_min((gradf8)1.0f, v);
}

// Parameter solve for a horizontal run of `n` pixel centres
// (x0 + i + 0.5, y), i in [0, n).  Writes t in [0,1] per pixel, or -1 where the
// point has no gradient parameter (the radial "outside" case) so the caller
// paints transparent.  Along a row only x varies, so the per-pixel work
// vectorizes 8 wide.
void cnvs_gradient_param_row(cnvs_gradient const *gr, int x0, float y, int n,
                             float *__counted_by(n) t_out) {
    gradf8 const lane = { 0, 1, 2, 3, 4, 5, 6, 7 };
    float base = (float)x0 + 0.5f - gr->p0.x;
    int i = 0;
    if (gr->kind == CNVS_GRAD_LINEAR) {
        float dx = gr->p1.x - gr->p0.x;
        float dy = gr->p1.y - gr->p0.y;
        float denom = dx * dx + dy * dy;
        float inv = denom > 1e-12f ? 1.0f / denom : 0.0f;  // inv == 0 -> all-zero t
        float cy = (y - gr->p0.y) * dy;                    // y term constant per row
        for (; i + 8 <= n; i += 8) {
            gradf8 px = base + ((float)i + lane);
            gradf8 v = vclamp01((px * dx + cy) * inv);  // clamp01(((p-p0).d)/|d|^2)
            memcpy(t_out + i, &v, sizeof v);            // bounds-checked vector store
        }
    } else if (gr->kind == CNVS_GRAD_RADIAL) {
        float cdx = gr->p1.x - gr->p0.x;
        float cdy = gr->p1.y - gr->p0.y;
        float dr  = gr->r1   - gr->r0  ;
        float r0 = gr->r0;
        float a = cdx * cdx + cdy * cdy - dr * dr;
        float pdy = y - gr->p0.y;
        float bconst = cdy * pdy + r0 * dr;  // b = -2*(cdx*pdx + bconst)
        float cconst = pdy * pdy - r0 * r0;  // c = pdx*pdx + cconst
        bool a_lin = fabsf(a) < 1e-9f;       // a is constant along the row
        gradf8 inv2a = (gradf8)(a_lin ? 0.0f : 1.0f / (2.0f * a));
        for (; i + 8 <= n; i += 8) {
            gradf8 pdx = base + ((float)i + lane);
            gradf8 b = -2.0f * (cdx * pdx + bconst);
            gradf8 c = pdx * pdx + cconst;
            gradf8 root;
            gradi8 valid;
            if (a_lin) {  // degenerate: the t^2 term vanishes -> b t + c = 0
                root = -c / b;
                valid = ((b > 1e-12f) | (b < -1e-12f)) & (r0 + root * dr >= 0.0f);
            } else {
                gradf8 disc = b * b - 4.0f * a * c;
                gradf8 sq = __builtin_elementwise_sqrt(
                    __builtin_elementwise_max((gradf8)0.0f, disc));
                gradf8 r1_ = (-b + sq) * inv2a;
                gradf8 r2_ = (-b - sq) * inv2a;
                gradf8 hi = __builtin_elementwise_max(r1_, r2_);
                gradf8 lo = __builtin_elementwise_min(r1_, r2_);
                gradi8 hiok = r0 + hi * dr >= 0.0f;  // prefer the larger valid root
                gradi8 look = r0 + lo * dr >= 0.0f;
                root = vsel_bits(hiok, hi, vsel_bits(look, lo, (gradf8)0.0f));
                valid = (disc >= 0.0f) & (hiok | look);
            }
            gradf8 out = vsel_bits(valid, vclamp01(root), (gradf8)-1.0f);
            memcpy(t_out + i, &out, sizeof out);  // bounds-checked vector store
        }
    }
    // Tail: the scalar solver for the n % 8 remainder -- and, since the conic kind
    // takes neither vector branch above (i stays 0), the whole row for conic.
    for (; i < n; i++) {
        float t;
        cnvs_vec2 p = { .x = (float)x0 + (float)i + 0.5f, .y = y };
        t_out[i] = cnvs_gradient_param(gr, p, &t) ? t : -1.0f;
    }
}

// Eight unpremultiplied pixels as four channel planes -- the planar shape of
// cnvs_px8 (cnvs_planar.h), kept a distinct type for the same reason
// cnvs_unpremul is distinct from cnvs_premul.
typedef struct {
    half8 r, g, b, a;
} gradpx8;

static gradpx8 gradpx8_sel(mask8 m, gradpx8 x, gradpx8 y) {
    return (gradpx8){ half8_sel(m, x.r, y.r), half8_sel(m, x.g, y.g),
                      half8_sel(m, x.b, y.b), half8_sel(m, x.a, y.a) };
}

// The planar->AoS seam for unpremultiplied colours, mirroring cnvs_px8_store:
// the __counted_by(8) parameter makes the implicit conversion at the call site
// the bounds check, one per 8-pixel block.
static void gradpx8_store(cnvs_unpremul *__counted_by(8) p, gradpx8 px) {
    float16x8x4_t v = { { (float16x8_t)px.r, (float16x8_t)px.g,
                          (float16x8_t)px.b, (float16x8_t)px.a } };
    vst4q_f16((float16_t *)p, v);
}

// Colour for a row of solved parameters, eight pixels per step: each lane finds
// its surrounding stop pair by a compare+select scan over the (sorted) stops --
// every lane visits every stop, no gathers -- then runs the scalar lerp's exact
// arithmetic eight wide (u in f32 with the same true divide and span guard,
// narrowed once to _Float16 for the colour lerp).  Lane for lane bit-identical
// to cnvs_gradient_color_at, the semantic reference; t < 0 -- the row
// solver's "outside" sentinel -- paints transparent black.
void cnvs_gradient_color_row(cnvs_gradient const *gr,
                             float const *__counted_by(n) t, int n,
                             cnvs_unpremul *__counted_by(n) out) {
    int const sc = gr->stop_count;
    int i = 0;
    if (sc > 0) {
        // Per-stop lane constants, splatted once per row: offsets in f32 (they
        // are geometry, like the parameter), channels in _Float16 (the colour
        // compute type, docs/decisions/color-axis.md).
        gradf8 off[CNVS_MAX_STOPS];
        gradpx8 col[CNVS_MAX_STOPS];
        for (int s = 0; s < sc; s++) {
            cnvs_stop st = gr->stops[s];
            off[s] = (gradf8)st.offset;
            col[s] = (gradpx8){ (half8)st.color.r, (half8)st.color.g,
                                (half8)st.color.b, (half8)st.color.a };
        }
        int const last = sc - 1;
        half8 const zero = (half8)(_Float16)0.0f;
        for (; i + 8 <= n; i += 8) {
            gradf8 tv;
            memcpy(&tv, t + i, sizeof tv);  // bounds-checked vector load
            // Stop search: lo starts at stop 0 and advances to the last stop
            // whose offset is strictly below t, hi to the stop after it --
            // strict, so a lane between coincident offsets resolves to the
            // same pair as the scalar scan.
            gradf8  lo_off = off[0], hi_off = off[last > 0 ? 1 : 0];
            gradpx8 lo     = col[0], hi     = col[last > 0 ? 1 : 0];
            for (int s = 1; s + 1 < sc; s++) {
                gradi8 m = tv > off[s];
                mask8 mh = __builtin_convertvector(m, mask8);
                lo_off = vsel_bits(m, off[s],     lo_off);
                hi_off = vsel_bits(m, off[s + 1], hi_off);
                lo = gradpx8_sel(mh, col[s],     lo);
                hi = gradpx8_sel(mh, col[s + 1], hi);
            }
            // The scalar lerp, eight lanes wide.  Lanes the edge selects below
            // overwrite may divide by a zero span; the bitwise selects discard
            // the resulting inf/NaN exactly.
            gradf8 span = hi_off - lo_off;
            gradf8 u32 = vsel_bits(span > 0.0f, (tv - lo_off) / span,
                                   (gradf8)0.0f);
            half8 u = __builtin_convertvector(u32, half8);
            gradpx8 c = { lo.r + (hi.r - lo.r) * u, lo.g + (hi.g - lo.g) * u,
                          lo.b + (hi.b - lo.b) * u, lo.a + (hi.a - lo.a) * u };
            // Edge and sentinel lanes, in the scalar path's precedence: t at or
            // past the last stop takes the last colour, t at or before the
            // first takes the first (applied second, so it wins ties exactly
            // like the scalar's early-out order), and t < 0 is transparent.
            mask8 mlast  = __builtin_convertvector(tv >= off[last],    mask8);
            mask8 mfirst = __builtin_convertvector(tv <= off[0],       mask8);
            mask8 mout   = __builtin_convertvector(tv <  (gradf8)0.0f, mask8);
            c = gradpx8_sel(mlast,  col[last], c);
            c = gradpx8_sel(mfirst, col[0],    c);
            c = gradpx8_sel(mout,   (gradpx8){ zero, zero, zero, zero }, c);
            gradpx8_store(out + i, c);
        }
    }
    // Tail: the scalar evaluator for the n % 8 remainder (and, with no stops,
    // the whole row -- color_at is the transparent-black constant then).
    for (; i < n; i++) {
        out[i] = t[i] >= 0.0f ? cnvs_gradient_color_at(gr, t[i])
                              : cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 0.0f);
    }
}
