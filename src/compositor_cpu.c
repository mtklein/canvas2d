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
//
// Everything below runs eight pixels at a time over channel planes
// (cnvs_planar.h); the scalar form's per-pixel branches are lane selects
// that compute both arms and keep the guarded one, so the divide/sqrt modes
// are straight-line vector code.  Selects are bitwise (cnvs_h8_sel): a
// guarded divide's inf/NaN lanes are discarded exactly, and every selected
// lane carries the arithmetic the scalar kernel produced, bit for bit
// (verified exhaustively against the scalar form, all 26 modes, random +
// edge-value sweeps -- the gallery byte-gate holds).
//
// Coverage (the op's AA plane x the clip mask) applies per the §3.8 ruling
// (docs/rasterization.md): in principle out = lerp(dst, blend(src, dst), cov)
// -- the uncovered fraction of a pixel keeps its destination.  Folding
// coverage into source alpha instead is identical math exactly when, in
// co = Fa*s + Fb*d, Fa is free of sa and Fb is affine in sa with Fb(0) = 1
// (compositor_coverage_folds): the over-family folds -- cheaper, and
// bit-compatible with the folded source-over pipeline -- and every other
// mode blends at full strength and lerps (test_coverage_lerp is the
// supersampled-oracle gate).

// minh/maxh: one fminnm/fmaxnm per plane.  These differ from the old
// compare+select spelling (`a < b ? a : b`) only on signed zeros and NaN
// ordering: every NaN reaching a min/max here sits in a lane a bitwise select
// later discards, and the premultiplied inputs are non-negative, so neither
// case survives to an output byte.  The select spelling existed to bit-match
// the scalar kernel the planar conversion replaced; that reference is gone.
static cnvs_h8 minh8(cnvs_h8 a, cnvs_h8 b) { return __builtin_elementwise_min(a, b); }
static cnvs_h8 maxh8(cnvs_h8 a, cnvs_h8 b) { return __builtin_elementwise_max(a, b); }

// Separable blend B(cb, cs), unpremultiplied; only the non-linear modes need it.
static cnvs_h8 blend_sep8(compositor_blend_mode mode, cnvs_h8 cb, cnvs_h8 cs) {
    cnvs_h8 const zero = (cnvs_h8)(_Float16)0.0f, one = (cnvs_h8)(_Float16)1.0f;
    switch ((int)mode) {
        case COMPOSITOR_COLOR_DODGE:
            return cnvs_h8_sel(cb <= zero, zero,
                   cnvs_h8_sel(cs >= one, one,
                               minh8(one, cb / (one - cs))));
        case COMPOSITOR_COLOR_BURN:
            return cnvs_h8_sel(cb >= one, one,
                   cnvs_h8_sel(cs <= zero, zero,
                               one - minh8(one, (one - cb) / cs)));
        case COMPOSITOR_SOFT_LIGHT: {
            cnvs_h8 dd = cnvs_h8_sel(cb <= (cnvs_h8)(_Float16)0.25f,
                (((_Float16)16.0f * cb - (_Float16)12.0f) * cb + (_Float16)4.0f) * cb,
                __builtin_elementwise_sqrt(cb));  // one fsqrt.8h, IEEE-exact
            return cnvs_h8_sel(cs <= (cnvs_h8)(_Float16)0.5f,
                cb - (one - (_Float16)2.0f * cs) * cb * (one - cb),
                cb + ((_Float16)2.0f * cs - one) * (dd - cb));
        }
        default:
            return cs;
    }
}

