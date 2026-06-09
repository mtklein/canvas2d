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

// Overwrite (putImageData): no clip, pipeline blending disabled.  The source is
// straight RGBA8, so premultiply it before storing into the premultiplied target.
fragment float4 tile_replace_fs(tile_io in [[stage_in]],
                                float2 constant &origin [[buffer(0)]],
                                texture2d<float> tile [[texture(0)]]) {
    float4 c = tile.read(uint2(in.pos.xy) - uint2(origin));
    c.rgb *= c.a;
    return c;
}

// Erase (clearRect): output alpha = clip coverage; the pipeline blend
// (factors Zero / OneMinusSourceAlpha) scales the destination by (1 - clip).
fragment float4 clear_fs(tile_io in [[stage_in]],
                         texture2d<float> clip [[texture(1)]]) {
    return float4(0.0, 0.0, 0.0, clip.read(uint2(in.pos.xy)).r);
}

// --- globalCompositeOperation (W3C Compositing and Blending Level 1) ----------
// Source-over has a fixed-function fast path (tile_blend_fs); every other mode
// goes through tile_composite_fs below, which reads the backdrop via framebuffer
// fetch and writes the finished straight-alpha result (pipeline blending off).
// Mode integers match compositor_blend_mode in compositor.h.

// Separable blend B(cb, cs), evaluated per channel; cb/cs are straight [0,1].
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

fragment float4 tile_composite_fs(tile_io in [[stage_in]],
                                  float4 dst [[color(0)]],
                                  float2 constant &origin [[buffer(0)]],
                                  uint constant &mode [[buffer(1)]],
                                  texture2d<float> tile [[texture(0)]],
                                  texture2d<float> clip [[texture(1)]]) {
    uint2 p = uint2(in.pos.xy);
    float4 s = tile.read(p - uint2(origin));
    float as = s.a * clip.read(p).r;                      // source alpha (clip-attenuated)
    float3 cs = s.a > 0.0 ? s.rgb / s.a : float3(0.0);    // un-premultiply to straight
    float ab = dst.a;
    float3 cb = ab > 0.0 ? dst.rgb / ab : float3(0.0);    // backdrop, un-premultiplied

    // Blend modes (>=11) replace the source colour with the backdrop-mixed blend,
    // then composite as source-over; Porter-Duff operators use their own factors.
    float3 csb = cs;
    if (mode >= 22) {
        csb = (1.0 - ab) * cs + ab * blend_nonsep(mode, cb, cs);
    } else if (mode >= 11) {
        float3 b = float3(blend_sep(mode, cb.r, cs.r),
                          blend_sep(mode, cb.g, cs.g),
                          blend_sep(mode, cb.b, cs.b));
        csb = (1.0 - ab) * cs + ab * b;
    }

    // Porter-Duff (Fa, Fb); blend modes composite as source-over.
    float fa, fb;
    switch (mode) {
        case 1:  fa = ab;        fb = 0.0;       break;  // source-in
        case 2:  fa = 1.0 - ab;  fb = 0.0;       break;  // source-out
        case 3:  fa = ab;        fb = 1.0 - as;  break;  // source-atop
        case 4:  fa = 1.0 - ab;  fb = 1.0;       break;  // destination-over
        case 5:  fa = 0.0;       fb = as;        break;  // destination-in
        case 6:  fa = 0.0;       fb = 1.0 - as;  break;  // destination-out
        case 7:  fa = 1.0 - ab;  fb = as;        break;  // destination-atop
        case 8:  fa = 1.0 - ab;  fb = 1.0 - as;  break;  // xor
        case 9:  fa = 1.0;       fb = 1.0;       break;  // lighter
        case 10: fa = 1.0;       fb = 0.0;       break;  // copy
        default: fa = 1.0;       fb = 1.0 - as;  break;  // source-over & blend modes
    }

    float ao = as * fa + ab * fb;
    float3 co = as * fa * csb + ab * fb * cb;     // premultiplied result colour
    // Clamp alpha to 1 (additive 'lighter' can reach 2) and keep the colour within
    // [0, alpha] so the stored value is a valid premultiplied pixel.
    float aoc = min(ao, 1.0);
    return float4(clamp(co, 0.0, aoc), aoc);
}
