#include "cnvs_filter.h"

#include <math.h>
#include <string.h>

// Rec.709 luminance weights, full precision (the Filter Effects spec's
// grayscale() constants; its saturate()/hueRotate() tables print the same
// weights rounded to three digits, so grayscale(1) == saturate(0) here).
static float const LR = 0.2126f;
static float const LG = 0.7152f;
static float const LB = 0.0722f;

// rgb' = k*rgb + c*a: the shared form of brightness (c = 0), contrast, invert,
// and opacity (which also scales alpha by k).
static cnvs_filter scale_offset(float k, float c) {
    return (cnvs_filter){
        .m   = { k, 0.0f, 0.0f, 0.0f, k, 0.0f, 0.0f, 0.0f, k },
        .off = { c, c, c },
        .ka  = 1.0f,
    };
}

cnvs_filter cnvs_filter_brightness(float amount) {
    return scale_offset(amount, 0.0f);
}

// contrast(k) pivots about mid-gray: unpremultiplied c' = k*c + (0.5 - 0.5k),
// whose constant term scales by alpha under premultiplication.
cnvs_filter cnvs_filter_contrast(float amount) {
    return scale_offset(amount, 0.5f - 0.5f * amount);
}

// invert(k) lerps each channel toward its complement: c' = c*(1 - 2k) + k.
cnvs_filter cnvs_filter_invert(float amount) {
    return scale_offset(1.0f - 2.0f * amount, amount);
}

cnvs_filter cnvs_filter_opacity(float amount) {
    cnvs_filter f = scale_offset(amount, 0.0f);
    f.ka = amount;
    return f;
}

// saturate(s) interpolates from the luminance projection L (s = 0: every
// channel becomes the pixel's luminance) through identity (s = 1) and beyond:
// M = L + s*(I - L), where each row of L is the luminance weights.
static cnvs_filter saturate_matrix(float s) {
    return (cnvs_filter){
        .m = {
            LR + (1.0f - LR) * s,  LG - LG * s,           LB - LB * s,
            LR - LR * s,           LG + (1.0f - LG) * s,  LB - LB * s,
            LR - LR * s,           LG - LG * s,           LB + (1.0f - LB) * s,
        },
        .off = { 0.0f, 0.0f, 0.0f },
        .ka  = 1.0f,
    };
}

cnvs_filter cnvs_filter_saturate(float amount) {
    return saturate_matrix(amount);
}

// grayscale(g) is the same interpolation run the other way (the spec's
// grayscale matrix is its saturate matrix evaluated at 1 - amount).
cnvs_filter cnvs_filter_grayscale(float amount) {
    return saturate_matrix(1.0f - amount);
}

// sepia(g) interpolates identity (t = 1) toward the spec's sepia matrix (t = 0).
cnvs_filter cnvs_filter_sepia(float amount) {
    float t = 1.0f - amount;
    return (cnvs_filter){
        .m = {
            0.393f + 0.607f * t,  0.769f - 0.769f * t,  0.189f - 0.189f * t,
            0.349f - 0.349f * t,  0.686f + 0.314f * t,  0.168f - 0.168f * t,
            0.272f - 0.272f * t,  0.534f - 0.534f * t,  0.131f + 0.869f * t,
        },
        .off = { 0.0f, 0.0f, 0.0f },
        .ka  = 1.0f,
    };
}

// hue_rotate(theta): M = L + cos*(I - L) + sin*S.  The generator S's outer rows
// follow from the rotation's geometry; its middle row is pinned by luminance
// preservation (each column of S must satisfy lr*S0 + lg*S1 + lb*S2 = 0, so the
// whole M keeps a pixel's luminance for every theta).  Evaluated at the spec's
// three-digit weights this reproduces feColorMatrix hueRotate's table.
cnvs_filter cnvs_filter_hue_rotate(float radians) {
    float c = cosf(radians);
    float s = sinf(radians);
    float s10 = (LR * LR + LB * (1.0f - LR)) / LG;
    float s11 = LR - LB;
    float s12 = -(LR * (1.0f - LB) + LB * LB) / LG;
    return (cnvs_filter){
        .m = {
            LR + c * (1.0f - LR) - s * LR,
            LG - c * LG - s * LG,
            LB - c * LB + s * (1.0f - LB),

            LR - c * LR + s * s10,
            LG + c * (1.0f - LG) + s * s11,
            LB - c * LB + s * s12,

            LR - c * LR - s * (1.0f - LR),
            LG - c * LG + s * LG,
            LB + c * (1.0f - LB) + s * LB,
        },
        .off = { 0.0f, 0.0f, 0.0f },
        .ka  = 1.0f,
    };
}

// blur() carries only its box radius; the matrix part is the identity so the
// entry is inert if it ever reaches the colour loop below (it shouldn't -- the
// canvas pipeline dispatches blur entries to the separable passes instead).
cnvs_filter cnvs_filter_blur(int radius) {
    cnvs_filter f = scale_offset(1.0f, 0.0f);
    f.blur = radius;
    return f;
}

