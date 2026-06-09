#include <metal_stdlib>
using namespace metal;

// A quad in canvas pixel space (origin top-left, +y down) -> NDC.
struct tile_io {
    float4 pos [[position]];
};

vertex tile_io tile_vs(uint vid [[vertex_id]],
                       float2 device const *verts [[buffer(0)]],
                       float2 constant &viewport [[buffer(1)]]) {
    float2 p = verts[vid];
    tile_io o;
    o.pos = float4((p.x / viewport.x) * 2.0 - 1.0,
                   1.0 - (p.y / viewport.y) * 2.0, 0.0, 1.0);
    return o;
}

// Sample the premultiplied tile at this pixel (framebuffer position minus the
// tile origin) and scale it by the clip coverage; the pipeline source-overs it.
// Premultiplied, so coverage scales colour and alpha together (one multiply).
fragment float4 tile_blend_fs(tile_io in [[stage_in]],
                              float2 constant &origin [[buffer(0)]],
                              texture2d<float> tile [[texture(0)]],
                              texture2d<float> clip [[texture(1)]]) {
    uint2 p = uint2(in.pos.xy);
    return tile.read(p - uint2(origin)) * clip.read(p).r;
}

// --- globalCompositeOperation (W3C Compositing and Blending Level 1) ----------
// Source-over has a fixed-function fast path (tile_blend_fs); every other mode
// goes through tile_composite_fs below, which reads the *premultiplied* backdrop
// via framebuffer fetch and writes a premultiplied result (pipeline blending off).
// Everything is premultiplied throughout: the result is
//     co = s*(1-da) + d*(1-sa) + T,   ao = sa + da*(1-sa)
// for the blend modes (source-over compositing of the blended colour), where the
// premultiplied blend term T = sa*da*B(Cb, Cs).  The polynomial separable modes
// have divide-free premultiplied forms for T; the intrinsically non-linear ones
// (dodge/burn/soft-light and the HSL set) are *defined* on straight colour, so
// they un-premultiply with a guarded divide.  Mode integers match
// compositor_blend_mode in compositor.h.

// Separable blend B(cb, cs) on straight channels -- used only for the non-linear
// modes (color-dodge/-burn, soft-light); the rest fold into pm_term below.
static float blend_sep(uint mode, float cb, float cs) {
    switch (mode) {
        case 11: return cb * cs;                                    // multiply
        case 12: return cb + cs - cb * cs;                          // screen
        case 13: return cb <= 0.5 ? 2.0 * cs * cb                   // overlay
                                  : 1.0 - 2.0 * (1.0 - cs) * (1.0 - cb);
        case 14: return min(cb, cs);                                // darken
        case 15: return max(cb, cs);                                // lighten
        case 16: return cb == 0.0 ? 0.0                             // color-dodge
                       : cs >= 1.0 ? 1.0 : min(1.0, cb / (1.0 - cs));
        case 17: return cb >= 1.0 ? 1.0                             // color-burn
                       : cs <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - cb) / cs);
        case 18: return cs <= 0.5 ? 2.0 * cb * cs                   // hard-light
                                  : 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs);
        case 19: {                                                  // soft-light
            float d = cb <= 0.25 ? ((16.0 * cb - 12.0) * cb + 4.0) * cb : sqrt(cb);
            return cs <= 0.5 ? cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb)
                             : cb + (2.0 * cs - 1.0) * (d - cb);
        }
        case 20: return abs(cb - cs);                               // difference
        case 21: return cb + cs - 2.0 * cb * cs;                    // exclusion
        default: return cs;
    }
}

static float lum(float3 c) { return 0.3 * c.r + 0.59 * c.g + 0.11 * c.b; }

static float3 clip_color(float3 c) {
    float l = lum(c);
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    if (n < 0.0) { c = l + (c - l) * (l / (l - n)); }
    if (x > 1.0) { c = l + (c - l) * ((1.0 - l) / (x - l)); }
    return c;
}

static float3 set_lum(float3 c, float l) { return clip_color(c + (l - lum(c))); }

// Set saturation s: max channel -> s, min -> 0, mid -> proportional.  The
// vectorized form reproduces the spec's min/mid/max walk without sorting.
static float3 set_sat(float3 c, float s) {
    float mn = min(c.r, min(c.g, c.b));
    float mx = max(c.r, max(c.g, c.b));
    return mx > mn ? (c - mn) * (s / (mx - mn)) : float3(0.0);
}

static float sat3(float3 c) {
    return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
}