// Premultiplied separable term T = sa*da*B per channel plane (s, d premultiplied).
static cnvs_h8 blend_term8(compositor_blend_mode mode,
                           cnvs_h8 s, cnvs_h8 d, cnvs_h8 sa, cnvs_h8 da) {
    switch ((int)mode) {
        case COMPOSITOR_MULTIPLY:   return s * d;
        case COMPOSITOR_SCREEN:     return sa * d + da * s - s * d;
        case COMPOSITOR_OVERLAY:
            return cnvs_h8_sel((_Float16)2.0f * d <= da,
                               (_Float16)2.0f * s * d,
                               sa * da - (_Float16)2.0f * (da - d) * (sa - s));
        case COMPOSITOR_DARKEN:     return minh8(s * da, d * sa);
        case COMPOSITOR_LIGHTEN:    return maxh8(s * da, d * sa);
        case COMPOSITOR_HARD_LIGHT:
            return cnvs_h8_sel((_Float16)2.0f * s <= sa,
                               (_Float16)2.0f * s * d,
                               sa * da - (_Float16)2.0f * (da - d) * (sa - s));
        case COMPOSITOR_DIFFERENCE:
            return __builtin_elementwise_abs(s * da - d * sa);
        case COMPOSITOR_EXCLUSION:  return sa * d + da * s - (_Float16)2.0f * s * d;
        default: {  // color-dodge / color-burn / soft-light
            cnvs_h8 const zero = (cnvs_h8)(_Float16)0.0f;
            cnvs_h8 cs = cnvs_h8_sel(sa > zero, s / sa, zero);
            cnvs_h8 cb = cnvs_h8_sel(da > zero, d / da, zero);
            return sa * da * blend_sep8(mode, cb, cs);
        }
    }
}

// Source-over for one planar block: co = s + (1-sa)*d, ao = sa + (1-sa)*da --
// the same fold over every channel plane, alpha included.  Takes and returns
// whole blocks in registers (cnvs_px8 is a four-vector HVA, q0-q3).
static cnvs_px8 src_over8(cnvs_px8 s, cnvs_px8 d) {
    cnvs_h8 fb = (cnvs_h8)(_Float16)1.0f - s.a;
    cnvs_px8 co = { s.r + fb * d.r, s.g + fb * d.g,
                    s.b + fb * d.b, s.a + fb * d.a };
    return cnvs_px8_clamp_premul(co);
}

// Eight pixels' unpremultiplied colour as three channel planes.
typedef struct {
    cnvs_h8 r, g, b;
} rgb8;

static cnvs_h8 lum8(rgb8 c) {
    return (_Float16)0.3f * c.r + (_Float16)0.59f * c.g + (_Float16)0.11f * c.b;
}

static rgb8 clip_color8(rgb8 c) {
    cnvs_h8 const zero = (cnvs_h8)(_Float16)0.0f, one = (cnvs_h8)(_Float16)1.0f;
    cnvs_h8 l = lum8(c);
    cnvs_h8 n = minh8(c.r, minh8(c.g, c.b));
    cnvs_h8 x = maxh8(c.r, maxh8(c.g, c.b));
    cnvs_m8 lo = n < zero;  // lanes with a channel below 0: scale about l
    cnvs_h8 kn = l / (l - n);
    c.r = cnvs_h8_sel(lo, l + (c.r - l) * kn, c.r);
    c.g = cnvs_h8_sel(lo, l + (c.g - l) * kn, c.g);
    c.b = cnvs_h8_sel(lo, l + (c.b - l) * kn, c.b);
    cnvs_m8 hi = x > one;   // lanes with a channel above 1, on the updated c
    cnvs_h8 kx = (one - l) / (x - l);
    c.r = cnvs_h8_sel(hi, l + (c.r - l) * kx, c.r);
    c.g = cnvs_h8_sel(hi, l + (c.g - l) * kx, c.g);
    c.b = cnvs_h8_sel(hi, l + (c.b - l) * kx, c.b);
    return c;
}

static rgb8 set_lum8(rgb8 c, cnvs_h8 l) {
    cnvs_h8 dl = l - lum8(c);
    c.r += dl;
    c.g += dl;
    c.b += dl;
    return clip_color8(c);
}

static cnvs_h8 sat8(rgb8 c) {
    return maxh8(c.r, maxh8(c.g, c.b)) - minh8(c.r, minh8(c.g, c.b));
}

// Set saturation: max channel -> s, min -> 0, mid proportional; an all-equal
// lane (mx <= mn) has no saturation axis and goes to black.
static rgb8 set_sat8(rgb8 c, cnvs_h8 s) {
    cnvs_h8 const zero = (cnvs_h8)(_Float16)0.0f;
    cnvs_h8 mn = minh8(c.r, minh8(c.g, c.b));
    cnvs_h8 mx = maxh8(c.r, maxh8(c.g, c.b));
    cnvs_m8 flat = mx <= mn;
    cnvs_h8 k = s / (mx - mn);
    return (rgb8){ .r = cnvs_h8_sel(flat, zero, (c.r - mn) * k),
                   .g = cnvs_h8_sel(flat, zero, (c.g - mn) * k),
                   .b = cnvs_h8_sel(flat, zero, (c.b - mn) * k) };
}