// drop-shadow() carries its offset, blur radius, and colour; identity matrix
// for the same inert-if-mishandled property as blur().
cnvs_filter cnvs_filter_drop_shadow(int dx, int dy, int radius,
                                    float r, float g, float b, float a) {
    cnvs_filter f = scale_offset(1.0f, 0.0f);
    f.blur = radius;
    f.shadow = true;
    f.dx = dx;
    f.dy = dy;
    f.color[0] = r;
    f.color[1] = g;
    f.color[2] = b;
    f.color[3] = a;
    return f;
}

// One pixel's four channels as a vector -- a cnvs_premul is four contiguous
// _Float16, so it loads straight into an h4 -- and two pixels as one 8-lane
// vector, the colour pipeline's wide unit (docs/decisions/color-axis.md).
typedef _Float16 h4 __attribute__((ext_vector_type(4)));
typedef _Float16 h8 __attribute__((ext_vector_type(8)));

// One pixel's matrix-multiply-add + premul clamp, in _Float16: q is the
// column fold cr*r + cg*g + cb*b + ca*a, where ca carries the offsets and ka
// so lane 3 lands ka*a; the clamp pins rgb into [0, clamp01(ka*a)] (the
// spec's per-function unpremultiplied [0,1] clamp, premultiplied) and lane 3
// to clamp01(ka*a) itself.
static h4 filter_px(h4 p, h4 cr, h4 cg, h4 cb, h4 ca) {
    h4 q = cr * p[0] + cg * p[1] + cb * p[2] + ca * p[3];
    q = __builtin_elementwise_max(q, (h4)(_Float16)0.0f);
    h4 lim = __builtin_elementwise_min((h4)q[3], (h4)(_Float16)1.0f);
    return __builtin_elementwise_min(q, lim);
}

void cnvs_filter_apply(cnvs_filter const *__counted_by(count) list, int count,
                       cnvs_premul *__counted_by(n) px, int n) {
    // Functions outermost: each entry's matrix is narrowed to _Float16 column
    // vectors once (one checked access per function, not per pixel), and the
    // per-pixel body runs two pixels (8 lanes) per step -- a bounds-checked
    // vector load, the matrix fold, the premul clamp, and a vector store, all
    // in f16 arithmetic with no widen/narrow converts.  An odd tail pixel
    // runs the same fold at 4 lanes.
    for (int f = 0; f < count; f++) {
        cnvs_filter const fn = list[f];
        h4 const cr = { (_Float16)fn.m[0], (_Float16)fn.m[3], (_Float16)fn.m[6],
                        (_Float16)0.0f };
        h4 const cg = { (_Float16)fn.m[1], (_Float16)fn.m[4], (_Float16)fn.m[7],
                        (_Float16)0.0f };
        h4 const cb = { (_Float16)fn.m[2], (_Float16)fn.m[5], (_Float16)fn.m[8],
                        (_Float16)0.0f };
        h4 const ca = { (_Float16)fn.off[0], (_Float16)fn.off[1],
                        (_Float16)fn.off[2], (_Float16)fn.ka };
        h8 const cr2 = __builtin_shufflevector(cr, cr, 0, 1, 2, 3, 0, 1, 2, 3);
        h8 const cg2 = __builtin_shufflevector(cg, cg, 0, 1, 2, 3, 0, 1, 2, 3);
        h8 const cb2 = __builtin_shufflevector(cb, cb, 0, 1, 2, 3, 0, 1, 2, 3);
        h8 const ca2 = __builtin_shufflevector(ca, ca, 0, 1, 2, 3, 0, 1, 2, 3);
        int i = 0;
        for (; i + 2 <= n; i += 2) {
            h8 p;
            memcpy(&p, &px[i], sizeof p);  // one bounds check, two pixels
            h8 q = cr2 * __builtin_shufflevector(p, p, 0, 0, 0, 0, 4, 4, 4, 4)
                 + cg2 * __builtin_shufflevector(p, p, 1, 1, 1, 1, 5, 5, 5, 5)
                 + cb2 * __builtin_shufflevector(p, p, 2, 2, 2, 2, 6, 6, 6, 6)
                 + ca2 * __builtin_shufflevector(p, p, 3, 3, 3, 3, 7, 7, 7, 7);
            q = __builtin_elementwise_max(q, (h8)(_Float16)0.0f);
            h8 lim = __builtin_elementwise_min(
                __builtin_shufflevector(q, q, 3, 3, 3, 3, 7, 7, 7, 7),
                (h8)(_Float16)1.0f);
            q = __builtin_elementwise_min(q, lim);
            memcpy(&px[i], &q, sizeof q);  // bounds-checked vector store
        }
        for (; i < n; i++) {
            h4 p;
            memcpy(&p, &px[i], sizeof p);
            h4 q = filter_px(p, cr, cg, cb, ca);
            memcpy(&px[i], &q, sizeof q);
        }
    }
}
