#pragma once

// Per-pixel premultiplied compositing: the W3C composite + blend math (the same
// formulas the Metal shader runs), in checked C.  The software compositor calls
// cnvs_blend once per pixel with the clip-attenuated source and the backdrop, both
// premultiplied, and gets the premultiplied result.
//
// For the blend modes the result is co = s*(1-da) + d*(1-sa) + T with the
// premultiplied term T = sa*da*B(Cb,Cs); the polynomial modes have divide-free
// forms for T, while the intrinsically non-linear ones (dodge/burn/soft-light and
// the HSL set) un-premultiply once.  Porter-Duff operators are co = Fa*s + Fb*d.
// Switches run over (int)mode to stay clear of -Wswitch-enum.

#include "compositor.h"  // compositor_blend_mode, cnvs_premul

#include <math.h>

static inline _Float16 cnvs_clamp16(float v, float hi) {
    if (v < 0.0f) { v = 0.0f; }
    if (v > hi) { v = hi; }
    return (_Float16)v;
}

// Separable blend B(cb, cs) on unpremultiplied channels -- only the non-linear
// modes need it; the rest fold into the premultiplied term below.
static inline float cnvs_blend_sep(compositor_blend_mode mode, float cb, float cs) {
    switch ((int)mode) {
        case COMPOSITOR_COLOR_DODGE:
            return cb <= 0.0f ? 0.0f : cs >= 1.0f ? 1.0f : fminf(1.0f, cb / (1.0f - cs));
        case COMPOSITOR_COLOR_BURN:
            return cb >= 1.0f ? 1.0f : cs <= 0.0f ? 0.0f : 1.0f - fminf(1.0f, (1.0f - cb) / cs);
        case COMPOSITOR_SOFT_LIGHT: {
            float dd = cb <= 0.25f ? ((16.0f * cb - 12.0f) * cb + 4.0f) * cb : sqrtf(cb);
            return cs <= 0.5f ? cb - (1.0f - 2.0f * cs) * cb * (1.0f - cb)
                              : cb + (2.0f * cs - 1.0f) * (dd - cb);
        }
        default:
            return cs;
    }
}

// Premultiplied separable blend term T = sa*da*B for one channel (s,d premultiplied).
static inline float cnvs_blend_term(compositor_blend_mode mode,
                                    float s, float d, float sa, float da) {
    switch ((int)mode) {
        case COMPOSITOR_MULTIPLY:   return s * d;
        case COMPOSITOR_SCREEN:     return sa * d + da * s - s * d;
        case COMPOSITOR_OVERLAY:    return 2.0f * d <= da ? 2.0f * s * d
                                         : sa * da - 2.0f * (da - d) * (sa - s);
        case COMPOSITOR_DARKEN:     return fminf(s * da, d * sa);
        case COMPOSITOR_LIGHTEN:    return fmaxf(s * da, d * sa);
        case COMPOSITOR_HARD_LIGHT: return 2.0f * s <= sa ? 2.0f * s * d
                                         : sa * da - 2.0f * (da - d) * (sa - s);
        case COMPOSITOR_DIFFERENCE: return fabsf(s * da - d * sa);
        case COMPOSITOR_EXCLUSION:  return sa * d + da * s - 2.0f * s * d;
        default: {  // color-dodge / color-burn / soft-light
            float cs = sa > 0.0f ? s / sa : 0.0f;
            float cb = da > 0.0f ? d / da : 0.0f;
            return sa * da * cnvs_blend_sep(mode, cb, cs);
        }
    }
}

// Whole-colour helpers for the non-separable (HSL) blend modes.
typedef struct {
    float r, g, b;
} cnvs_rgb;

static inline float cnvs_lum(cnvs_rgb c) {
    return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b;
}

static inline cnvs_rgb cnvs_clip_color(cnvs_rgb c) {
    float l = cnvs_lum(c);
    float n = fminf(c.r, fminf(c.g, c.b));
    float x = fmaxf(c.r, fmaxf(c.g, c.b));
    if (n < 0.0f) {
        float k = l / (l - n);
        c.r = l + (c.r - l) * k;
        c.g = l + (c.g - l) * k;
        c.b = l + (c.b - l) * k;
    }
    if (x > 1.0f) {
        float k = (1.0f - l) / (x - l);
        c.r = l + (c.r - l) * k;
        c.g = l + (c.g - l) * k;
        c.b = l + (c.b - l) * k;
    }
    return c;
}

static inline cnvs_rgb cnvs_set_lum(cnvs_rgb c, float l) {
    float dl = l - cnvs_lum(c);
    c.r += dl;
    c.g += dl;
    c.b += dl;
    return cnvs_clip_color(c);
}

static inline float cnvs_sat(cnvs_rgb c) {
    return fmaxf(c.r, fmaxf(c.g, c.b)) - fminf(c.r, fminf(c.g, c.b));
}