static rgb8 blend_nonsep8(compositor_blend_mode mode, rgb8 cb, rgb8 cs) {
    switch ((int)mode) {
        case COMPOSITOR_HUE:        return set_lum8(set_sat8(cs, sat8(cb)), lum8(cb));
        case COMPOSITOR_SATURATION: return set_lum8(set_sat8(cb, sat8(cs)), lum8(cb));
        case COMPOSITOR_COLOR:      return set_lum8(cs, lum8(cb));
        default:                    return set_lum8(cb, lum8(cs));  // luminosity
    }
}

// `s` is one planar block of premultiplied source pixels, already
// clip-attenuated; the whole kernel stays in f16 arithmetic.  The composite
// fold runs the same expression over every plane: in the Porter-Duff arm the
// alpha plane of Fa*s + Fb*d is Fa*sa + Fb*da = ao, and in the blend arm the
// alpha plane of s*(1-da) + d*(1-sa) + T is sa + da*(1-sa) = ao because T's
// alpha plane is pinned to sa*da.
static cnvs_px8 blend8(cnvs_px8 s, cnvs_px8 d, compositor_blend_mode mode) {
    if (mode == COMPOSITOR_SRC_OVER) {
        // Delegate to the fast path's kernel: the Porter-Duff arm below would
        // spell source-over as fa*s + fb*d with fa = 1, and the contraction
        // shape differs -- fa*s rounds fb*d's product separately where
        // src_over8's s + fb*d fuses it -- so the explicit kernel keeps every
        // source-over bit-identical no matter which loop reached it.
        return src_over8(s, d);
    }
    cnvs_h8 const zero = (cnvs_h8)(_Float16)0.0f, one = (cnvs_h8)(_Float16)1.0f;
    cnvs_h8 sa = s.a, da = d.a;
    cnvs_px8 co;

    if ((int)mode <= COMPOSITOR_COPY) {
        // Porter-Duff: co = Fa*s + Fb*d, ao = Fa*sa + Fb*da.
        cnvs_h8 fa, fb;
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
        co.r = fa * s.r + fb * d.r;
        co.g = fa * s.g + fb * d.g;
        co.b = fa * s.b + fb * d.b;
        co.a = fa * sa + fb * da;
    } else {
        cnvs_px8 t;
        if ((int)mode >= COMPOSITOR_HUE) {
            cnvs_m8 sm = sa > zero, dm = da > zero;  // a == 0 un-premultiplies to 0
            rgb8 cs = { cnvs_h8_sel(sm, s.r / sa, zero),
                        cnvs_h8_sel(sm, s.g / sa, zero),
                        cnvs_h8_sel(sm, s.b / sa, zero) };
            rgb8 cb = { cnvs_h8_sel(dm, d.r / da, zero),
                        cnvs_h8_sel(dm, d.g / da, zero),
                        cnvs_h8_sel(dm, d.b / da, zero) };
            rgb8 bl = blend_nonsep8(mode, cb, cs);
            t.r = sa * da * bl.r;
            t.g = sa * da * bl.g;
            t.b = sa * da * bl.b;
            t.a = sa * da;
        } else {
            t.r = blend_term8(mode, s.r, d.r, sa, da);
            t.g = blend_term8(mode, s.g, d.g, sa, da);
            t.b = blend_term8(mode, s.b, d.b, sa, da);
            t.a = sa * da;
        }
        co.r = s.r * (one - da) + d.r * (one - sa) + t.r;
        co.g = s.g * (one - da) + d.g * (one - sa) + t.g;
        co.b = s.b * (one - da) + d.b * (one - sa) + t.b;
        co.a = sa * (one - da) + da * (one - sa) + t.a;
    }
    return cnvs_px8_clamp_premul(co);  // additive 'lighter' can exceed 1
}

