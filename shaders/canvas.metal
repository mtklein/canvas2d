#include <metal_stdlib>
using namespace metal;

// y is flipped so canvas pixel (0,0) maps to the image's top-left corner.
vertex float4 solid_vs(uint vid [[vertex_id]],
                       float2 device const *verts [[buffer(0)]],
                       float2 constant &viewport [[buffer(1)]]) {
    float2 p = verts[vid];
    float2 ndc = float2((p.x / viewport.x) * 2.0 - 1.0,
                        1.0 - (p.y / viewport.y) * 2.0);
    return float4(ndc, 0.0, 1.0);
}

fragment float4 solid_fs(float4 constant &color [[buffer(0)]]) {
    return color;
}
