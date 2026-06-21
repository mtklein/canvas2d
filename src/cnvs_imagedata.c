#include "canvas.h"

#include "cnvs_blend.h"
#include "cnvs_canvas.h"
#include "cnvs_color.h"
#include "cnvs_image.h"
#include "cnvs_math.h"
#include "cnvs_planar.h"
#include "cnvs_record.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// One planar slab's un-premultiply and 8-bit quantize, in _Float16: the
// divide, clamp, and 255-scale with no f32 anywhere (docs/decisions/
// color-axis.md).  A fully transparent lane (a <= 0) un-premultiplies to all
// zero -- selected bitwise BEFORE the byte convert, so the masked divide's
// inf/NaN lanes never reach the (undefined for them) float->int conversion.
// Every 8-bit edge value still quantizes back exactly (test_image's
// exhaustive round-trip).  Returns finished byte values in [0.5, 255.5) for
// the truncating store seam (cnvs_px8_store_rgba8).
static cnvs_px8 unpremul_to_unorm8(cnvs_px8 p) {
    half8 const zero = (half8)(_Float16)0.0f, one = (half8)(_Float16)1.0f;
    short8 const opaque = p.a > zero;
    cnvs_px8 u = { p.r / p.a, p.g / p.a, p.b / p.a, p.a };
    u.r = half8_if_then_else(opaque, __builtin_elementwise_min(one,
                        __builtin_elementwise_max(zero, u.r)), zero);
    u.g = half8_if_then_else(opaque, __builtin_elementwise_min(one,
                        __builtin_elementwise_max(zero, u.g)), zero);
    u.b = half8_if_then_else(opaque, __builtin_elementwise_min(one,
                        __builtin_elementwise_max(zero, u.b)), zero);
    u.a = half8_if_then_else(opaque, __builtin_elementwise_min(one,
                        __builtin_elementwise_max(zero, u.a)), zero);
    _Float16 const bias = (_Float16)0.5f, k255 = (_Float16)255.0f;
    return (cnvs_px8){ u.r * k255 + bias, u.g * k255 + bias,
                       u.b * k255 + bias, u.a * k255 + bias };
}

// The LINEAR canvas readback: the same un-premultiply and 8-bit quantize, but
// with a linear->sRGB encode (cnvs_linear_to_srgb) inserted between the divide
// and the clamp -- the one and only clamp in the linear pipeline lives in this
// exit, and the transfer must run BEFORE it so an extended linear value collapses
// to its encoded byte rather than being crushed to 1.0 first.  Scalar per lane:
// the encode is a precision-sensitive f32 pow (cnvs_color.c's deferral note),
// with no half8 spelling in libm; the readback hot path is the sRGB-canvas
// SIMD function above, never this.  Alpha takes no transfer (coverage, not
// colour).  An out-of-[0,1] alpha still clamps, like the planar path.
static cnvs_px8 unpremul_encode_to_unorm8(cnvs_px8 p) {
    cnvs_px8 o = { (half8)(_Float16)0.0f, (half8)(_Float16)0.0f,
                   (half8)(_Float16)0.0f, (half8)(_Float16)0.0f };
    for (int i = 0; i < 8; i++) {
        float const a = (float)p.a[i];
        float r = 0.0f, g = 0.0f, b = 0.0f, ca = 0.0f;
        if (a > 0.0f) {  // a transparent lane stays all-zero (the planar mask's twin)
            float const inv = 1.0f / a;
            r  = cnvs_clamp01(cnvs_linear_to_srgb((float)p.r[i] * inv));
            g  = cnvs_clamp01(cnvs_linear_to_srgb((float)p.g[i] * inv));
            b  = cnvs_clamp01(cnvs_linear_to_srgb((float)p.b[i] * inv));
            ca = cnvs_clamp01(a);
        }
        o.r[i] = (_Float16)(r  * 255.0f + 0.5f);
        o.g[i] = (_Float16)(g  * 255.0f + 0.5f);
        o.b[i] = (_Float16)(b  * 255.0f + 0.5f);
        o.a[i] = (_Float16)(ca * 255.0f + 0.5f);
    }
    return o;
}

// One Oklab byte-channel convention, shared by the OKLAB readback below and the
// OKLAB putImageData input (px8_oklab_to_linear): L rides [0,1] straight onto a
// byte; the signed a,b ride a centred [-0.5, 0.5] window (byte = (v+0.5)*255,
// the inverse v = byte/255 - 0.5).  In-window components round-trip to 8-bit
// tolerance; out-of-window components saturate at the byte ends -- exotic, but
// uniform and self-inverse, which is what a consistent transport demands.
#define CNVS_OKLAB_AB_BIAS 0.5f

