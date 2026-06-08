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

// Gouraud path: each vertex carries its own colour (gradients are evaluated on
// the CPU and baked into vertex colours), interpolated across the triangle.
// packed_* keeps the layout identical to the C `gpu_cvert` (no float4 padding).
struct grad_in {
    packed_float2 pos;
    packed_float4 color;
};

struct grad_io {
    float4 pos [[position]];
    float4 color;
};

vertex grad_io grad_vs(uint vid [[vertex_id]],
                       grad_in device const *verts [[buffer(0)]],
                       float2 constant &viewport [[buffer(1)]]) {
    grad_in v = verts[vid];
    float2 ndc = float2((v.pos.x / viewport.x) * 2.0 - 1.0,
                        1.0 - (v.pos.y / viewport.y) * 2.0);
    grad_io o;
    o.pos = float4(ndc, 0.0, 1.0);
    o.color = v.color;
    return o;
}

fragment float4 grad_fs(grad_io in [[stage_in]]) {
    return in.color;
}