// Set saturation: max channel -> s, min -> 0, mid -> proportional (the spec's
// min/mid/max walk, vectorized).
static inline cnvs_rgb cnvs_set_sat(cnvs_rgb c, float s) {
    float mn = fminf(c.r, fminf(c.g, c.b));
    float mx = fmaxf(c.r, fmaxf(c.g, c.b));
    if (mx <= mn) {
        return (cnvs_rgb){ .r = 0.0f, .g = 0.0f, .b = 0.0f };
    }
    float k = s / (mx - mn);
    return (cnvs_rgb){ .r = (c.r - mn) * k, .g = (c.g - mn) * k, .b = (c.b - mn) * k };
}

static inline cnvs_rgb cnvs_blend_nonsep(compositor_blend_mode mode,
                                         cnvs_rgb cb, cnvs_rgb cs) {
    switch ((int)mode) {
        case COMPOSITOR_HUE:        return cnvs_set_lum(cnvs_set_sat(cs, cnvs_sat(cb)), cnvs_lum(cb));
        case COMPOSITOR_SATURATION: return cnvs_set_lum(cnvs_set_sat(cb, cnvs_sat(cs)), cnvs_lum(cb));
        case COMPOSITOR_COLOR:      return cnvs_set_lum(cs, cnvs_lum(cb));
        default:                    return cnvs_set_lum(cb, cnvs_lum(cs));  // luminosity
    }
}

// Composite a clip-attenuated premultiplied source over a premultiplied backdrop.
static inline cnvs_premul cnvs_blend(cnvs_premul src, cnvs_premul dst,
                                     compositor_blend_mode mode) {
    float sa = (float)src.a, da = (float)dst.a;
    float sr = (float)src.r, sg = (float)src.g, sb = (float)src.b;
    float dr = (float)dst.r, dg = (float)dst.g, db = (float)dst.b;
    float cor, cog, cob, ao;

    if ((int)mode <= COMPOSITOR_COPY) {
        // Porter-Duff: co = Fa*s + Fb*d, ao = Fa*sa + Fb*da.
        float fa, fb;
        switch ((int)mode) {
            case COMPOSITOR_SRC_IN:   fa = da;        fb = 0.0f;       break;
            case COMPOSITOR_SRC_OUT:  fa = 1.0f - da; fb = 0.0f;       break;
            case COMPOSITOR_SRC_ATOP: fa = da;        fb = 1.0f - sa;  break;
            case COMPOSITOR_DST_OVER: fa = 1.0f - da; fb = 1.0f;       break;
            case COMPOSITOR_DST_IN:   fa = 0.0f;      fb = sa;         break;
            case COMPOSITOR_DST_OUT:  fa = 0.0f;      fb = 1.0f - sa;  break;
            case COMPOSITOR_DST_ATOP: fa = 1.0f - da; fb = sa;         break;
            case COMPOSITOR_XOR:      fa = 1.0f - da; fb = 1.0f - sa;  break;
            case COMPOSITOR_LIGHTER:  fa = 1.0f;      fb = 1.0f;       break;
            case COMPOSITOR_COPY:     fa = 1.0f;      fb = 0.0f;       break;
            default:                  fa = 1.0f;      fb = 1.0f - sa;  break;  // source-over
        }
        cor = fa * sr + fb * dr;
        cog = fa * sg + fb * dg;
        cob = fa * sb + fb * db;
        ao  = fa * sa + fb * da;
    } else {
        // Blend modes composite as source-over of the blended colour.
        float tr, tg, tb;
        if ((int)mode >= COMPOSITOR_HUE) {
            cnvs_rgb cs = { .r = sa > 0.0f ? sr / sa : 0.0f,
                            .g = sa > 0.0f ? sg / sa : 0.0f,
                            .b = sa > 0.0f ? sb / sa : 0.0f };
            cnvs_rgb cb = { .r = da > 0.0f ? dr / da : 0.0f,
                            .g = da > 0.0f ? dg / da : 0.0f,
                            .b = da > 0.0f ? db / da : 0.0f };
            cnvs_rgb bl = cnvs_blend_nonsep(mode, cb, cs);
            tr = sa * da * bl.r;
            tg = sa * da * bl.g;
            tb = sa * da * bl.b;
        } else {
            tr = cnvs_blend_term(mode, sr, dr, sa, da);
            tg = cnvs_blend_term(mode, sg, dg, sa, da);
            tb = cnvs_blend_term(mode, sb, db, sa, da);
        }
        cor = sr * (1.0f - da) + dr * (1.0f - sa) + tr;
        cog = sg * (1.0f - da) + dg * (1.0f - sa) + tg;
        cob = sb * (1.0f - da) + db * (1.0f - sa) + tb;
        ao  = sa + da * (1.0f - sa);
    }

    float aoc = fminf(ao, 1.0f);  // additive 'lighter' can exceed 1
    return (cnvs_premul){ .r = cnvs_clamp16(cor, aoc), .g = cnvs_clamp16(cog, aoc),
                          .b = cnvs_clamp16(cob, aoc), .a = (_Float16)aoc };
}