// The OKLAB readback: un-premultiply, take the unpremultiplied colour
// working->linear->Oklab, and quantize (L, a, b, alpha) to bytes via the
// convention above.  Scalar per lane (the Oklab pipeline is f32 transcendental
// math, no half8 spelling; correctness over speed, like the linear encode).  A
// transparent lane stays all-zero, and alpha takes no transfer (coverage, not
// colour) -- both as in the other readbacks.  `linear_canvas` says the STORED
// colour is already linear (skip the sRGB decode); otherwise the stored colour
// is encoded sRGB and decodes first.
static cnvs_px8 unpremul_to_oklab_unorm8(cnvs_px8 p, bool linear_canvas) {
    cnvs_px8 o = { (half8)(_Float16)0.0f, (half8)(_Float16)0.0f,
                   (half8)(_Float16)0.0f, (half8)(_Float16)0.0f };
    for (int i = 0; i < 8; i++) {
        float const a = (float)p.a[i];
        float qL = 0.0f, qa = 0.0f, qb = 0.0f, ca = 0.0f;
        if (a > 0.0f) {  // a transparent lane stays all-zero
            float const inv = 1.0f / a;
            cnvs_rgb lin = { .r = (float)p.r[i] * inv,
                             .g = (float)p.g[i] * inv,
                             .b = (float)p.b[i] * inv };
            if (!linear_canvas) {  // stored encoded sRGB -> linear first
                lin = cnvs_rgb_srgb_to_linear(lin);
            }
            cnvs_oklab const lab = cnvs_linear_srgb_to_oklab(lin);
            qL = cnvs_clamp01(lab.L);
            qa = cnvs_clamp01(lab.a + CNVS_OKLAB_AB_BIAS);
            qb = cnvs_clamp01(lab.b + CNVS_OKLAB_AB_BIAS);
            ca = cnvs_clamp01(a);
        }
        o.r[i] = (_Float16)(qL * 255.0f + 0.5f);
        o.g[i] = (_Float16)(qa * 255.0f + 0.5f);
        o.b[i] = (_Float16)(qb * 255.0f + 0.5f);
        o.a[i] = (_Float16)(ca * 255.0f + 0.5f);
    }
    return o;
}

// The LINEAR_SRGB readback: emit the unpremultiplied colour as linear-sRGB bytes
// (no encode).  On a LINEAR canvas the stored colour already IS linear, so this
// is just un-premultiply + clamp + quantize (no transfer at all).  On an sRGB
// canvas the stored colour is encoded sRGB and decodes sRGB->linear first.  Both
// clamp into [0,1] for the byte range (an extended linear value saturates here;
// the byte transport has no room for out-of-gamut).  Scalar per lane to share the
// decode with the sRGB-canvas branch; alpha takes no transfer.
static cnvs_px8 unpremul_to_linear_unorm8(cnvs_px8 p, bool linear_canvas) {
    cnvs_px8 o = { (half8)(_Float16)0.0f, (half8)(_Float16)0.0f,
                   (half8)(_Float16)0.0f, (half8)(_Float16)0.0f };
    for (int i = 0; i < 8; i++) {
        float const a = (float)p.a[i];
        float r = 0.0f, g = 0.0f, b = 0.0f, ca = 0.0f;
        if (a > 0.0f) {  // a transparent lane stays all-zero
            float const inv = 1.0f / a;
            r = (float)p.r[i] * inv;
            g = (float)p.g[i] * inv;
            b = (float)p.b[i] * inv;
            if (!linear_canvas) {  // stored encoded sRGB -> linear
                r = cnvs_srgb_to_linear(r);
                g = cnvs_srgb_to_linear(g);
                b = cnvs_srgb_to_linear(b);
            }
            r  = cnvs_clamp01(r);
            g  = cnvs_clamp01(g);
            b  = cnvs_clamp01(b);
            ca = cnvs_clamp01(a);
        }
        o.r[i] = (_Float16)(r  * 255.0f + 0.5f);
        o.g[i] = (_Float16)(g  * 255.0f + 0.5f);
        o.b[i] = (_Float16)(b  * 255.0f + 0.5f);
        o.a[i] = (_Float16)(ca * 255.0f + 0.5f);
    }
    return o;
}

// Per-canvas, per-output-space readback.  CANVAS_CS_SRGB off an sRGB canvas is a
// direct SIMD bypass (no transfer; the stored encoded bytes quantize as-is);
// off a linear canvas it encodes linear->sRGB scalar.  Either way it is NOT a
// linear round-trip.  The LINEAR_SRGB and OKLAB branches run scalar too -- the
// transfer/Oklab math is transcendental and has no half8 spelling.
static cnvs_px8 read_unorm8(struct canvas *__single cv,
                            enum canvas_color_space space, cnvs_px8 p) {
    bool const lin = cv->space == CANVAS_CS_LINEAR_SRGB;
    if (space == CANVAS_CS_LINEAR_SRGB) {
        return unpremul_to_linear_unorm8(p, lin);
    }
    if (space == CANVAS_CS_OKLAB) {
        return unpremul_to_oklab_unorm8(p, lin);
    }
    // CANVAS_CS_SRGB (and any out-of-range value): never a decode/encode round
    // trip.
    return lin ? unpremul_encode_to_unorm8(p) : unpremul_to_unorm8(p);
}

