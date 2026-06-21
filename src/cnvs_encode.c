#include "canvas.h"

#include "cnvs_canvas.h"
#include "cnvs_color.h"
#include "cnvs_math.h"
#include "cnvs_png.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// --- 16-bit Rec.2020 / PQ PNG output ----------------------------------------
//
// PNG output is BT.2100: 16-bit, Rec.2020 primaries, PQ (ST 2084) transfer,
// signalled by a cICP chunk (the cnvs_png encoder writes the chunk; this is the
// pixel pipeline that feeds it).  Per pixel off the premultiplied f16 surface:
// unpremultiply to linear sRGB light (an sRGB working surface decodes; a linear
// surface is already linear, with extended HDR / wide-gamut values intact),
// rotate to Rec.2020 primaries, scale to the PQ range against the reference
// white, PQ-encode, quantize to 16-bit.  Alpha is straight, linear, 16-bit
// (coverage, no transfer).

// A linear working value of 1.0 maps to this many cd/m^2 (PQ then normalizes
// against its 10000-nit ceiling).  203 is BT.2408 HDR reference white; 100 is a
// common alternative.  The one knob.
#define CNVS_REF_WHITE_NITS 203.0f

static uint16_t q16(float x) {  // [0,1] -> 16-bit unorm
    float const v = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    return (uint16_t)(v * 65535.0f + 0.5f);
}

// 8-wide q16: clamp to [0,1], scale, round, truncate -- the lanewise twin of q16.
// min/max clamp matches the scalar ternary for the finite [0,1]-ish inputs here,
// and convertvector truncates toward zero like the (uint16_t) cast.
static int8 q16x8(float8 x) {
    float8 const v = __builtin_elementwise_max((float8)0.0f,
                     __builtin_elementwise_min((float8)1.0f, x));
    return __builtin_convertvector(v * 65535.0f + 0.5f, int8);
}

// Fill `out` (width*height*4 native-endian uint16) with the surface as BT.2100.
static void surface_to_pq16(struct canvas *__single cv,
                            uint16_t *__counted_by(n4) out, int n4) {
    (void)n4;
    bool const lin = cv->space == CANVAS_CS_LINEAR_SRGB;
    float const scale = CNVS_REF_WHITE_NITS / 10000.0f;
    int const n = cv->target_len;

    // One pixel's worth of the scalar pipeline into RGB planes (+ alpha): unpremul
    // -> (sRGB->linear) -> Rec.2020 -> *scale.  A transparent pixel contributes 0
    // to every plane, which PQ maps to 0 -- the all-zero output the spec wants.
    // The PQ OETF and quantize then run 8 pixels at a time; the cheap per-pixel
    // unpremul/matrix stays scalar (data-dependent 1/a, AoS source), bit-exact.
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        float8 wr = {0}, wg = {0}, wb = {0}, af = {0};
        for (int j = 0; j < 8; j++) {
            cnvs_premul const px = cv->target[i + j];
            float const a = (float)px.a;
            af[j] = a;
            if (a > 0.0f) {
                float const inv = 1.0f / a;
                cnvs_rgb l = { (float)px.r * inv, (float)px.g * inv, (float)px.b * inv };
                if (!lin) {
                    l = cnvs_rgb_srgb_to_linear(l);
                }
                cnvs_rgb const wide = cnvs_linear_srgb_to_rec2020(l);
                wr[j] = wide.r * scale;
                wg[j] = wide.g * scale;
                wb[j] = wide.b * scale;
            }
        }
        int8 const r = q16x8(cnvs_pq_oetf8(wr));
        int8 const g = q16x8(cnvs_pq_oetf8(wg));
        int8 const b = q16x8(cnvs_pq_oetf8(wb));
        int8 const av = q16x8(af);
        for (int j = 0; j < 8; j++) {
            out[(i + j) * 4 + 0] = (uint16_t)r[j];
            out[(i + j) * 4 + 1] = (uint16_t)g[j];
            out[(i + j) * 4 + 2] = (uint16_t)b[j];
            out[(i + j) * 4 + 3] = (uint16_t)av[j];
        }
    }
    for (; i < n; i++) {  // scalar tail (n % 8), the original per-pixel path
        cnvs_premul const px = cv->target[i];
        float const a = (float)px.a;
        uint16_t r = 0, g = 0, b = 0, av = 0;
        if (a > 0.0f) {  // a transparent pixel stays all-zero
            float const inv = 1.0f / a;
            cnvs_rgb l = { (float)px.r * inv, (float)px.g * inv, (float)px.b * inv };
            if (!lin) {
                l = cnvs_rgb_srgb_to_linear(l);  // sRGB surface -> linear light
            }
            cnvs_rgb const wide = cnvs_linear_srgb_to_rec2020(l);
            r  = q16(cnvs_pq_oetf(wide.r * scale));
            g  = q16(cnvs_pq_oetf(wide.g * scale));
            b  = q16(cnvs_pq_oetf(wide.b * scale));
            av = q16(a);
        }
        out[i * 4 + 0] = r;
        out[i * 4 + 1] = g;
        out[i * 4 + 2] = b;
        out[i * 4 + 3] = av;
    }
}

// Encode the canvas to a PNG in memory (caller frees), *outlen its byte length.
// The in-memory sibling of canvas_write_png; the byte gate uses it to compare
// against a committed file without round-tripping through disk.  NULL on OOM.
uint8_t *__counted_by_or_null(*outlen)
canvas_encode_png(struct canvas *__single cv, int *__single outlen) {
    *outlen = 0;
    int const n4 = cv->width * cv->height * 4;
    uint16_t *__counted_by_or_null(n4) px = malloc((size_t)n4 * sizeof *px);
    if (!px) {
        return NULL;
    }
    surface_to_pq16(cv, px, n4);
    uint8_t *out = cnvs_png_encode(px, cv->width, cv->height, outlen);
    free(px);
    return out;
}

bool canvas_write_png(struct canvas *__single cv, char const *__null_terminated path) {
    int len = 0;
    uint8_t *out = canvas_encode_png(cv, &len);
    if (!out) {
        return false;
    }
    bool ok = false;
    FILE *f = fopen(path, "wb");
    if (f) {
        ok = (fwrite(out, 1, (size_t)len, f) == (size_t)len);
        ok = (fclose(f) == 0) && ok;
    }
    free(out);
    return ok;
}
