#include "cnvs_geom.h"

#include "cnvs_mem.h"

#include <stdlib.h>
#include <string.h>

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

bool cnvs_verts_append(cnvs_verts *v, cnvs_vec2 const *__counted_by(k) src, int k) {
    if (k <= 0) {
        return true;
    }
    if (!verts_reserve(v, v->len + k)) {
        return false;
    }
    memcpy(v->data + v->len, src, (size_t)k * sizeof *src);
    v->len += k;
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
