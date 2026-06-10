// Software compositor backend for compositor.h: the target is a premultiplied
// cnvs_premul buffer, blend() runs the per-pixel blend over the tiles, read()
// copies it out.

#include "compositor.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// W3C composite + blend math, premultiplied throughout.  Blend modes:
// co = s*(1-da) + d*(1-sa) + T, ao = sa + da*(1-sa), with premultiplied term
// T = sa*da*B(Cb,Cs); the polynomial modes have divide-free T, the non-linear ones
// (dodge/burn/soft-light, HSL) un-premultiply once.  Porter-Duff: co = Fa*s + Fb*d.
// Switches run over (int)mode to avoid -Wswitch-enum.

// Clamp a channel to [0, hi] and store it as a half (round to nearest-even).
static _Float16 clamp16(float v, float hi) {
    if (v < 0.0f) { v = 0.0f; }
    if (v > hi) { v = hi; }
    return (_Float16)v;
}

// Separable blend B(cb, cs), unpremultiplied; only the non-linear modes need it.
static float blend_sep(compositor_blend_mode mode, float cb, float cs) {
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

// Premultiplied separable term T = sa*da*B for one channel (s, d premultiplied).
static float blend_term(compositor_blend_mode mode,
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
            return sa * da * blend_sep(mode, cb, cs);
        }
    }
}

// One premultiplied pixel as a vector: cnvs_premul is four contiguous _Float16.
typedef _Float16 blendh4 __attribute__((ext_vector_type(4)));
typedef float blendf4 __attribute__((ext_vector_type(4)));
// Two premultiplied pixels as one 8-lane _Float16 vector -- a full 128-bit
// NEON register of native fp16 arithmetic, the colour pipeline's wide unit
// (docs/decisions/color-axis.md).
typedef _Float16 blendh8 __attribute__((ext_vector_type(8)));

typedef struct {
    float r, g, b;
} rgb;

static float lum(rgb c) {
    return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b;
}

static rgb clip_color(rgb c) {
    float l = lum(c);
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

static rgb set_lum(rgb c, float l) {
    float dl = l - lum(c);
    c.r += dl;
    c.g += dl;
    c.b += dl;
    return clip_color(c);
}

static float sat(rgb c) {
    return fmaxf(c.r, fmaxf(c.g, c.b)) - fminf(c.r, fminf(c.g, c.b));
}

// Set saturation: max channel -> s, min -> 0, mid proportional.
static rgb set_sat(rgb c, float s) {
    float mn = fminf(c.r, fminf(c.g, c.b));
    float mx = fmaxf(c.r, fmaxf(c.g, c.b));
    if (mx <= mn) {
        return (rgb){ .r = 0.0f, .g = 0.0f, .b = 0.0f };
    }
    float k = s / (mx - mn);
    return (rgb){ .r = (c.r - mn) * k, .g = (c.g - mn) * k, .b = (c.b - mn) * k };
}

static rgb blend_nonsep(compositor_blend_mode mode, rgb cb, rgb cs) {
    switch ((int)mode) {
        case COMPOSITOR_HUE:        return set_lum(set_sat(cs, sat(cb)), lum(cb));
        case COMPOSITOR_SATURATION: return set_lum(set_sat(cb, sat(cs)), lum(cb));
        case COMPOSITOR_COLOR:      return set_lum(cs, lum(cb));
        default:                    return set_lum(cb, lum(cs));  // luminosity
    }
}

// `src` is the premultiplied source pixel, already clip-attenuated and kept in
// float32 -- never rounded back to half on the way in (clip attenuation stays in
// float through the blend; see compositor_blend).
static cnvs_premul blend(blendf4 src, cnvs_premul dst, compositor_blend_mode mode) {
    float sa = src[3], da = (float)dst.a;
    float sr = src[0], sg = src[1], sb = src[2];
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
        float tr, tg, tb;
        if ((int)mode >= COMPOSITOR_HUE) {
            rgb cs = { .r = sa > 0.0f ? sr / sa : 0.0f,
                       .g = sa > 0.0f ? sg / sa : 0.0f,
                       .b = sa > 0.0f ? sb / sa : 0.0f };
            rgb cb = { .r = da > 0.0f ? dr / da : 0.0f,
                       .g = da > 0.0f ? dg / da : 0.0f,
                       .b = da > 0.0f ? db / da : 0.0f };
            rgb bl = blend_nonsep(mode, cb, cs);
            tr = sa * da * bl.r;
            tg = sa * da * bl.g;
            tb = sa * da * bl.b;
        } else {
            tr = blend_term(mode, sr, dr, sa, da);
            tg = blend_term(mode, sg, dg, sa, da);
            tb = blend_term(mode, sb, db, sa, da);
        }
        cor = sr * (1.0f - da) + dr * (1.0f - sa) + tr;
        cog = sg * (1.0f - da) + dg * (1.0f - sa) + tg;
        cob = sb * (1.0f - da) + db * (1.0f - sa) + tb;
        ao  = sa + da * (1.0f - sa);
    }

    float aoc = fminf(ao, 1.0f);  // additive 'lighter' can exceed 1
    return (cnvs_premul){ .r = clamp16(cor, aoc), .g = clamp16(cog, aoc),
                          .b = clamp16(cob, aoc), .a = (_Float16)aoc };
}

struct compositor {
    int width;
    int height;
    cnvs_premul *__counted_by(tn) target;  // premultiplied; all-zero == transparent
    int tn;                                // == width * height
    uint8_t *__counted_by(cn) clip;        // coverage 0..255, NULL = open
    int cn;                                // 0 when open
};