// Read the canvas back as unpremultiplied RGBA8 in the requested OUTPUT colour
// space, straight off the premultiplied target: the un-premultiply and 8-bit
// quantize happen here, eight pixels per step over channel planes with st4
// re-interleaving at the RGBA8 seam (cnvs_planar.h); the n%8 tail runs the same
// slab gathered.  `space` names the space the OUTPUT bytes are encoded in:
//   CANVAS_CS_SRGB        -- encoded sRGB (an sRGB canvas is the SIMD bypass, a
//                            linear canvas encodes linear->sRGB).  NOT a linear
//                            round trip.
//   CANVAS_CS_LINEAR_SRGB -- linear-sRGB bytes (linear canvas: the stored values
//                            quantized directly; sRGB canvas: decode then quantize).
//   CANVAS_CS_OKLAB       -- Oklab (L,a,b) bytes (working->linear->Oklab).
// get_image_data reads back through this same entry point.  write_png does NOT
// route here: it goes through canvas_encode_png -> surface_to_pq16 (16-bit
// Rec.2020/PQ), a separate output pipeline.
void canvas_read_rgba(struct canvas *__single cv, enum canvas_color_space space,
                      uint8_t *__counted_by(len) out, int len) {
    if (len < cv->width * cv->height * 4) {
        return;
    }
    int const n = cv->target_len;
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        cnvs_px8_store_rgba8(out + i * 4,
                             read_unorm8(cv, space, cnvs_px8_load(cv->target + i)));
    }
    if (i < n) {
        int const k = n - i;
        cnvs_px8_store_rgba8_k(out + i * 4, k,
                               read_unorm8(cv, space, cnvs_px8_load_k(cv->target + i, k)));
    }
}

// --- straight f16 readback (rgba-float16 ImageData) -------------------------
//
// The f16 twins of the unorm8 readbacks above: un-premultiply and convert the
// working colour into the OUTPUT space `space`, but emit straight _Float16 with
// NO clamp to [0,1] and NO 8-bit quantize.  This is the extended-range path --
// HDR (>1) and wide-gamut (negative) colour a LINEAR canvas stores survive the
// round trip, where the byte transports collapse them.  A fully transparent
// lane (a <= 0) un-premultiplies to transparent black (all-zero), as in the
// byte path and as the spec wants.  Alpha takes no transfer (coverage, not
// colour); it is clamped into [0,1] (the surface-alpha invariant), the only
// clamp in these functions.

// SRGB output: a pure un-premultiply.  On an sRGB canvas the stored colour is
// already encoded sRGB, so this hands back the encoded sRGB colour straight;
// on a linear canvas the stored colour is linear, encoded sRGB->... is the
// caller's choice of space, so SRGB here is the linear value encoded to sRGB.
// Eight pixels per slab (no transfer on an sRGB canvas, an encode per lane on a
// linear one).
static cnvs_px8 unpremul_to_srgb_f16(cnvs_px8 p, bool linear_canvas) {
    half8 const zero = (half8)(_Float16)0.0f, one = (half8)(_Float16)1.0f;
    short8 const opaque = p.a > zero;
    half8 const ca = __builtin_elementwise_min(one,
                         __builtin_elementwise_max(zero, p.a));
    cnvs_px8 u = { p.r / p.a, p.g / p.a, p.b / p.a, ca };
    u.r = half8_if_then_else(opaque, u.r, zero);
    u.g = half8_if_then_else(opaque, u.g, zero);
    u.b = half8_if_then_else(opaque, u.b, zero);
    u.a = half8_if_then_else(opaque, ca,  zero);
    if (!linear_canvas) {
        return u;  // stored colour already encoded sRGB: hand it back straight
    }
    for (int i = 0; i < 8; i++) {  // linear canvas: encode the linear colour to sRGB
        u.r[i] = (_Float16)cnvs_linear_to_srgb((float)u.r[i]);
        u.g[i] = (_Float16)cnvs_linear_to_srgb((float)u.g[i]);
        u.b[i] = (_Float16)cnvs_linear_to_srgb((float)u.b[i]);
    }
    return u;
}

// LINEAR_SRGB output: the un-premultiplied colour as linear sRGB, NO clamp.  On
// a linear canvas the stored colour already IS linear, so this is a bare
// un-premultiply that carries extended values through verbatim.  On an sRGB
// canvas the stored colour is encoded sRGB and decodes sRGB->linear first.
// Scalar per lane to share the decode with the sRGB-canvas branch.
static cnvs_px8 unpremul_to_linear_f16(cnvs_px8 p, bool linear_canvas) {
    cnvs_px8 o = { (half8)(_Float16)0.0f, (half8)(_Float16)0.0f,
                   (half8)(_Float16)0.0f, (half8)(_Float16)0.0f };
    for (int i = 0; i < 8; i++) {
        float const a = (float)p.a[i];
        if (a > 0.0f) {  // a transparent lane stays all-zero
            float const inv = 1.0f / a;
            float r = (float)p.r[i] * inv;
            float g = (float)p.g[i] * inv;
            float b = (float)p.b[i] * inv;
            if (!linear_canvas) {  // stored encoded sRGB -> linear
                r = cnvs_srgb_to_linear(r);
                g = cnvs_srgb_to_linear(g);
                b = cnvs_srgb_to_linear(b);
            }
            o.r[i] = (_Float16)r;
            o.g[i] = (_Float16)g;
            o.b[i] = (_Float16)b;
            o.a[i] = (_Float16)cnvs_clamp01(a);
        }
    }
    return o;
}

