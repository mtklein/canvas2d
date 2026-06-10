// Software compositor backend for compositor.h: the target is a premultiplied
// cnvs_premul buffer, blend() runs the per-pixel blend over the tiles, read()
// copies it out.

#include "compositor.h"
#include "cnvs_planar.h"

#include <stdlib.h>
#include <string.h>

// W3C composite + blend math, premultiplied throughout, in _Float16 arithmetic
// end to end (docs/decisions/color-axis.md: f16 is the compute type, not just
// the storage type -- no widen/narrow converts anywhere).  Blend modes:
// co = s*(1-da) + d*(1-sa) + T, ao = sa + da*(1-sa), with premultiplied term
// T = sa*da*B(Cb,Cs); the polynomial modes have divide-free T, the non-linear ones
// (dodge/burn/soft-light, HSL) un-premultiply once.  Porter-Duff: co = Fa*s + Fb*d.
// Switches run over (int)mode to avoid -Wswitch-enum.

static _Float16 minh(_Float16 a, _Float16 b) { return a < b ? a : b; }
static _Float16 maxh(_Float16 a, _Float16 b) { return a > b ? a : b; }

// Separable blend B(cb, cs), unpremultiplied; only the non-linear modes need it.
static _Float16 blend_sep(compositor_blend_mode mode, _Float16 cb, _Float16 cs) {
    switch ((int)mode) {
        case COMPOSITOR_COLOR_DODGE:
            return cb <= (_Float16)0.0f ? (_Float16)0.0f
                 : cs >= (_Float16)1.0f ? (_Float16)1.0f
                 : minh((_Float16)1.0f, cb / ((_Float16)1.0f - cs));
        case COMPOSITOR_COLOR_BURN:
            return cb >= (_Float16)1.0f ? (_Float16)1.0f
                 : cs <= (_Float16)0.0f ? (_Float16)0.0f
                 : (_Float16)1.0f - minh((_Float16)1.0f, ((_Float16)1.0f - cb) / cs);
        case COMPOSITOR_SOFT_LIGHT: {
            _Float16 dd = cb <= (_Float16)0.25f
                ? (((_Float16)16.0f * cb - (_Float16)12.0f) * cb + (_Float16)4.0f) * cb
                : __builtin_sqrtf16(cb);
            return cs <= (_Float16)0.5f
                ? cb - ((_Float16)1.0f - (_Float16)2.0f * cs) * cb * ((_Float16)1.0f - cb)
                : cb + ((_Float16)2.0f * cs - (_Float16)1.0f) * (dd - cb);
        }
        default:
            return cs;
    }
}

// Premultiplied separable term T = sa*da*B for one channel (s, d premultiplied).
static _Float16 blend_term(compositor_blend_mode mode,
                           _Float16 s, _Float16 d, _Float16 sa, _Float16 da) {
    switch ((int)mode) {
        case COMPOSITOR_MULTIPLY:   return s * d;
        case COMPOSITOR_SCREEN:     return sa * d + da * s - s * d;
        case COMPOSITOR_OVERLAY:    return (_Float16)2.0f * d <= da
                                         ? (_Float16)2.0f * s * d
                                         : sa * da - (_Float16)2.0f * (da - d) * (sa - s);
        case COMPOSITOR_DARKEN:     return minh(s * da, d * sa);
        case COMPOSITOR_LIGHTEN:    return maxh(s * da, d * sa);
        case COMPOSITOR_HARD_LIGHT: return (_Float16)2.0f * s <= sa
                                         ? (_Float16)2.0f * s * d
                                         : sa * da - (_Float16)2.0f * (da - d) * (sa - s);
        case COMPOSITOR_DIFFERENCE: return __builtin_fabsf16(s * da - d * sa);
        case COMPOSITOR_EXCLUSION:  return sa * d + da * s - (_Float16)2.0f * s * d;
        default: {  // color-dodge / color-burn / soft-light
            _Float16 cs = sa > (_Float16)0.0f ? s / sa : (_Float16)0.0f;
            _Float16 cb = da > (_Float16)0.0f ? d / da : (_Float16)0.0f;
            return sa * da * blend_sep(mode, cb, cs);
        }
    }
}

