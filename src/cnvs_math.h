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
