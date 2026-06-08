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

// Sample the tile at this pixel (framebuffer position minus the tile origin),
// scale its alpha by the clip coverage, and let the pipeline source-over it.
fragment float4 tile_blend_fs(tile_io in [[stage_in]],
                              float2 constant &origin [[buffer(0)]],
                              texture2d<float> tile [[texture(0)]],
                              texture2d<float> clip [[texture(1)]]) {
    uint2 p = uint2(in.pos.xy);
    float4 c = tile.read(p - uint2(origin));
    c.a *= clip.read(p).r;
    return c;
}

// Overwrite (putImageData): no clip, pipeline blending disabled.
fragment float4 tile_replace_fs(tile_io in [[stage_in]],
                                float2 constant &origin [[buffer(0)]],
                                texture2d<float> tile [[texture(0)]]) {
    return tile.read(uint2(in.pos.xy) - uint2(origin));
}

// Erase (clearRect): output alpha = clip coverage; the pipeline blend
// (factors Zero / OneMinusSourceAlpha) scales the destination by (1 - clip).
fragment float4 clear_fs(tile_io in [[stage_in]],
                         texture2d<float> clip [[texture(1)]]) {
    return float4(0.0, 0.0, 0.0, clip.read(uint2(in.pos.xy)).r);
}