// OKLAB output: un-premultiply, working->linear->Oklab, emit (L, a, b, alpha)
// straight (NO bias, NO clamp -- the byte convention's window is a transport
// detail the f16 path doesn't need).  Scalar per lane (the Oklab pipeline is
// f32 transcendental, no half8 spelling).
static cnvs_px8 unpremul_to_oklab_f16(cnvs_px8 p, bool linear_canvas) {
    cnvs_px8 o = { (half8)(_Float16)0.0f, (half8)(_Float16)0.0f,
                   (half8)(_Float16)0.0f, (half8)(_Float16)0.0f };
    for (int i = 0; i < 8; i++) {
        float const a = (float)p.a[i];
        if (a > 0.0f) {  // a transparent lane stays all-zero
            float const inv = 1.0f / a;
            cnvs_rgb lin = { .r = (float)p.r[i] * inv,
                             .g = (float)p.g[i] * inv,
                             .b = (float)p.b[i] * inv };
            if (!linear_canvas) {  // stored encoded sRGB -> linear first
                lin = cnvs_rgb_srgb_to_linear(lin);
            }
            cnvs_oklab const lab = cnvs_linear_srgb_to_oklab(lin);
            o.r[i] = (_Float16)lab.L;
            o.g[i] = (_Float16)lab.a;
            o.b[i] = (_Float16)lab.b;
            o.a[i] = (_Float16)cnvs_clamp01(a);
        }
    }
    return o;
}

// Per-canvas, per-output-space straight f16 readback (the f16 twin of
// read_unorm8): route to the matching space branch above.
static cnvs_px8 read_f16(struct canvas *__single cv,
                         enum canvas_color_space space, cnvs_px8 p) {
    bool const lin = cv->space == CANVAS_CS_LINEAR_SRGB;
    if (space == CANVAS_CS_LINEAR_SRGB) {
        return unpremul_to_linear_f16(p, lin);
    }
    if (space == CANVAS_CS_OKLAB) {
        return unpremul_to_oklab_f16(p, lin);
    }
    return unpremul_to_srgb_f16(p, lin);  // CANVAS_CS_SRGB (and any out-of-range)
}

// canvas_read_rgba's f16 twin: read the whole canvas back as straight
// (unpremultiplied) _Float16 RGBA into `out` (cnvs_unpremul-shaped, w*h pixels)
// in the requested OUTPUT space, extended range preserved.  Eight pixels per
// slab with st4 re-interleave at the unpremul seam (cnvs_planar.h); the n%8 tail
// runs the same slab gathered.  Internal: get_image_data_f16 reads through here.
static void canvas_read_rgba_f16(struct canvas *__single cv,
                                 enum canvas_color_space space,
                                 cnvs_unpremul *__counted_by(npx) out, int npx) {
    if (npx < cv->width * cv->height) {
        return;
    }
    int const n = cv->target_len;
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        cnvs_px8_store_unpremul(out + i, read_f16(cv, space, cnvs_px8_load(cv->target + i)));
    }
    if (i < n) {
        int const k = n - i;
        cnvs_px8_store_unpremul_k(out + i, k,
                                  read_f16(cv, space, cnvs_px8_load_k(cv->target + i, k)));
    }
}

void canvas_get_image_data(struct canvas *__single cv,
                           enum canvas_color_space space,
                           int x, int y, int w, int h,
                           uint8_t *__counted_by(len) out, int len) {
    if (!cnvs_rgba8_dims_ok(w, h) || len < w * h * 4) {
        return;
    }
    memset(out, 0, (size_t)len);  // pixels outside the canvas stay transparent
    int const clen = cv->width * cv->height * 4;
    uint8_t *__counted_by_or_null(clen) buf = malloc((size_t)clen);
    if (!buf) {
        return;
    }
    canvas_read_rgba(cv, space, buf, clen);  // reads back in the requested space
    cnvs_blit_rgba(out, w, h, 0, 0, buf, cv->width, cv->height, x, y, w, h);
    free(buf);
}

uint8_t *__counted_by_or_null(*len)
canvas_create_image_data(int sw, int sh, int *__single len) {
    if (!cnvs_rgba8_dims_ok(sw, sh)) {
        *len = 0;
        return NULL;
    }
    // cnvs_rgba8_dims_ok guarantees sw*sh*4 fits a positive int, so this is overflow-free.
    int const n = sw * sh * 4;
    uint8_t *buf = calloc((size_t)n, 1);  // zeroed == transparent black
    if (!buf) {
        *len = 0;
        return NULL;
    }
    *len = n;
    return buf;
}

// canvas_get_image_data's f16 twin (rgba-float16 ImageData): read the w*h
// sub-image at (x, y) back as straight _Float16 RGBA in the OUTPUT space `space`,
// extended range preserved (no clamp01, no 8-bit quantize -- the whole point of
// the f16 path).  `len` is the ELEMENT count (w*h*4), like the u8 `len` minus the
// byte conversion.  Pixels outside the canvas read back transparent black.  Bad
// dims or too-small len: no-op.
void canvas_get_image_data_f16(struct canvas *__single cv,
                               enum canvas_color_space space,
                               int x, int y, int w, int h,
                               _Float16 *__counted_by(len) out, int len) {
    if (!cnvs_rgba8_dims_ok(w, h) || len < w * h * 4) {
        return;
    }
    memset(out, 0, (size_t)len * sizeof *out);  // outside the canvas stays transparent
    int const cpx = cv->width * cv->height;     // canvas pixel count
    cnvs_unpremul *__counted_by_or_null(cpx) buf = malloc((size_t)cpx * sizeof *buf);
    if (!buf) {
        return;
    }
    canvas_read_rgba_f16(cv, space, buf, cpx);  // reads back in the requested space
    // cnvs_unpremul is four contiguous _Float16, so buf is a w*h*4-element
    // _Float16 image -- the layout cnvs_blit_f16 copies, four components/pixel.
    cnvs_blit_f16(out, w, h, 0, 0, (_Float16 const *)buf, cv->width, cv->height,
                  x, y, w, h);
    free(buf);
}

