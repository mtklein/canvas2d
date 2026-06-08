#include "cnvs_gradient.h"

#include <math.h>

static float clamp01(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

void cnvs_gradient_add_stop(cnvs_gradient *gr, float offset, gpu_rgba color) {
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

gpu_rgba cnvs_gradient_color_at(cnvs_gradient const *gr, float t) {
    int n = gr->stop_count;
    if (n == 0) {
        return (gpu_rgba){ .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f };
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
            return (gpu_rgba){
                .r = lo.color.r + (hi.color.r - lo.color.r) * u,
                .g = lo.color.g + (hi.color.g - lo.color.g) * u,
                .b = lo.color.b + (hi.color.b - lo.color.b) * u,
                .a = lo.color.a + (hi.color.a - lo.color.a) * u,
            };
        }
    }
    return gr->stops[n - 1].color;  // unreachable: t < last offset handled above
}

bool cnvs_gradient_param(cnvs_gradient const *gr, cnvs_vec2 p, float *__single t) {
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

gpu_rgba cnvs_gradient_sample(cnvs_gradient const *gr, cnvs_vec2 p, float alpha) {
    float t;
    gpu_rgba c;
    if (cnvs_gradient_param(gr, p, &t)) {
        c = cnvs_gradient_color_at(gr, t);
    } else {
        c = (gpu_rgba){ .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f };
    }
    c.a *= alpha;
    return c;
}