compositor *__single compositor_create(int width, int height) {
    if (width <= 0 || height <= 0) {
        return NULL;
    }
    compositor *__single c = calloc(1, sizeof *c);
    if (!c) {
        return NULL;
    }
    int const n = width * height;
    cnvs_premul *__counted_by_or_null(n) t = calloc((size_t)n, sizeof *t);
    if (!t) {
        free(c);
        return NULL;
    }
    c->width = width;
    c->height = height;
    c->tn = n;
    c->target = t;  // count before pointer
    return c;
}

void compositor_destroy(compositor *__single c) {
    if (!c) {
        return;
    }
    free(c->target);
    free(c->clip);
    free(c);
}

void compositor_set_clip(compositor *__single c,
                         uint8_t const *__counted_by(len) mask, int len) {
    if (!c) {
        return;
    }
    if (!mask) {
        free(c->clip);
        c->cn = 0;
        c->clip = NULL;
        return;
    }
    int const n = c->width * c->height;
    if (len < n) {
        return;
    }
    if (!c->clip) {
        uint8_t *__counted_by_or_null(n) m = malloc((size_t)n);
        if (!m) {
            return;
        }
        c->cn = n;
        c->clip = m;
    }
    memcpy(c->clip, mask, (size_t)n);
}

void compositor_blend(compositor *__single c, int x, int y, int w, int h,
                      cnvs_premul const *__counted_by(w * h) tile,
                      compositor_blend_mode mode) {
    if (!c || !tile || w <= 0 || h <= 0) {
        return;
    }
    if (x < 0 || y < 0 || x + w > c->width || y + h > c->height) {
        return;
    }
    if (mode == COMPOSITOR_SRC_OVER) {
        // The overwhelmingly common mode (every ordinary fill).  Its math --
        // co = s + (1-sa)*d, ao = sa + (1-sa)*da -- folds identically over all
        // four channels (lanes 3 and 7 yield ao), so blend two whole pixels as
        // one 8-lane _Float16 vector: load f16, blend f16, clamp f16, store
        // f16, no widen/narrow converts anywhere (docs/decisions/color-axis.md).
        // Clip attenuation is f16 too -- this deliberately reverses the
        // float32-attenuation choice the Metal-parity era kept (there is no
        // shader left to bit-match, and full coverage still attenuates by
        // exactly 1.0: 255 * RN16(1/255) rounds back to 1).  An odd-width
        // tail does one pixel at 4 lanes.
        _Float16 const k255 = (_Float16)(1.0f / 255.0f);
        for (int row = 0; row < h; row++) {
            int col = 0;
            for (; col + 2 <= w; col += 2) {
                int di = (y + row) * c->width + (x + col);
                blendh8 s, d;
                memcpy(&s, &tile[row * w + col], sizeof s);  // one check, two pixels
                memcpy(&d, &c->target[di], sizeof d);
                if (c->clip) {  // attenuate premultiplied source by clip coverage
                    _Float16 k0 = (_Float16)c->clip[di] * k255;
                    _Float16 k1 = (_Float16)c->clip[di + 1] * k255;
                    s = s * (blendh8){ k0, k0, k0, k0, k1, k1, k1, k1 };
                }
                blendh8 fb = (blendh8)(_Float16)1.0f -
                             __builtin_shufflevector(s, s, 3, 3, 3, 3, 7, 7, 7, 7);
                blendh8 co = s + fb * d;               // lanes 3,7 = ao
                blendh8 aoc = __builtin_elementwise_min(
                    __builtin_shufflevector(co, co, 3, 3, 3, 3, 7, 7, 7, 7),
                    (blendh8)(_Float16)1.0f);
                co = __builtin_elementwise_max((blendh8)(_Float16)0.0f,
                                               __builtin_elementwise_min(aoc, co));
                memcpy(&c->target[di], &co, sizeof co);
            }
            for (; col < w; col++) {  // odd-width tail: one pixel, 4 lanes
                int di = (y + row) * c->width + (x + col);
                blendh4 s, d;
                memcpy(&s, &tile[row * w + col], sizeof s);
                memcpy(&d, &c->target[di], sizeof d);
                if (c->clip) {
                    s = s * ((_Float16)c->clip[di] * k255);
                }
                _Float16 fb = (_Float16)1.0f - s[3];
                blendh4 co = s + fb * d;
                _Float16 aoc = co[3] < (_Float16)1.0f ? co[3] : (_Float16)1.0f;
                co = __builtin_elementwise_max((blendh4)(_Float16)0.0f,
                                               __builtin_elementwise_min((blendh4)aoc, co));
                memcpy(&c->target[di], &co, sizeof co);
            }
        }
        return;
    }
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int di = (y + row) * c->width + (x + col);
            blendh4 sh;
            memcpy(&sh, &tile[row * w + col], sizeof sh);
            blendf4 s = __builtin_convertvector(sh, blendf4);
            if (c->clip) {  // attenuate premultiplied source by clip coverage, in float
                float k = (float)c->clip[di] / 255.0f;
                s = s * k;
            }
            c->target[di] = blend(s, c->target[di], mode);
        }
    }
}

void compositor_read(compositor *__single c, cnvs_premul *__counted_by(len) out, int len) {
    if (!c || !out || len < c->width * c->height) {
        return;
    }
    memcpy(out, c->target, (size_t)(c->width * c->height) * sizeof *out);
}
