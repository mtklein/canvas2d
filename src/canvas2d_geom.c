#include "canvas2d_geom.h"

#include "canvas2d_mem.h"

#include <stdlib.h>
#include <string.h>

static bool verts_reserve(struct canvas2d_verts *v, int need) {
    if (need <= v->cap) {
        return true;
    }
    int const newcap = canvas2d_grow_cap(v->cap, need);
    canvas2d_vec2 *nd = realloc(v->data, (size_t)newcap * sizeof *nd);
    if (!nd) {
        return false;
    }
    v->data = nd;
    v->cap = newcap;
    return true;
}

bool canvas2d_verts_append(struct canvas2d_verts *v, canvas2d_vec2 const *__counted_by(k) src, int k) {
    if (k <= 0) {
        return true;
    }
    if (!verts_reserve(v, v->nverts + k)) {
        return false;
    }
    // Blocks are small (a quad stages 6 verts, a join wedge 3), so a variable-
    // size memcpy goes out of line to libc memmove and the call costs more
    // than the copy; the constant-size vector memcpys below inline to one
    // load/store pair each.  The destination converts to a counted local
    // FIRST -- that's the block's one real bounds check; copying through
    // v->data directly would reload data/nverts/cap and recheck every step, since
    // the stores could alias *v.
    int const cnt = k;  // __counted_by on a local can't name a parameter
    canvas2d_vec2 *__counted_by(cnt) dst = v->data + v->nverts;
    int i = 0;
    for (; i + 8 <= cnt; i += 8) {  // a stroke block stages 36-48 verts
        float16 q;
        memcpy(&q, src + i, sizeof q);
        memcpy(dst + i, &q, sizeof q);
    }
    for (; i + 4 <= cnt; i += 4) {
        float8 q;
        memcpy(&q, src + i, sizeof q);
        memcpy(dst + i, &q, sizeof q);
    }
    for (; i < cnt; i++) {
        dst[i] = src[i];
    }
    v->nverts += k;
    return true;
}

void canvas2d_verts_reset(struct canvas2d_verts *v) {
    v->nverts = 0;
}

void canvas2d_verts_free(struct canvas2d_verts *v) {
    free(v->data);
    v->data = NULL;
    v->nverts = 0;
    v->cap = 0;
}

foldv8 mat_apply8(canvas2d_mat m, float8 x, float y) {
    return (foldv8){ .x = m.a * x + m.c * y + m.e,
                     .y = m.b * x + m.d * y + m.f };
}

foldv8 mat_apply8_persp(canvas2d_mat m, float8 x, float y) {
    float8 const u = m.a * x + m.c * y + m.e;
    float8 const v = m.b * x + m.d * y + m.f;
    float8 const w = m.g * x + m.h * y + m.i;
    float8 const inv = (float8)1.0f / w;
    return (foldv8){ .x = u * inv, .y = v * inv };
}
