#pragma once

// Growable buffers: each data pointer and its count are siblings, so
// __counted_by(cap) is valid and the two must be reassigned together.

#include "gpu.h"

#include <ptrcheck.h>

typedef struct {
    gpu_vert *__counted_by(cap) data;
    int len;
    int cap;
} cnvs_verts;

bool cnvs_verts_push(cnvs_verts *v, gpu_vert p);
bool cnvs_verts_tri(cnvs_verts *v, gpu_vert a, gpu_vert b, gpu_vert c);
void cnvs_verts_reset(cnvs_verts *v);
void cnvs_verts_free(cnvs_verts *v);

typedef struct {
    int *__counted_by(cap) data;
    int len;
    int cap;
} cnvs_ints;

bool cnvs_ints_push(cnvs_ints *v, int value);
void cnvs_ints_remove(cnvs_ints *v, int index);
void cnvs_ints_reset(cnvs_ints *v);
void cnvs_ints_free(cnvs_ints *v);