// canvas_create_image_data's f16 twin: a blank (transparent black) rgba-float16
// image of sw*sh pixels.  *len is the ELEMENT count (sw*sh*4); NULL + *len=0 on
// non-positive dims, a size that would overflow, or OOM.
_Float16 *__counted_by_or_null(*len)
canvas_create_image_data_f16(int sw, int sh, int *__single len) {
    if (!cnvs_rgba8_dims_ok(sw, sh)) {
        *len = 0;
        return NULL;
    }
    // cnvs_rgba8_dims_ok guarantees sw*sh*4 fits a positive int, so this is overflow-free.
    int const n = sw * sh * 4;
    _Float16 *buf = calloc((size_t)n, sizeof *buf);  // zeroed == transparent black
    if (!buf) {
        *len = 0;
        return NULL;
    }
    *len = n;
    return buf;
}

// Decode the RGB planes of a [0,1]-normalized straight slab sRGB->linear, in
// place, leaving alpha alone -- the linear canvas's putImageData entry transfer.
// Scalar per lane (the f32 decode, cnvs_color.c's deferral note); incoming
// putImageData bytes are in [0,1] so the decode stays in [0,1], no extended
// values to carry.  Reached only on cv->space == CANVAS_CS_LINEAR_SRGB.
static cnvs_px8 px8_decode_rgb(cnvs_px8 p) {
    for (int i = 0; i < 8; i++) {
        p.r[i] = (_Float16)cnvs_srgb_to_linear((float)p.r[i]);
        p.g[i] = (_Float16)cnvs_srgb_to_linear((float)p.g[i]);
        p.b[i] = (_Float16)cnvs_srgb_to_linear((float)p.b[i]);
    }
    return p;
}

// putImageData's non-sRGB input transfers, in place over a [0,1]-normalized
// straight slab (alpha untouched -- coverage, not colour).  These mirror
// intern_color's non-sRGB branches at the bulk seam: reduce the incoming bytes
// to linear sRGB, then leave them linear for a LINEAR canvas or encode
// linear->sRGB for an sRGB canvas.  Scalar per lane (the transfer/Oklab math is
// f32 transcendental, no half8 spelling); reached only off the CANVAS_CS_SRGB
// fast path.  `linear_canvas` says the WORKING space is linear (skip the encode).

// Incoming bytes ARE linear sRGB (CANVAS_CS_LINEAR_SRGB input).
static cnvs_px8 px8_in_linear(cnvs_px8 p, bool linear_canvas) {
    if (linear_canvas) {
        return p;  // input already linear, working space linear: store as-is
    }
    for (int i = 0; i < 8; i++) {  // sRGB working canvas: encode linear->sRGB
        p.r[i] = (_Float16)cnvs_linear_to_srgb((float)p.r[i]);
        p.g[i] = (_Float16)cnvs_linear_to_srgb((float)p.g[i]);
        p.b[i] = (_Float16)cnvs_linear_to_srgb((float)p.b[i]);
    }
    return p;
}

// Incoming bytes are an Oklab (L,a,b) triple in the CNVS_OKLAB_AB_BIAS
// convention (above): un-bias a,b, Oklab->linear sRGB, then the linear handling.
static cnvs_px8 px8_in_oklab(cnvs_px8 p, bool linear_canvas) {
    for (int i = 0; i < 8; i++) {
        cnvs_oklab const lab = { .L = (float)p.r[i],
                                 .a = (float)p.g[i] - CNVS_OKLAB_AB_BIAS,
                                 .b = (float)p.b[i] - CNVS_OKLAB_AB_BIAS };
        cnvs_rgb lin = cnvs_oklab_to_linear_srgb(lab);
        if (!linear_canvas) {  // sRGB working canvas: encode linear->sRGB
            lin = cnvs_rgb_linear_to_srgb(lin);
        }
        p.r[i] = (_Float16)lin.r;
        p.g[i] = (_Float16)lin.g;
        p.b[i] = (_Float16)lin.b;
    }
    return p;
}

// Route a [0,1]-normalized straight slab from the INPUT space `space` into the
// working space (alpha already in [0,1] off the /255 divide).  Encoded-sRGB
// input: on a linear canvas it is exactly px8_decode_rgb, on an sRGB canvas a
// literal pass-through.
static cnvs_px8 put_to_working(cnvs_px8 p, enum canvas_color_space space,
                               bool linear_canvas) {
    if (space == CANVAS_CS_LINEAR_SRGB) {
        return px8_in_linear(p, linear_canvas);
    }
    if (space == CANVAS_CS_OKLAB) {
        return px8_in_oklab(p, linear_canvas);
    }
    // CANVAS_CS_SRGB (and any out-of-range value): px8_decode_rgb on a linear
    // canvas, a literal pass-through on an sRGB one.
    return linear_canvas ? px8_decode_rgb(p) : p;
}

