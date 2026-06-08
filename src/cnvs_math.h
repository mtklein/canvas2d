#pragma once

// 2D affine transforms, matching the HTML Canvas 2D convention.
//
// A matrix maps a point (x, y) to:
//     (a*x + c*y + e,  b*x + d*y + f)
// i.e. the 3x3 homogeneous matrix
//     | a c e |
//     | b d f |
//     | 0 0 1 |
// The six fields are exactly the (a, b, c, d, e, f) arguments of
// CanvasRenderingContext2D.setTransform().

typedef struct {
    float x, y;
} cnvs_vec2;

// Straight-alpha colour.  Channels are _Float16 -- the project's lingua franca
// for colour: native on this hardware, half the footprint of float32 in the
// pixel tiles, and a direct match for Metal's `half` / RGBA16Float.  Plenty of
// precision for [0,1] colour (and headroom past it); 8-bit only at the edges.
typedef struct {
    _Float16 r, g, b, a;
} cnvs_rgba;

// Build a colour from float components (the only place float -> _Float16 narrows).
cnvs_rgba cnvs_rgba_of(float r, float g, float b, float a);

// sRGB transfer functions on a single colour channel, both clamped to [0,1].
// Convention: float/16F values are linear-light; unorm r,g,b are sRGB-encoded
// (alpha and coverage are always linear, so they don't use these).
float cnvs_srgb_encode(float linear);  // linear -> sRGB (for 8-bit output)
float cnvs_srgb_decode(float srgb);    // sRGB -> linear (for 8-bit input)

typedef struct {
    float a, b, c, d, e, f;
} cnvs_mat;

cnvs_mat cnvs_mat_identity(void);

// Composition: cnvs_mat_apply(cnvs_mat_mul(m, n), p) == apply(m, apply(n, p)).
// So `n` is applied first, then `m` -- the order Canvas uses when you call
// translate() then scale(): the later call affects coordinates first.
cnvs_mat cnvs_mat_mul(cnvs_mat m, cnvs_mat n);

cnvs_mat cnvs_mat_translate(float tx, float ty);
cnvs_mat cnvs_mat_scale(float sx, float sy);
cnvs_mat cnvs_mat_rotate(float radians);

cnvs_vec2 cnvs_mat_apply(cnvs_mat m, cnvs_vec2 p);
