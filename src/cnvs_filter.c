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

// One pixel's four channels as a vector: a cnvs_premul is four contiguous
// _Float16, so it loads straight into an h4 (the read_unpremul idiom).
typedef _Float16 h4 __attribute__((ext_vector_type(4)));
typedef float f4 __attribute__((ext_vector_type(4)));

void cnvs_filter_apply(cnvs_filter const *__counted_by(count) list, int count,
                       cnvs_premul *__counted_by(n) px, int n) {
    // Functions outermost: each entry's matrix is copied to locals once (one
    // checked access per function, not per pixel), and the per-pixel body is a
    // bounds-checked vector load, a matrix-multiply-add, the premul clamp, and
    // a vector store.  Intermediate results round to _Float16 between
    // functions, the tile's own precision.
    for (int f = 0; f < count; f++) {
        cnvs_filter const fn = list[f];
        for (int i = 0; i < n; i++) {
            h4 ph;
            memcpy(&ph, &px[i], sizeof ph);  // bounds-checked vector load
            f4 p = __builtin_convertvector(ph, f4);
            float a = p[3] * fn.ka;
            a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);  // alpha in [0,1] first
            f4 q;
            q[0] = fn.m[0] * p[0] + fn.m[1] * p[1] + fn.m[2] * p[2] + fn.off[0] * p[3];
            q[1] = fn.m[3] * p[0] + fn.m[4] * p[1] + fn.m[5] * p[2] + fn.off[1] * p[3];
            q[2] = fn.m[6] * p[0] + fn.m[7] * p[1] + fn.m[8] * p[2] + fn.off[2] * p[3];
            q[3] = a;
            // The premul invariant rgb in [0, a] -- the spec's per-function
            // [0,1] unpremultiplied clamp.  Lane 3 passes through (min(a,a)).
            q = __builtin_elementwise_max(q, (f4)0.0f);
            q = __builtin_elementwise_min(q, (f4)a);
            h4 out = __builtin_convertvector(q, h4);
            memcpy(&px[i], &out, sizeof out);  // bounds-checked vector store
        }
    }
}