// The f16 putImageData OKLAB input transfer: like px8_in_oklab, but the (L,a,b)
// arrive UNBIASED (the f16 path carries Oklab straight -- no CNVS_OKLAB_AB_BIAS
// window, which is a byte-transport detail), the inverse of unpremul_to_oklab_f16
// above.  Oklab->linear sRGB, then the linear handling.
static cnvs_px8 px8_in_oklab_f16(cnvs_px8 p, bool linear_canvas) {
    for (int i = 0; i < 8; i++) {
        cnvs_oklab const lab = { .L = (float)p.r[i],
                                 .a = (float)p.g[i],
                                 .b = (float)p.b[i] };
        cnvs_rgb lin = cnvs_oklab_to_linear_srgb(lab);
        if (!linear_canvas) {  // sRGB working canvas: encode linear->sRGB
            lin = cnvs_rgb_linear_to_srgb(lin);
        }
        p.r[i] = (_Float16)lin.r;
        p.g[i] = (_Float16)lin.g;
        p.b[i] = (_Float16)lin.b;
    }
    return p;
}

// The f16 putImageData router into the working space.  SRGB and LINEAR reuse the
// byte path's per-lane transfers verbatim (they are total -- extended values
// pass through unbounded); only OKLAB differs, taking the unbiased input above
// (the f16 readback's inverse) so an f16 Oklab put/get round-trips.
static cnvs_px8 put_to_working_f16(cnvs_px8 p, enum canvas_color_space space,
                                   bool linear_canvas) {
    if (space == CANVAS_CS_OKLAB) {
        return px8_in_oklab_f16(p, linear_canvas);
    }
    return put_to_working(p, space, linear_canvas);  // SRGB / LINEAR unchanged
}

// Copy the sub-rectangle [sx, sx+sw) x [sy, sy+sh) of the w-wide RGBA8 source onto
// the canvas with the ImageData origin at (dx, dy): source pixel (col, row) lands
// at (dx+col, dy+row).  Overwrites (no blending) and ignores the clip, clipped to
// the canvas.  The caller guarantees the sub-rect lies within the source
// ([0,w] x [0,h]) with sw, sh > 0, and len >= w*h*4.
static void put_image_sub(struct canvas *__single cv,
                          enum canvas_color_space space,
                          uint8_t const *__counted_by(len) data, int len,
                          int w, int dx, int dy, int sx, int sy, int sw, int sh) {
    (void)len;
    // Destination rect in canvas space, clamped to the canvas.  64-bit so a wild
    // (dx, dy) can't overflow the clamp arithmetic (the API boundary is untrusted).
    int64_t xs = (int64_t)dx + sx, ys = (int64_t)dy + sy;
    int64_t cx0 = xs < 0 ? 0 : xs, cy0 = ys < 0 ? 0 : ys;
    int64_t cx1 = xs + sw, cy1 = ys + sh;
    if (cx1 > cv->width)  { cx1 = cv->width; }
    if (cy1 > cv->height) { cy1 = cv->height; }
    int const rw = cx1 > cx0 ? (int)(cx1 - cx0) : 0;
    int const rh = cy1 > cy0 ? (int)(cy1 - cy0) : 0;
    if (rw <= 0 || rh <= 0 || !cnvs_ensure_tile(cv, rw * rh)) {
        return;
    }
    // Source column/row of the first painted canvas pixel; col0+px stays in
    // [sx, sx+sw) ⊆ [0,w) and row0+py in [sy, sy+sh) ⊆ [0,h), so si < w*h*4 <= len.
    // Eight pixels per step: ld4 deinterleaves the RGBA8 source into channel
    // planes (cnvs_planar.h), a true _Float16 divide scales each to [0,1],
    // and the planar premultiply writes finished tile pixels through st4.
    int const col0 = (int)(cx0 - dx);
    int const row0 = (int)(cy0 - dy);
    // The working space, and whether this input space needs any transfer.  For
    // encoded-sRGB input (CANVAS_CS_SRGB) put_to_working is px8_decode_rgb on a
    // linear canvas and a no-op on an sRGB canvas.
    bool const lin = cv->space == CANVAS_CS_LINEAR_SRGB;
    _Float16 const k255 = (_Float16)255.0f;
    for (int py = 0; py < rh; py++) {
        int px = 0;
        for (; px + 8 <= rw; px += 8) {
            int si = ((row0 + py) * w + (col0 + px)) * 4;
            cnvs_px8 p = cnvs_px8_load_rgba8(data + si);
            p = (cnvs_px8){ p.r / k255, p.g / k255, p.b / k255, p.a / k255 };
            p = put_to_working(p, space, lin);
            cnvs_px8_store(cv->tile + py * rw + px, cnvs_px8_premultiply(p));
        }
        if (px < rw) {
            int const k = rw - px;
            int const si = ((row0 + py) * w + (col0 + px)) * 4;
            cnvs_px8 p = cnvs_px8_load_rgba8_k(data + si, k);
            p = (cnvs_px8){ p.r / k255, p.g / k255, p.b / k255, p.a / k255 };
            p = put_to_working(p, space, lin);
            cnvs_px8_store_k(cv->tile + py * rw + px, k, cnvs_px8_premultiply(p));
        }
    }
    // putImageData overwrites and ignores the clip: composite COPY with no clip.
    cnvs_blend(cv, (int)cx0, (int)cy0, rw, rh, cv->tile, NULL, NULL, 0,
               CANVAS_OP_COPY);
}

