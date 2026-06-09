#include "cnvs_geom.h"

#include "cnvs_mem.h"

#include <stdlib.h>

static bool verts_reserve(cnvs_verts *v, int need) {
    if (need <= v->cap) {
        return true;
    }
    int newcap = cnvs_grow_cap(v->cap, need);
    cnvs_vec2 *nd = realloc(v->data, (size_t)newcap * sizeof *nd);
    if (!nd) {
        return false;
    }
    v->data = nd;
    v->cap = newcap;
    return true;
}

bool cnvs_verts_push(cnvs_verts *v, cnvs_vec2 p) {
    if (!verts_reserve(v, v->len + 1)) {
        return false;
    }
    v->data[v->len] = p;
    v->len += 1;
    return true;
}

bool cnvs_verts_tri(cnvs_verts *v, cnvs_vec2 a, cnvs_vec2 b, cnvs_vec2 c) {
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
