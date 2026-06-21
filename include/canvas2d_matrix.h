#pragma once

#include <stdbool.h>

// 2D projective (homography) transforms, a deliberate extension beyond the HTML
// Canvas 2D affine spec (docs/decisions/perspective.md).  A matrix maps (x, y) to
//     x' = (a*x + c*y + e) / w,  y' = (b*x + d*y + f) / w,  w = g*x + h*y + i.
// Affine is the (g, h, i) = (0, 0, 1) subset -- then w == 1, no divide, and the
// (a, b, c, d, e, f) are the CanvasRenderingContext2D.setTransform() six.  The
// affine subset stays on a divide-free fast path so existing scenes compute bit
// for bit as before.

typedef struct {
    float x, y;
} canvas2d_vec2;

typedef struct {
    float a, b, c, d, e, f, g, h, i;
} canvas2d_matrix;

canvas2d_matrix canvas2d_matrix_identity(void);

// Whether the bottom row is (0, 0, 1) -- the affine subset, on which apply/mul/
// invert take a divide-free path that reproduces the old 2x3 arithmetic exactly.
bool canvas2d_matrix_is_affine(canvas2d_matrix m);

// apply(mul(m, n), p) == apply(m, apply(n, p)): n is applied first, as when
// Canvas chains translate() then scale().
canvas2d_matrix canvas2d_matrix_mul(canvas2d_matrix m, canvas2d_matrix n);

// translate/scale/rotate build affine matrices (g = h = 0, i = 1); their (a..f)
// values are unchanged from the 2x3 era.
canvas2d_matrix canvas2d_matrix_translate(float tx, float ty);
canvas2d_matrix canvas2d_matrix_scale(float sx, float sy);
canvas2d_matrix canvas2d_matrix_rotate(float radians);

// Apply to (x, y): affine maps with no divide (bit-identical to the 2x3 era);
// projective divides by w.
canvas2d_vec2 canvas2d_matrix_apply(canvas2d_matrix m, canvas2d_vec2 p);

// Inverse; identity if (near-)singular.  Affine inputs yield the affine inverse
// bit-identically.
canvas2d_matrix canvas2d_matrix_invert(canvas2d_matrix m);
