#include "cnvs_geom.h"

#include "cnvs_mem.h"

#include <stdlib.h>
#include <string.h>

typedef float cnvs_geom_f8 __attribute__((ext_vector_type(8)));
typedef float cnvs_geom_f16 __attribute__((ext_vector_type(16)));

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
    // Blocks are small (a quad stages 6 verts, a join wedge 3), so a variable-
    // size memcpy goes out of line to libc memmove and the call costs more
    // than the copy; the constant-size vector memcpys below inline to one
    // load/store pair each.  The destination converts to a counted local
    // FIRST -- that's the block's one real bounds check; copying through
    // v->data directly would reload data/len/cap and recheck every step, since
    // the stores could alias *v.
    int cnt = k;  // __counted_by on a local can't name a parameter
    cnvs_vec2 *__counted_by(cnt) dst = v->data + v->len;
    int i = 0;
    for (; i + 8 <= cnt; i += 8) {  // a stroke block stages 36-48 verts
        cnvs_geom_f16 q;
        memcpy(&q, src + i, sizeof q);
        memcpy(dst + i, &q, sizeof q);
    }
    for (; i + 4 <= cnt; i += 4) {
        cnvs_geom_f8 q;
        memcpy(&q, src + i, sizeof q);
        memcpy(dst + i, &q, sizeof q);
    }
    for (; i < cnt; i++) {
        dst[i] = src[i];
    }
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