// put_image_sub's f16 twin (rgba-float16 ImageData).  Same sub-rect contract,
// but the source is straight _Float16 RGBA (already normalized/extended, no /255
// scale) and the deposit preserves extended range: load straight via
// cnvs_px8_load_unpremul, put_to_working (total -- works on extended values),
// then a NON-clamping premultiply (cnvs_px8_premultiply_unclamped keeps the
// colour planes unbounded; only alpha clamps into [0,1]).  The COPY blend's
// output clamp is colour-unbounded on a linear canvas (cnvs_px8_clamp_premul_lin)
// and bounds on an sRGB one, so extended values survive end to end on a linear
// canvas and naturally bound on an sRGB one.  `len` is the element count (w*h*4);
// `data` is viewed as cnvs_unpremul (four contiguous _Float16, the ImageData
// layout).
static void put_image_sub_f16(struct canvas *__single cv,
                              enum canvas_color_space space,
                              _Float16 const *__counted_by(len) data, int len,
                              int w, int dx, int dy, int sx, int sy, int sw, int sh) {
    int64_t xs = (int64_t)dx + sx, ys = (int64_t)dy + sy;
    int64_t cx0 = xs < 0 ? 0 : xs, cy0 = ys < 0 ? 0 : ys;
    int64_t cx1 = xs + sw, cy1 = ys + sh;
    if (cx1 > cv->width)  { cx1 = cv->width; }
    if (cy1 > cv->height) { cy1 = cv->height; }
    int const rw = cx1 > cx0 ? (int)(cx1 - cx0) : 0;
    int const rh = cy1 > cy0 ? (int)(cy1 - cy0) : 0;
    if (rw <= 0 || rh <= 0 || !cnvs_ensure_tile(cv, rw * rh)) {
        return;
    }
    int const col0 = (int)(cx0 - dx);
    int const row0 = (int)(cy0 - dy);
    bool const lin = cv->space == CANVAS_CS_LINEAR_SRGB;
    // The w*h*4-element _Float16 source viewed as w*h cnvs_unpremul pixels; the
    // src index runs in pixels (each load takes four _Float16 per pixel).
    int const npx = len / 4;
    (void)npx;  // used only by the __counted_by annotation below (stripped unsafe)
    cnvs_unpremul const *__counted_by(npx) src = (cnvs_unpremul const *)data;
    for (int py = 0; py < rh; py++) {
        int px = 0;
        for (; px + 8 <= rw; px += 8) {
            int const si = (row0 + py) * w + (col0 + px);  // pixel index, in [0, npx)
            cnvs_px8 p = cnvs_px8_load_unpremul(src + si);  // straight f16, NO /255
            p = put_to_working_f16(p, space, lin);
            cnvs_px8_store(cv->tile + py * rw + px, cnvs_px8_premultiply_unclamped(p));
        }
        if (px < rw) {
            int const k = rw - px;
            int const si = (row0 + py) * w + (col0 + px);
            cnvs_px8 p = cnvs_px8_load_unpremul_k(src + si, k);
            p = put_to_working_f16(p, space, lin);
            cnvs_px8_store_k(cv->tile + py * rw + px, k, cnvs_px8_premultiply_unclamped(p));
        }
    }
    // putImageData overwrites and ignores the clip: composite COPY with no clip.
    cnvs_blend(cv, (int)cx0, (int)cy0, rw, rh, cv->tile, NULL, NULL, 0,
               CANVAS_OP_COPY);
}

void canvas_put_image_data(struct canvas *__single cv,
                           enum canvas_color_space space,
                           uint8_t const *__counted_by(len) data, int len,
                           int w, int h, int dx, int dy) {
    if (!cnvs_rgba8_dims_ok(w, h) || len < w * h * 4) {
        return;
    }
    if (cv->rec) {
        // Exactly the w*h*4 pixels the op reads ride the block; the int-typed
        // placement rides the op line.  The input's colour space rides the
        // BLOCK's colour-space tag (cnvs_rec_image), written for every space, so
        // every put_image_data round-trips through the tag (replay reads it back
        // off the block).
        int const id = cnvs_rec_image(cv->rec, data, w * h * 4, w, h,
                                      CANVAS_COLOR_UNORM8, CANVAS_ALPHA_UNPREMUL,
                                      space);
        if (id >= 0) {
            cnvs_rec_image_ints(cv->rec, "put_image_data", id,
                                (int[]){ dx, dy }, 2);
        }
    }
    put_image_sub(cv, space, data, len, w, dx, dy, 0, 0, w, h);
}