// The coverage lerp: out = blend*k + dst*(1-k) per plane.  Two products, not
// dst + (blend-dst)*k, so k == 1 returns the blend bit-exactly and k == 0
// returns dst bit-exactly (full coverage must not perturb the blend; zero
// coverage must not perturb the destination).  The clamp restores the
// premultiplied invariant against the one-ULP drift of k + (1-k) in f16.
static cnvs_px8 cov_lerp8(cnvs_px8 d, cnvs_px8 co, cnvs_h8 k) {
    cnvs_h8 j = (cnvs_h8)(_Float16)1.0f - k;
    cnvs_px8 o = { co.r * k + d.r * j, co.g * k + d.g * j,
                   co.b * k + d.b * j, co.a * k + d.a * j };
    return cnvs_px8_clamp_premul(o);
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

// The generic modes, shared by the tile and solid-colour entry points: the
// same planar block walk as the source-over fast path, with the 26-mode
// kernel in place of the source-over fold.  `tile` may be NULL, in which case
// every block's source is `splat` -- one solid colour broadcast across the
// lanes, standing in for the constant tile the canvas used to write out and
// this loop used to read back (a ~16 B/px round trip carrying no
// information).  Effective coverage is the op plane x the clip mask (each
// absent factor is 1); the over-family folds it into the source exactly as
// the fast path does, every other mode blends at full strength and lerps
// toward the destination (cov_lerp8) -- the §3.8 ruling.
static void blend_region(compositor *__single c, int x, int y, int w, int h,
                         cnvs_premul const *__counted_by_or_null(w * h) tile,
                         cnvs_px8 splat,
                         uint8_t const *__counted_by_or_null(w * h) cov,
                         compositor_blend_mode mode) {
    bool const folds = compositor_coverage_folds(mode);
    bool const atten = cov || c->clip;  // any coverage to apply?
    _Float16 const k255 = (_Float16)(1.0f / 255.0f);
    cnvs_h8 const one = (cnvs_h8)(_Float16)1.0f;
    for (int row = 0; row < h; row++) {
        int col = 0;
        for (; col + 8 <= w; col += 8) {
            int ti = row * w + col;
            int di = (y + row) * c->width + (x + col);
            cnvs_px8 s = tile ? cnvs_px8_load(tile + ti) : splat;
            cnvs_px8 d = cnvs_px8_load(c->target + di);
            cnvs_px8 o;
            if (!atten) {
                o = blend8(s, d, mode);
            } else if (folds) {
                // Fold: attenuate the source by each factor in turn -- the
                // source-over fast path's exact arithmetic (scale by cov,
                // then scale by clip; combining the factors first would
                // re-round).
                if (cov) {
                    s = cnvs_px8_scale(s, cnvs_h8_from_u8(cov + ti) * k255);
                }
                if (c->clip) {
                    s = cnvs_px8_scale(s, cnvs_h8_from_u8(c->clip + di) * k255);
                }
                o = blend8(s, d, mode);
            } else {
                cnvs_h8 k = one;  // 1*x is exact: a lone factor passes through
                if (cov) {
                    k = k * (cnvs_h8_from_u8(cov + ti) * k255);
                }
                if (c->clip) {
                    k = k * (cnvs_h8_from_u8(c->clip + di) * k255);
                }
                o = cov_lerp8(d, blend8(s, d, mode), k);
            }
            cnvs_px8_store(c->target + di, o);
        }
        if (col < w) {  // tail: k < 8 pixels through the same planar block
            int n = w - col;
            int ti = row * w + col;
            int di = (y + row) * c->width + (x + col);
            cnvs_px8 s = tile ? cnvs_px8_load_k(tile + ti, n) : splat;
            cnvs_px8 d = cnvs_px8_load_k(c->target + di, n);
            cnvs_px8 o;
            if (!atten) {
                o = blend8(s, d, mode);
            } else if (folds) {
                if (cov) {
                    s = cnvs_px8_scale(s, cnvs_h8_from_u8_k(cov + ti, n) * k255);
                }
                if (c->clip) {
                    s = cnvs_px8_scale(s, cnvs_h8_from_u8_k(c->clip + di, n) * k255);
                }
                o = blend8(s, d, mode);
            } else {
                cnvs_h8 k = one;
                if (cov) {
                    k = k * (cnvs_h8_from_u8_k(cov + ti, n) * k255);
                }
                if (c->clip) {
                    k = k * (cnvs_h8_from_u8_k(c->clip + di, n) * k255);
                }
                o = cov_lerp8(d, blend8(s, d, mode), k);
            }
            cnvs_px8_store_k(c->target + di, n, o);
        }
    }
}

void compositor_blend(compositor *__single c, int x, int y, int w, int h,
                      cnvs_premul const *__counted_by(w * h) tile,
                      uint8_t const *__counted_by_or_null(w * h) cov,
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
        // Source-over folds: op coverage (normally already folded by the
        // shade stage, so cov is NULL here) and clip attenuation both scale
        // the premultiplied source, in f16 -- this deliberately reverses the
        // float32-attenuation choice the Metal-parity era kept (there is no
        // shader left to bit-match, and full coverage still attenuates by
        // exactly 1.0: 255 * RN16(1/255) rounds back to 1).  A w%8 tail runs
        // the same planar block over gathered pixels, zero-filled.
        _Float16 const k255 = (_Float16)(1.0f / 255.0f);
        for (int row = 0; row < h; row++) {
            int col = 0;
            for (; col + 8 <= w; col += 8) {
                int ti = row * w + col;
                int di = (y + row) * c->width + (x + col);
                cnvs_px8 s = cnvs_px8_load(tile + ti);
                if (cov) {      // fold op coverage into the source (exact here)
                    s = cnvs_px8_scale(s, cnvs_h8_from_u8(cov + ti) * k255);
                }
                if (c->clip) {  // attenuate premultiplied source by clip coverage
                    s = cnvs_px8_scale(s, cnvs_h8_from_u8(c->clip + di) * k255);
                }
                cnvs_px8 d = cnvs_px8_load(c->target + di);
                cnvs_px8_store(c->target + di, src_over8(s, d));
            }
            if (col < w) {  // tail: k < 8 pixels through the same planar block
                int k = w - col;
                int ti = row * w + col;
                int di = (y + row) * c->width + (x + col);
                cnvs_px8 s = cnvs_px8_load_k(tile + ti, k);
                if (cov) {
                    s = cnvs_px8_scale(s, cnvs_h8_from_u8_k(cov + ti, k) * k255);
                }
                if (c->clip) {
                    s = cnvs_px8_scale(s, cnvs_h8_from_u8_k(c->clip + di, k) * k255);
                }
                cnvs_px8 d = cnvs_px8_load_k(c->target + di, k);
                cnvs_px8_store_k(c->target + di, k, src_over8(s, d));
            }
        }
        return;
    }
    // The generic modes: the shared region walk (splat unused, tile present).
    cnvs_px8 const zero = { (cnvs_h8)(_Float16)0.0f, (cnvs_h8)(_Float16)0.0f,
                            (cnvs_h8)(_Float16)0.0f, (cnvs_h8)(_Float16)0.0f };
    blend_region(c, x, y, w, h, tile, zero, cov, mode);
}

