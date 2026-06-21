#pragma once

// Growable buffers: each data pointer and its count are siblings, so
// __counted_by(cap) is valid and the two must be reassigned together.

#include "canvas2d_math.h"
#include "canvas2d_matrix.h"

#include <ptrcheck.h>

struct canvas2d_verts {
    canvas2d_vec2 *__counted_by(cap) data;
    int nverts;
    int cap;
};

// Append k vertices as one block: one capacity reserve and one bounds-checked
// copy, however many triangles the block holds.  Callers stage triangles in
// small local arrays and land them here, so the per-vertex cost is a plain
// store -- the memcpy through the __counted_by(cap) data pointer is the
// block's single bounds check.
bool canvas2d_verts_append(struct canvas2d_verts *v, canvas2d_vec2 const *__counted_by(k) src, int k);
void canvas2d_verts_reset(struct canvas2d_verts *v);
void canvas2d_verts_free(struct canvas2d_verts *v);

// canvas2d_matrix_apply for eight pixel centres along one row: only x varies and the
// affine map is elementwise, so the scalar expression runs per lane bit for
// bit.
typedef struct {
    f32x8 x, y;
} foldv8;

foldv8 mat_apply8(canvas2d_matrix m, f32x8 x, float y);

// Perspective-correct canvas2d_matrix_apply, eight pixel centres along one row.  The
// three homogeneous numerators u = a*x + c*y + e, v = b*x + d*y + f, and
// w = g*x + h*y + i are each LINEAR in x, so they step with the row (computed
// here 8 wide); the source coord is (u/w, v/w), the per-pixel divide the only
// added work over the affine mat_apply8.  Used only on the !canvas2d_matrix_is_affine
// sampler branches -- the affine branch keeps mat_apply8's divide-free DDA, bit
// for bit.  This is the 8-wide twin of canvas2d_matrix_apply's projective arm.
foldv8 mat_apply8_persp(canvas2d_matrix m, f32x8 x, float y);
