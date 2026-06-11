#include "cnvs_path.h"

#include "cnvs_mem.h"

#include <math.h>
#include <stdlib.h>

void cnvs_path_init(struct cnvs_path *p) {
    p->pts = NULL;
    p->npts = 0;
    p->pt_cap = 0;
    p->subs = NULL;
    p->nsubs = 0;
    p->sp_cap = 0;
    p->has_cur = false;
    p->cur = (cnvs_vec2){ .x = 0.0f, .y = 0.0f };
}

void cnvs_path_free(struct cnvs_path *p) {
    free(p->pts);
    free(p->subs);
    cnvs_path_init(p);
}

void cnvs_path_reset(struct cnvs_path *p) {
    p->npts = 0;
    p->nsubs = 0;
    p->has_cur = false;
}

static bool pts_reserve(struct cnvs_path *p, int need) {
    if (need <= p->pt_cap) {
        return true;
    }
    int const newcap = cnvs_grow_cap(p->pt_cap, need);
    cnvs_vec2 *nd = realloc(p->pts, (size_t)newcap * sizeof *nd);
    if (!nd) {
        return false;
    }
    p->pts = nd;
    p->pt_cap = newcap;
    return true;
}

static bool subs_reserve(struct cnvs_path *p, int need) {
    if (need <= p->sp_cap) {
        return true;
    }
    int const newcap = cnvs_grow_cap(p->sp_cap, need);
    cnvs_subpath *nd = realloc(p->subs, (size_t)newcap * sizeof *nd);
    if (!nd) {
        return false;
    }
    p->subs = nd;
    p->sp_cap = newcap;
    return true;
}

bool cnvs_path_move_to(struct cnvs_path *p, cnvs_vec2 pt) {
    if (!subs_reserve(p, p->nsubs + 1) || !pts_reserve(p, p->npts + 1)) {
        return false;
    }
    p->subs[p->nsubs] =
        (cnvs_subpath){ .start = p->npts, .count = 1, .closed = false };
    p->nsubs += 1;
    p->pts[p->npts] = pt;
    p->npts += 1;
    p->has_cur = true;
    p->cur = pt;
    return true;
}

bool cnvs_path_line_to(struct cnvs_path *p, cnvs_vec2 pt) {
    if (!p->has_cur) {
        return cnvs_path_move_to(p, pt);
    }
    if (!pts_reserve(p, p->npts + 1)) {
        return false;
    }
    p->pts[p->npts] = pt;
    p->npts += 1;
    p->subs[p->nsubs - 1].count += 1;
    p->cur = pt;
    return true;
}

bool cnvs_path_close(struct cnvs_path *p) {
    if (p->nsubs > 0) {
        cnvs_subpath const s = p->subs[p->nsubs - 1];
        p->subs[p->nsubs - 1].closed = true;
        if (s.count > 0) {
            p->cur = p->pts[s.start];
        }
        p->has_cur = false;  // a following line_to begins a fresh subpath
    }
    return true;
}

bool cnvs_path_rect(struct cnvs_path *p,
                    cnvs_vec2 a, cnvs_vec2 b, cnvs_vec2 c, cnvs_vec2 d) {
    if (!subs_reserve(p, p->nsubs + 1) || !pts_reserve(p, p->npts + 4)) {
        return false;
    }
    int start = p->npts;
    p->pts[p->npts] = a;
    p->pts[p->npts + 1] = b;
    p->pts[p->npts + 2] = c;
    p->pts[p->npts + 3] = d;
    p->npts += 4;
    p->subs[p->nsubs] =
        (cnvs_subpath){ .start = start, .count = 4, .closed = true };
    p->nsubs += 1;
    p->has_cur = true;
    p->cur = a;
    return true;
}

static cnvs_vec2 mid(cnvs_vec2 a, cnvs_vec2 b) {
    return (cnvs_vec2){ .x = (a.x + b.x) * 0.5f, .y = (a.y + b.y) * 0.5f };
}

// Squared perpendicular distance from c to the line through a and b, times the
// squared chord length (lets us compare against tol^2 without a divide).
static float cross_chord2(cnvs_vec2 a, cnvs_vec2 b, cnvs_vec2 c) {
    float const dx = b.x - a.x;
    float const dy = b.y - a.y;
    float const ex = c.x - a.x;
    float const ey = c.y - a.y;
    float const cross = ex * dy - ey * dx;
    return cross * cross;
}

static float chord_len2(cnvs_vec2 a, cnvs_vec2 b) {
    float const dx = b.x - a.x;
    float const dy = b.y - a.y;
    return dx * dx + dy * dy;
}

// The flatness tests are written !(error > tolerance) rather than
// (error <= tolerance) so that NON-FINITE geometry counts as flat: with an
// inf/NaN control point every comparison is false, and the (a <= b) form
// would subdivide all the way to the depth cap -- 2^16 (2^18 for cubics)
// emitted points PER CURVE, which a text line full of curves multiplies into
// a multi-GB path.  Identical behavior for finite inputs; degenerate curves
// emit one segment, like any flat curve.
static bool quad_rec(struct cnvs_path *p, cnvs_vec2 p0, cnvs_vec2 p1, cnvs_vec2 p2,
                     float tol2, int depth) {
    if (depth >= 16 || !(cross_chord2(p0, p2, p1) > tol2 * chord_len2(p0, p2))) {
        return cnvs_path_line_to(p, p2);
    }
    cnvs_vec2 const p01 = mid(p0, p1);
    cnvs_vec2 const p12 = mid(p1, p2);
    cnvs_vec2 const m = mid(p01, p12);
    return quad_rec(p, p0, p01, m, tol2, depth + 1) &&
           quad_rec(p, m, p12, p2, tol2, depth + 1);
}

bool cnvs_path_quad_to(struct cnvs_path *p, cnvs_vec2 ctrl, cnvs_vec2 end, float tol) {
    if (!p->has_cur && !cnvs_path_move_to(p, ctrl)) {
        return false;
    }
    return quad_rec(p, p->cur, ctrl, end, tol * tol, 0);
}

static bool cubic_rec(struct cnvs_path *p, cnvs_vec2 p0, cnvs_vec2 p1, cnvs_vec2 p2,
                      cnvs_vec2 p3, float tol2, int depth) {
    float const d = cross_chord2(p0, p3, p1) + cross_chord2(p0, p3, p2);
    if (depth >= 18 || !(d > tol2 * chord_len2(p0, p3))) {  // !(>): NaN is
        return cnvs_path_line_to(p, p3);                    // flat (see quad_rec)
    }
    cnvs_vec2 const p01 = mid(p0, p1);
    cnvs_vec2 const p12 = mid(p1, p2);
    cnvs_vec2 const p23 = mid(p2, p3);
    cnvs_vec2 const p012 = mid(p01, p12);
    cnvs_vec2 const p123 = mid(p12, p23);
    cnvs_vec2 const m = mid(p012, p123);
    return cubic_rec(p, p0, p01, p012, m, tol2, depth + 1) &&
           cubic_rec(p, m, p123, p23, p3, tol2, depth + 1);
}

bool cnvs_path_cubic_to(struct cnvs_path *p, cnvs_vec2 c1, cnvs_vec2 c2,
                        cnvs_vec2 end, float tol) {
    if (!p->has_cur && !cnvs_path_move_to(p, c1)) {
        return false;
    }
    return cubic_rec(p, p->cur, c1, c2, end, tol * tol, 0);
}