void compositor_blend_solid(compositor *__single c, int x, int y, int w, int h,
                            cnvs_premul color,
                            uint8_t const *__counted_by_or_null(w * h) cov,
                            compositor_blend_mode mode) {
    if (!c || w <= 0 || h <= 0) {
        return;
    }
    if (x < 0 || y < 0 || x + w > c->width || y + h > c->height) {
        return;
    }
    // Broadcast the colour across the lanes once; the region walk's source is
    // this block for every step, bit-for-bit the tile of identical pixels the
    // caller used to materialize (a splat lane equals the stored-and-reloaded
    // f16 exactly).  SRC_OVER takes the region walk here, not the fast path,
    // and still lands the same bytes: blend8 delegates source-over to
    // src_over8 and the walk's fold arm applies cov/clip as the same two
    // successive scales (test_compositor's solid_vs_tile sweep pins all 26
    // modes x coverage shapes byte-for-byte).
    cnvs_px8 const splat = { (cnvs_h8)color.r, (cnvs_h8)color.g,
                             (cnvs_h8)color.b, (cnvs_h8)color.a };
    blend_region(c, x, y, w, h, NULL, splat, cov, mode);
}

void compositor_read(compositor *__single c, cnvs_premul *__counted_by(len) out, int len) {
    if (!c || !out || len < c->width * c->height) {
        return;
    }
    memcpy(out, c->target, (size_t)(c->width * c->height) * sizeof *out);
}
