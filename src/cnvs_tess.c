#include "cnvs_tess.h"

static gpu_vert vp(cnvs_vec2 p) {
    return (gpu_vert){ .x = p.x, .y = p.y };
}

// Twice the signed area of triangle (a, b, c); > 0 for a CCW turn.
static float turn(cnvs_vec2 a, cnvs_vec2 b, cnvs_vec2 c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Strictly inside test (boundary counts as outside, so ears with collinear
// neighbours are still clippable).
static bool in_tri(cnvs_vec2 pt, cnvs_vec2 a, cnvs_vec2 b, cnvs_vec2 c) {
    float d1 = turn(pt, a, b);
    float d2 = turn(pt, b, c);
    float d3 = turn(pt, c, a);
    bool any_neg = (d1 < 0.0f) || (d2 < 0.0f) || (d3 < 0.0f);
    bool any_pos = (d1 > 0.0f) || (d2 > 0.0f) || (d3 > 0.0f);
    return !(any_neg && any_pos);
}

static float poly_area2(const cnvs_vec2 *__counted_by(n) poly, int n) {
    float s = 0.0f;
    for (int i = 0; i < n; i++) {
        cnvs_vec2 a = poly[i];
        cnvs_vec2 b = poly[(i + 1) % n];
        s += a.x * b.y - b.x * a.y;
    }
    return s;
}

bool cnvs_tess_polygon(const cnvs_vec2 *__counted_by(n) poly, int n,
                       cnvs_verts *out, cnvs_ints *scratch) {
    if (n < 3) {
        return true;
    }

    // Build a CCW-ordered index ring.
    cnvs_ints_reset(scratch);
    if (poly_area2(poly, n) >= 0.0f) {
        for (int i = 0; i < n; i++) {
            if (!cnvs_ints_push(scratch, i)) {
                return false;
            }
        }
    } else {
        for (int i = n - 1; i >= 0; i--) {
            if (!cnvs_ints_push(scratch, i)) {
                return false;
            }
        }
    }

    int guard = 0;
    int guard_max = n * n + 16;
    while (scratch->len > 3 && guard < guard_max) {
        guard += 1;
        bool clipped = false;
        for (int i = 0; i < scratch->len; i++) {
            int prev = (i + scratch->len - 1) % scratch->len;
            int next = (i + 1) % scratch->len;
            int ia = scratch->data[prev];
            int ib = scratch->data[i];
            int ic = scratch->data[next];
            cnvs_vec2 a = poly[ia];
            cnvs_vec2 b = poly[ib];
            cnvs_vec2 c = poly[ic];
            if (turn(a, b, c) <= 0.0f) {
                continue;  // reflex or collinear -- not an ear tip
            }
            bool empty = true;
            for (int j = 0; j < scratch->len; j++) {
                int ij = scratch->data[j];
                if (ij == ia || ij == ib || ij == ic) {
                    continue;
                }
                if (in_tri(poly[ij], a, b, c)) {
                    empty = false;
                    break;
                }
            }
            if (!empty) {
                continue;
            }
            if (!cnvs_verts_tri(out, vp(a), vp(b), vp(c))) {
                return false;
            }
            cnvs_ints_remove(scratch, i);
            clipped = true;
            break;
        }
        if (!clipped) {
            break;  // numerical fallback: fan the remainder below
        }
    }

    // Final triangle, or fan fallback for a remainder we could not ear-clip.
    for (int i = 1; i + 1 < scratch->len; i++) {
        cnvs_vec2 a = poly[scratch->data[0]];
        cnvs_vec2 b = poly[scratch->data[i]];
        cnvs_vec2 c = poly[scratch->data[i + 1]];
        if (!cnvs_verts_tri(out, vp(a), vp(b), vp(c))) {
            return false;
        }
    }
    return true;
}
