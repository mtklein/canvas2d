#include "cnvs_gradient.h"

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
            float span = hi.offset - lo.offset;
            float u = span > 1e-9f ? (t - lo.offset) / span : 0.0f;
            // Interpolate in float, then narrow once via cnvs_unpremul_of.
            return cnvs_unpremul_of(
                (float)lo.color.r + ((float)hi.color.r - (float)lo.color.r) * u,
                (float)lo.color.g + ((float)hi.color.g - (float)lo.color.g) * u,
                (float)lo.color.b + ((float)hi.color.b - (float)lo.color.b) * u,
                (float)lo.color.a + ((float)hi.color.a - (float)lo.color.a) * u);
        }
    }
    return gr->stops[n - 1].color;  // unreachable: t < last offset handled above
}

void cnvs_gradient_build_ramp(cnvs_gradient const *gr,
                              cnvs_unpremul *__counted_by(n) ramp, int n) {
    if (n <= 0) {
        return;
    }
    if (n == 1) {
        ramp[0] = cnvs_gradient_color_at(gr, 0.0f);
        return;
    }
    float inv = 1.0f / (float)(n - 1);
    for (int i = 0; i < n; i++) {
        ramp[i] = cnvs_gradient_color_at(gr, (float)i * inv);
    }
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
    float dr = gr->r1 - gr->r0;
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

// Element-wise select (C's ?: collapses a vector condition to scalar, so do it by
// hand): a where the mask lane is true (-1 from a comparison), else b.
static gradf8 vsel(gradf8 a, gradf8 b, gradi8 mask) {
    gradf8 m = -__builtin_convertvector(mask, gradf8);  // 1.0 where true, else 0.0
    return b + (a - b) * m;
}

static gradf8 vclamp01(gradf8 v) {
    v = __builtin_elementwise_max((gradf8)0.0f, v);
    return __builtin_elementwise_min((gradf8)1.0f, v);
}

// Parameter solve for a horizontal run of `n` pixel centres
// (x0 + i + 0.5, y), i in [0, n).  Writes t in [0,1] per pixel, or -1 where the
// point has no gradient parameter (the radial "outside" case) so the caller paints
// transparent.  Along a row only x varies, so the per-pixel work vectorizes 8 wide;
// it agrees with cnvs_gradient_param to <1e-6 in t (a tail handles n % 8 scalar).
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
        float dr = gr->r1 - gr->r0;
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
                root = vsel(hi, vsel(lo, (gradf8)0.0f, look), hiok);
                valid = (disc >= 0.0f) & (hiok | look);
            }
            gradf8 out = vsel(vclamp01(root), (gradf8)-1.0f, valid);
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
