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

// Colour channels are _Float16 -- the project's lingua franca for colour: native
// on this hardware, half the footprint of float32 in the pixel tiles, and a direct
// match for Metal's `half` / RGBA16Float.  Plenty of precision for [0,1] colour
// (and headroom past it); 8-bit only at the edges.
//
// Two distinct types so premultiplied and straight colour can never be confused:
// cnvs_unpremul is what the Canvas API speaks (r,g,b independent of a); cnvs_premul
// is what every internal pixel buffer holds (r,g,b already scaled by a).  Convert
// only through cnvs_premultiply / cnvs_unpremultiply -- never by reinterpreting one
// as the other, even though they share a layout.
typedef struct cnvs_unpremul {
    _Float16 r, g, b, a;
} cnvs_unpremul;

typedef struct cnvs_premul {
    _Float16 r, g, b, a;
} cnvs_premul;

// Build a straight colour from float components (the float -> _Float16 narrow site).
cnvs_unpremul cnvs_unpremul_of(float r, float g, float b, float a);

// The explicit conversions.  premultiply scales r,g,b by a; unpremultiply divides
// it back out (a == 0 yields all-zero).  Both clamp channels to [0,1].
cnvs_premul cnvs_premultiply(cnvs_unpremul c);
cnvs_unpremul cnvs_unpremultiply(cnvs_premul c);

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

// Inverse of an affine transform; cnvs_mat_identity() if (near-)singular.
cnvs_mat cnvs_mat_invert(cnvs_mat m);