// One premultiplied pixel as a vector: cnvs_premul is four contiguous _Float16.
typedef _Float16 blendh4 __attribute__((ext_vector_type(4)));

// Source-over for one planar block: co = s + (1-sa)*d, ao = sa + (1-sa)*da --
// the same fold over every channel plane, alpha included.  Takes and returns
// whole blocks in registers (cnvs_px8 is a four-vector HVA, q0-q3).
static cnvs_px8 src_over8(cnvs_px8 s, cnvs_px8 d) {
    cnvs_h8 fb = (cnvs_h8)(_Float16)1.0f - s.a;
    cnvs_px8 co = { s.r + fb * d.r, s.g + fb * d.g,
                    s.b + fb * d.b, s.a + fb * d.a };
    return cnvs_px8_clamp_premul(co);
}

typedef struct {
    _Float16 r, g, b;
} rgb;

static _Float16 lum(rgb c) {
    return (_Float16)0.3f * c.r + (_Float16)0.59f * c.g + (_Float16)0.11f * c.b;
}

static rgb clip_color(rgb c) {
    _Float16 l = lum(c);
    _Float16 n = minh(c.r, minh(c.g, c.b));
    _Float16 x = maxh(c.r, maxh(c.g, c.b));
    if (n < (_Float16)0.0f) {
        _Float16 k = l / (l - n);
        c.r = l + (c.r - l) * k;
        c.g = l + (c.g - l) * k;
        c.b = l + (c.b - l) * k;
    }
    if (x > (_Float16)1.0f) {
        _Float16 k = ((_Float16)1.0f - l) / (x - l);
        c.r = l + (c.r - l) * k;
        c.g = l + (c.g - l) * k;
        c.b = l + (c.b - l) * k;
    }
    return c;
}

static rgb set_lum(rgb c, _Float16 l) {
    _Float16 dl = l - lum(c);
    c.r += dl;
    c.g += dl;
    c.b += dl;
    return clip_color(c);
}

static _Float16 sat(rgb c) {
    return maxh(c.r, maxh(c.g, c.b)) - minh(c.r, minh(c.g, c.b));
}

