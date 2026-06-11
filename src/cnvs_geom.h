#pragma once

// Growable buffers: each data pointer and its count are siblings, so
// __counted_by(cap) is valid and the two must be reassigned together.

#include "cnvs_math.h"

#include <ptrcheck.h>

struct cnvs_verts {
    cnvs_vec2 *__counted_by(cap) data;
    int nverts;
    int cap;
};

// Append k vertices as one block: one capacity reserve and one bounds-checked
// copy, however many triangles the block holds.  Callers stage triangles in
// small local arrays and land them here, so the per-vertex cost is a plain
// store -- the memcpy through the __counted_by(cap) data pointer is the
// block's single bounds check.
bool cnvs_verts_append(struct cnvs_verts *v, cnvs_vec2 const *__counted_by(k) src, int k);
void cnvs_verts_reset(struct cnvs_verts *v);
void cnvs_verts_free(struct cnvs_verts *v);