void canvas_put_image_data_dirty(struct canvas *__single cv,
                                 enum canvas_color_space space,
                                 uint8_t const *__counted_by(len) data, int len,
                                 int w, int h, int dx, int dy,
                                 int dirty_x, int dirty_y,
                                 int dirty_w, int dirty_h) {
    if (!cnvs_rgba8_dims_ok(w, h) || len < w * h * 4) {
        return;
    }
    if (cv->rec) {
        // The raw dirty args ride the op line; replay re-normalises them
        // through this very function, so the recorded form stays the call.
        // The input's colour space rides the block's tag, as for put_image_data
        // above: written for every space.
        int const id = cnvs_rec_image(cv->rec, data, w * h * 4, w, h,
                                      CANVAS_COLOR_UNORM8, CANVAS_ALPHA_UNPREMUL,
                                      space);
        if (id >= 0) {
            cnvs_rec_image_ints(cv->rec, "put_image_data_dirty", id,
                                (int[]){ dx, dy, dirty_x, dirty_y,
                                         dirty_w, dirty_h }, 6);
        }
    }
    // Normalise the dirty rect (ImageData space) into a sub-rect of [0,w] x [0,h],
    // per the spec: flip negative extents, then clamp to the source bounds.  All
    // in 64-bit so extreme/negative dirty args can't overflow.
    int64_t dxx = dirty_x, dyy = dirty_y, dww = dirty_w, dhh = dirty_h;
    if (dww < 0) { dxx += dww; dww = -dww; }
    if (dhh < 0) { dyy += dhh; dhh = -dhh; }
    if (dxx < 0) { dww += dxx; dxx = 0; }
    if (dyy < 0) { dhh += dyy; dyy = 0; }
    if (dxx + dww > w) { dww = (int64_t)w - dxx; }
    if (dyy + dhh > h) { dhh = (int64_t)h - dyy; }
    if (dww <= 0 || dhh <= 0) {
        return;  // empty (or fully-clipped) dirty rect: nothing to copy
    }
    put_image_sub(cv, space, data, len, w, dx, dy,
                  (int)dxx, (int)dyy, (int)dww, (int)dhh);
}

// canvas_put_image_data's f16 twin (rgba-float16 ImageData): the source is
// straight _Float16 RGBA in colour space `space`, extended range preserved.
// `len` is the element count (w*h*4).  Records via the SAME op line as the u8
// put -- the f16-ness rides the image block's colour type (CANVAS_COLOR_F16);
// the block carries the pixels in bytes, so its byte length is len*2.
void canvas_put_image_data_f16(struct canvas *__single cv,
                               enum canvas_color_space space,
                               _Float16 const *__counted_by(len) data, int len,
                               int w, int h, int dx, int dy) {
    if (!cnvs_rgba8_dims_ok(w, h) || len < w * h * 4) {
        return;
    }
    if (cv->rec) {
        // The block carries w*h*4 _Float16 (== w*h*8 bytes) as CANVAS_COLOR_F16;
        // the int-typed placement rides the op line, the colour space rides the
        // block's tag.  Same op spelling as the u8 put -- the colour type on the
        // block tells replay which API to call.
        int const id = cnvs_rec_image(cv->rec, (uint8_t const *)data,
                                      w * h * 4 * (int)sizeof(_Float16), w, h,
                                      CANVAS_COLOR_F16, CANVAS_ALPHA_UNPREMUL,
                                      space);
        if (id >= 0) {
            cnvs_rec_image_ints(cv->rec, "put_image_data", id,
                                (int[]){ dx, dy }, 2);
        }
    }
    put_image_sub_f16(cv, space, data, len, w, dx, dy, 0, 0, w, h);
}

void canvas_put_image_data_dirty_f16(struct canvas *__single cv,
                                     enum canvas_color_space space,
                                     _Float16 const *__counted_by(len) data, int len,
                                     int w, int h, int dx, int dy,
                                     int dirty_x, int dirty_y,
                                     int dirty_w, int dirty_h) {
    if (!cnvs_rgba8_dims_ok(w, h) || len < w * h * 4) {
        return;
    }
    if (cv->rec) {
        int const id = cnvs_rec_image(cv->rec, (uint8_t const *)data,
                                      w * h * 4 * (int)sizeof(_Float16), w, h,
                                      CANVAS_COLOR_F16, CANVAS_ALPHA_UNPREMUL,
                                      space);
        if (id >= 0) {
            cnvs_rec_image_ints(cv->rec, "put_image_data_dirty", id,
                                (int[]){ dx, dy, dirty_x, dirty_y,
                                         dirty_w, dirty_h }, 6);
        }
    }
    // Normalise the dirty rect exactly as canvas_put_image_data_dirty does.
    int64_t dxx = dirty_x, dyy = dirty_y, dww = dirty_w, dhh = dirty_h;
    if (dww < 0) { dxx += dww; dww = -dww; }
    if (dhh < 0) { dyy += dhh; dhh = -dhh; }
    if (dxx < 0) { dww += dxx; dxx = 0; }
    if (dyy < 0) { dhh += dyy; dyy = 0; }
    if (dxx + dww > w) { dww = (int64_t)w - dxx; }
    if (dyy + dhh > h) { dhh = (int64_t)h - dyy; }
    if (dww <= 0 || dhh <= 0) {
        return;  // empty (or fully-clipped) dirty rect: nothing to copy
    }
    put_image_sub_f16(cv, space, data, len, w, dx, dy,
                      (int)dxx, (int)dyy, (int)dww, (int)dhh);
}