// Set saturation: max channel -> s, min -> 0, mid proportional.
static rgb set_sat(rgb c, _Float16 s) {
    _Float16 mn = minh(c.r, minh(c.g, c.b));
    _Float16 mx = maxh(c.r, maxh(c.g, c.b));
    if (mx <= mn) {
        return (rgb){ .r = 0, .g = 0, .b = 0 };
    }
    _Float16 k = s / (mx - mn);
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

// `s` is the premultiplied source pixel, already clip-attenuated, as a 4-lane
// _Float16 vector; the whole kernel below stays in f16 arithmetic.  The
// composite fold runs over all four channels at once: in the Porter-Duff arm
// lane 3 of Fa*s + Fb*d is Fa*sa + Fb*da = ao, and in the blend arm lane 3 of
// s*(1-da) + d*(1-sa) + T is sa + da*(1-sa) = ao because T's alpha lane is
// pinned to sa*da.
static cnvs_premul blend(blendh4 s, cnvs_premul dst, compositor_blend_mode mode) {
    blendh4 d = { dst.r, dst.g, dst.b, dst.a };
    _Float16 sa = s[3], da = d[3];
    blendh4 co;

    if ((int)mode <= COMPOSITOR_COPY) {
        // Porter-Duff: co = Fa*s + Fb*d, ao = Fa*sa + Fb*da.
        _Float16 fa, fb;
        _Float16 const one = (_Float16)1.0f, zero = (_Float16)0.0f;
        switch ((int)mode) {
            case COMPOSITOR_SRC_IN:   fa = da;       fb = zero;      break;
            case COMPOSITOR_SRC_OUT:  fa = one - da; fb = zero;      break;
            case COMPOSITOR_SRC_ATOP: fa = da;       fb = one - sa;  break;
            case COMPOSITOR_DST_OVER: fa = one - da; fb = one;       break;
            case COMPOSITOR_DST_IN:   fa = zero;     fb = sa;        break;
            case COMPOSITOR_DST_OUT:  fa = zero;     fb = one - sa;  break;
            case COMPOSITOR_DST_ATOP: fa = one - da; fb = sa;        break;
            case COMPOSITOR_XOR:      fa = one - da; fb = one - sa;  break;
            case COMPOSITOR_LIGHTER:  fa = one;      fb = one;       break;
            case COMPOSITOR_COPY:     fa = one;      fb = zero;      break;
            default:                  fa = one;      fb = one - sa;  break;  // source-over
        }
        co = fa * s + fb * d;
    } else {
        blendh4 t;
        if ((int)mode >= COMPOSITOR_HUE) {
            rgb cs = { .r = sa > (_Float16)0.0f ? s[0] / sa : (_Float16)0.0f,
                       .g = sa > (_Float16)0.0f ? s[1] / sa : (_Float16)0.0f,
                       .b = sa > (_Float16)0.0f ? s[2] / sa : (_Float16)0.0f };
            rgb cb = { .r = da > (_Float16)0.0f ? d[0] / da : (_Float16)0.0f,
                       .g = da > (_Float16)0.0f ? d[1] / da : (_Float16)0.0f,
                       .b = da > (_Float16)0.0f ? d[2] / da : (_Float16)0.0f };
            rgb bl = blend_nonsep(mode, cb, cs);
            t = sa * da * (blendh4){ bl.r, bl.g, bl.b, (_Float16)1.0f };
        } else {
            t[0] = blend_term(mode, s[0], d[0], sa, da);
            t[1] = blend_term(mode, s[1], d[1], sa, da);
            t[2] = blend_term(mode, s[2], d[2], sa, da);
            t[3] = sa * da;
        }
        co = s * ((_Float16)1.0f - da) + d * ((_Float16)1.0f - sa) + t;
    }

    _Float16 aoc = minh(co[3], (_Float16)1.0f);  // additive 'lighter' can exceed 1
    co = __builtin_elementwise_max((blendh4)(_Float16)0.0f,
                                   __builtin_elementwise_min((blendh4)aoc, co));
    return (cnvs_premul){ .r = co[0], .g = co[1], .b = co[2], .a = co[3] };
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
        // The overwhelmingly common mode (every ordinary fill).  Eight pixels
        // per step as four channel planes (cnvs_planar.h): ld4 deinterleaves
        // at the tile seams, the blend is four fused multiply-adds with sa as
        // a plain vector -- no alpha-splat shuffles -- and everything stays in
        // _Float16 arithmetic end to end (docs/decisions/color-axis.md).
        // Clip attenuation is f16 too -- this deliberately reverses the
        // float32-attenuation choice the Metal-parity era kept (there is no
        // shader left to bit-match, and full coverage still attenuates by
        // exactly 1.0: 255 * RN16(1/255) rounds back to 1).  A w%8 tail runs
        // the same planar block over gathered pixels, zero-filled.
        _Float16 const k255 = (_Float16)(1.0f / 255.0f);
        for (int row = 0; row < h; row++) {
            int col = 0;
            for (; col + 8 <= w; col += 8) {
                int di = (y + row) * c->width + (x + col);
                cnvs_px8 s = cnvs_px8_load(tile + row * w + col);
                if (c->clip) {  // attenuate premultiplied source by clip coverage
                    s = cnvs_px8_scale(s, cnvs_h8_from_u8(c->clip + di) * k255);
                }
                cnvs_px8 d = cnvs_px8_load(c->target + di);
                cnvs_px8_store(c->target + di, src_over8(s, d));
            }
            if (col < w) {  // tail: k < 8 pixels through the same planar block
                int k = w - col;
                int di = (y + row) * c->width + (x + col);
                cnvs_px8 s = cnvs_px8_load_k(tile + row * w + col, k);
                if (c->clip) {
                    s = cnvs_px8_scale(s, cnvs_h8_from_u8_k(c->clip + di, k) * k255);
                }
                cnvs_px8 d = cnvs_px8_load_k(c->target + di, k);
                cnvs_px8_store_k(c->target + di, k, src_over8(s, d));
            }
        }
        return;
    }
    _Float16 const k255 = (_Float16)(1.0f / 255.0f);
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int di = (y + row) * c->width + (x + col);
            blendh4 s;
            memcpy(&s, &tile[row * w + col], sizeof s);
            if (c->clip) {  // attenuate premultiplied source by clip coverage
                s = s * ((_Float16)c->clip[di] * k255);
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
