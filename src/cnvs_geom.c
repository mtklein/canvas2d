#include "cnvs_geom.h"

#include "cnvs_mem.h"

#include <stdlib.h>

static bool verts_reserve(cnvs_verts *v, int need) {
    if (need <= v->cap) {
        return true;
    }
    int newcap = cnvs_grow_cap(v->cap, need);
    gpu_vert *nd = realloc(v->data, (size_t)newcap * sizeof *nd);
    if (!nd) {
        return false;
    }
    v->data = nd;
    v->cap = newcap;
    return true;
}

bool cnvs_verts_push(cnvs_verts *v, gpu_vert p) {
    if (!verts_reserve(v, v->len + 1)) {
        return false;
    }
    v->data[v->len] = p;
    v->len += 1;
    return true;
}

bool cnvs_verts_tri(cnvs_verts *v, gpu_vert a, gpu_vert b, gpu_vert c) {
    if (!verts_reserve(v, v->len + 3)) {
        return false;
    }
    v->data[v->len] = a;
    v->data[v->len + 1] = b;
    v->data[v->len + 2] = c;
    v->len += 3;
    return true;
}

void cnvs_verts_reset(cnvs_verts *v) {
    v->len = 0;
}

void cnvs_verts_free(cnvs_verts *v) {
    free(v->data);
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
}

static bool ints_reserve(cnvs_ints *v, int need) {
    if (need <= v->cap) {
        return true;
    }
    int newcap = cnvs_grow_cap(v->cap, need);
    int *nd = realloc(v->data, (size_t)newcap * sizeof *nd);
    if (!nd) {
        return false;
    }
    v->data = nd;
    v->cap = newcap;
    return true;
}

bool cnvs_ints_push(cnvs_ints *v, int value) {
    if (!ints_reserve(v, v->len + 1)) {
        return false;
    }
    v->data[v->len] = value;
    v->len += 1;
    return true;
}

void cnvs_ints_remove(cnvs_ints *v, int index) {
    if (index < 0 || index >= v->len) {
        return;
    }
    for (int i = index; i + 1 < v->len; i++) {
        v->data[i] = v->data[i + 1];
    }
    v->len -= 1;
}

void cnvs_ints_reset(cnvs_ints *v) {
    v->len = 0;
}

void cnvs_ints_free(cnvs_ints *v) {
    free(v->data);
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
}
