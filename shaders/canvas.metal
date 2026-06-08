#include <metal_stdlib>
using namespace metal;

// Vertices arrive in canvas pixel space (origin top-left, +y down).  The vertex
// shader maps them to clip space using the viewport size, flipping y so that
// pixel (0,0) lands at the top-left of the rendered image.
vertex float4 solid_vs(uint vid [[vertex_id]],
                       device float2 const *verts [[buffer(0)]],
                       constant float2 &viewport [[buffer(1)]]) {
    float2 p = verts[vid];
    float2 ndc = float2((p.x / viewport.x) * 2.0 - 1.0,
                        1.0 - (p.y / viewport.y) * 2.0);
    return float4(ndc, 0.0, 1.0);
}

fragment float4 solid_fs(constant float4 &color [[buffer(0)]]) {
    return color;
}