// Non-separable blend (hue/saturation/color/luminosity) on the whole colour.
static float3 blend_nonsep(uint mode, float3 cb, float3 cs) {
    switch (mode) {
        case 22: return set_lum(set_sat(cs, sat3(cb)), lum(cb));  // hue
        case 23: return set_lum(set_sat(cb, sat3(cs)), lum(cb));  // saturation
        case 24: return set_lum(cs, lum(cb));                     // color
        default: return set_lum(cb, lum(cs));                     // luminosity (25)
    }
}

// Premultiplied separable blend term T = sa*da*B(Cb,Cs) for one channel; s,d are
// premultiplied.  The polynomial modes are divide-free; the non-linear ones reuse
// the straight B with a single guarded un-premultiply.
static float pm_term(uint mode, float s, float d, float sa, float da) {
    switch (mode) {
        case 11: return s * d;                                  // multiply
        case 12: return sa * d + da * s - s * d;                // screen
        case 13: return 2.0 * d <= da ? 2.0 * s * d             // overlay
                       : sa * da - 2.0 * (da - d) * (sa - s);
        case 14: return min(s * da, d * sa);                    // darken
        case 15: return max(s * da, d * sa);                    // lighten
        case 18: return 2.0 * s <= sa ? 2.0 * s * d             // hard-light
                       : sa * da - 2.0 * (da - d) * (sa - s);
        case 20: return abs(s * da - d * sa);                   // difference
        case 21: return sa * d + da * s - 2.0 * s * d;          // exclusion
        default: {                                              // dodge/burn/soft-light
            float cs = sa > 0.0 ? s / sa : 0.0;
            float cb = da > 0.0 ? d / da : 0.0;
            return sa * da * blend_sep(mode, cb, cs);
        }
    }
}

fragment float4 tile_composite_fs(tile_io in [[stage_in]],
                                  float4 dst [[color(0)]],
                                  float2 constant &origin [[buffer(0)]],
                                  uint constant &mode [[buffer(1)]],
                                  texture2d<float> tile [[texture(0)]],
                                  texture2d<float> clip [[texture(1)]]) {
    uint2 p = uint2(in.pos.xy);
    float4 sp = tile.read(p - uint2(origin)) * clip.read(p).r;  // premult src, clip-attenuated
    float3 s = sp.rgb;
    float sa = sp.a;
    float3 d = dst.rgb;   // premultiplied backdrop (framebuffer fetch)
    float da = dst.a;

    float3 co;
    float ao;
    if (mode <= 10) {
        // Porter-Duff operators: co = Fa*s + Fb*d, ao = Fa*sa + Fb*da.
        float fa, fb;
        switch (mode) {
            case 1:  fa = da;        fb = 0.0;       break;  // source-in
            case 2:  fa = 1.0 - da;  fb = 0.0;       break;  // source-out
            case 3:  fa = da;        fb = 1.0 - sa;  break;  // source-atop
            case 4:  fa = 1.0 - da;  fb = 1.0;       break;  // destination-over
            case 5:  fa = 0.0;       fb = sa;        break;  // destination-in
            case 6:  fa = 0.0;       fb = 1.0 - sa;  break;  // destination-out
            case 7:  fa = 1.0 - da;  fb = sa;        break;  // destination-atop
            case 8:  fa = 1.0 - da;  fb = 1.0 - sa;  break;  // xor
            case 9:  fa = 1.0;       fb = 1.0;       break;  // lighter
            case 10: fa = 1.0;       fb = 0.0;       break;  // copy
            default: fa = 1.0;       fb = 1.0 - sa;  break;  // source-over
        }
        co = fa * s + fb * d;
        ao = fa * sa + fb * da;
    } else {
        // Blend modes composite as source-over of the blended colour.
        float3 t = mode <= 21
                 ? float3(pm_term(mode, s.r, d.r, sa, da),
                          pm_term(mode, s.g, d.g, sa, da),
                          pm_term(mode, s.b, d.b, sa, da))
                 : sa * da * blend_nonsep(mode, da > 0.0 ? d / da : float3(0.0),
                                                sa > 0.0 ? s / sa : float3(0.0));
        co = s * (1.0 - da) + d * (1.0 - sa) + t;
        ao = sa + da * (1.0 - sa);
    }

    // Clamp alpha to 1 (additive 'lighter' can reach 2) and keep the premultiplied
    // colour within [0, alpha] so the stored pixel stays valid.
    float aoc = min(ao, 1.0);
    return float4(clamp(co, 0.0, aoc), aoc);
}
