// Software backend for compositor.h: the target is a premultiplied cnvs_premul
// buffer, blend() runs the per-pixel blend over the tiles, read() copies it out.
// Built instead of compositor_metal.m for a GPU-free path (see configure.py).

#include "compositor.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// W3C composite + blend math, premultiplied throughout, mirroring
// shaders/compositor.metal.  Blend modes: co = s*(1-da) + d*(1-sa) + T,
// ao = sa + da*(1-sa), with premultiplied term T = sa*da*B(Cb,Cs); the polynomial
// modes have divide-free T, the non-linear ones (dodge/burn/soft-light, HSL)
// un-premultiply once.  Porter-Duff: co = Fa*s + Fb*d.  Switches run over
// (int)mode to avoid -Wswitch-enum.

// Metal's RGBA16Float store rounds toward zero (truncates) where C's (_Float16)
// cast rounds to nearest-even.  To make this software backend reproduce the GPU
// bit-for-bit (so `ninja`'s backenddiff gate holds at 0), match that: round every
// half store toward zero too.  This is deliberately *less* accurate than
// nearest-even (up to ~1 half-ULP low) -- the goal is parity with Metal, not
// numerical correctness.  It also couples this output to the GPU's store
// behaviour: a device that rounds-to-nearest would diverge (the differential
// would catch it).  See diff/ and docs.
static _Float16 to_half_rtz(float v) {  // v >= 0 here, so toward-zero == floor
    _Float16 n = (_Float16)v;
    if ((float)n > v) {  // nearest-even overshot -- step one ULP back toward zero
        uint16_t bits;
        memcpy(&bits, &n, sizeof bits);
        bits -= 1;
        memcpy(&n, &bits, sizeof bits);
    }
    return n;
}

static _Float16 clamp16(float v, float hi) {
    if (v < 0.0f) { v = 0.0f; }
    if (v > hi) { v = hi; }
    return to_half_rtz(v);
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

static cnvs_premul blend(cnvs_premul src, cnvs_premul dst, compositor_blend_mode mode) {
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
                          .b = clamp16(cob, aoc), .a = to_half_rtz(aoc) };
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

// One premultiplied pixel as a vector: cnvs_premul is four contiguous _Float16.
typedef _Float16 blendh4 __attribute__((ext_vector_type(4)));
typedef float blendf4 __attribute__((ext_vector_type(4)));
typedef int blendi4 __attribute__((ext_vector_type(4)));
typedef uint16_t blendu16x4 __attribute__((ext_vector_type(4)));

// Vector form of to_half_rtz (each lane v >= 0): round to nearest, then step the
// lanes that overshot one ULP back toward zero, so it bit-matches the scalar
// to_half_rtz the rest of the compositor uses (and thus Metal's RTZ store).
static blendh4 to_half_rtz4(blendf4 v) {
    blendh4 n = __builtin_convertvector(v, blendh4);
    blendf4 nf = __builtin_convertvector(n, blendf4);
    blendi4 over = nf > v;                                       // -1 where overshot
    blendu16x4 dec = __builtin_convertvector(over, blendu16x4);  // 0xFFFF where over
    blendu16x4 bits;
    memcpy(&bits, &n, sizeof bits);
    bits = bits + dec;                                           // += 0xFFFF == -= 1
    memcpy(&n, &bits, sizeof bits);
    return n;
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
        // co = s + (1-sa)*d, ao = sa + (1-sa)*da -- folds identically over all four
        // channels (lane 3 yields ao), so blend a whole pixel as one 4-lane vector.
        // Bit-identical to blend()'s source-over: the clip-attenuation round, the
        // clamp to [0,aoc], and the round-toward-zero half store are all reproduced.
        // contract(off): blend() compiles co = 1*s + fb*d as a rounded multiply then
        // add; without this the vector form fuses to one FMA and drifts a half-ULP,
        // which the exact (tolerance-0) backend differential would catch.
        #pragma clang fp contract(off)
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++) {
                int di = (y + row) * c->width + (x + col);
                blendh4 sh;
                memcpy(&sh, &tile[row * w + col], sizeof sh);
                blendf4 s = __builtin_convertvector(sh, blendf4);
                if (c->clip) {  // attenuate premultiplied source by clip coverage
                    float k = (float)c->clip[di] / 255.0f;
                    s = __builtin_convertvector(
                        __builtin_convertvector(s * k, blendh4), blendf4);  // round-to-nearest, as scalar
                }
                blendh4 dh;
                memcpy(&dh, &c->target[di], sizeof dh);
                blendf4 d = __builtin_convertvector(dh, blendf4);
                float fb = 1.0f - s[3];                // 1 - sa
                blendf4 co = s + fb * d;                // lane 3 = ao
                float aoc = co[3] < 1.0f ? co[3] : 1.0f;
                co = __builtin_elementwise_max((blendf4)0.0f,
                                               __builtin_elementwise_min((blendf4)aoc, co));
                blendh4 out = to_half_rtz4(co);
                memcpy(&c->target[di], &out, sizeof out);
            }
        }
        return;
    }
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int di = (y + row) * c->width + (x + col);
            cnvs_premul s = tile[row * w + col];
            if (c->clip) {  // attenuate premultiplied source by clip coverage
                float k = (float)c->clip[di] / 255.0f;
                s.r = (_Float16)((float)s.r * k);
                s.g = (_Float16)((float)s.g * k);
                s.b = (_Float16)((float)s.b * k);
                s.a = (_Float16)((float)s.a * k);
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

// No GPU here: the software compositor reports an empty profile.
void compositor_gpu_timing(compositor *__single c,
                           double *__single total_ns, long *__single dispatches) {
    (void)c;
    if (total_ns) {
        *total_ns = 0.0;
    }
    if (dispatches) {
        *dispatches = 0;
    }
}
