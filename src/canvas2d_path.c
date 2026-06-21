#include "canvas2d_path.h"

#include "canvas2d_mem.h"

#include <math.h>
#include <stdlib.h>

void canvas2d_path_init(struct canvas2d_path *p) {
    p->pts = NULL;
    p->npts = 0;
    p->pt_cap = 0;
    p->subs = NULL;
    p->nsubs = 0;
    p->sp_cap = 0;
    p->has_cur = false;
    p->cur = (canvas2d_vec2){ .x = 0.0f, .y = 0.0f };
}

void canvas2d_path_free(struct canvas2d_path *p) {
    free(p->pts);
    free(p->subs);
    canvas2d_path_init(p);
}

void canvas2d_path_reset(struct canvas2d_path *p) {
    p->npts = 0;
    p->nsubs = 0;
    p->has_cur = false;
}

static bool pts_reserve(struct canvas2d_path *p, int need) {
    if (need <= p->pt_cap) {
        return true;
    }
    int const newcap = canvas2d_grow_cap(p->pt_cap, need);
    canvas2d_vec2 *nd = realloc(p->pts, (size_t)newcap * sizeof *nd);
    if (!nd) {
        return false;
    }
    p->pts = nd;
    p->pt_cap = newcap;
    return true;
}

static bool subs_reserve(struct canvas2d_path *p, int need) {
    if (need <= p->sp_cap) {
        return true;
    }
    int const newcap = canvas2d_grow_cap(p->sp_cap, need);
    canvas2d_subpath *nd = realloc(p->subs, (size_t)newcap * sizeof *nd);
    if (!nd) {
        return false;
    }
    p->subs = nd;
    p->sp_cap = newcap;
    return true;
}

bool canvas2d_path_move_to(struct canvas2d_path *p, canvas2d_vec2 pt) {
    if (!subs_reserve(p, p->nsubs + 1) || !pts_reserve(p, p->npts + 1)) {
        return false;
    }
    p->subs[p->nsubs] =
        (canvas2d_subpath){ .start = p->npts, .count = 1, .closed = false };
    p->nsubs += 1;
    p->pts[p->npts] = pt;
    p->npts += 1;
    p->has_cur = true;
    p->cur = pt;
    return true;
}

bool canvas2d_path_line_to(struct canvas2d_path *p, canvas2d_vec2 pt) {
    if (!p->has_cur) {
        return canvas2d_path_move_to(p, pt);
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

bool canvas2d_path_close(struct canvas2d_path *p) {
    if (p->nsubs > 0) {
        canvas2d_subpath const s = p->subs[p->nsubs - 1];
        p->subs[p->nsubs - 1].closed = true;
        if (s.count > 0) {
            p->cur = p->pts[s.start];
        }
        p->has_cur = false;  // a following line_to begins a fresh subpath
    }
    return true;
}

bool canvas2d_path_rect(struct canvas2d_path *p,
                    canvas2d_vec2 a, canvas2d_vec2 b, canvas2d_vec2 c, canvas2d_vec2 d) {
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
        (canvas2d_subpath){ .start = start, .count = 4, .closed = true };
    p->nsubs += 1;
    p->has_cur = true;
    p->cur = a;
    return true;
}

static canvas2d_vec2 mid(canvas2d_vec2 a, canvas2d_vec2 b) {
    return (canvas2d_vec2){ .x = (a.x + b.x) * 0.5f, .y = (a.y + b.y) * 0.5f };
}

// Squared perpendicular distance from c to the line through a and b, times the
// squared chord length (lets us compare against tol^2 without a divide).
static float cross_chord2(canvas2d_vec2 a, canvas2d_vec2 b, canvas2d_vec2 c) {
    float const dx = b.x - a.x;
    float const dy = b.y - a.y;
    float const ex = c.x - a.x;
    float const ey = c.y - a.y;
    float const cross = ex * dy - ey * dx;
    return cross * cross;
}

static float chord_len2(canvas2d_vec2 a, canvas2d_vec2 b) {
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
static bool quad_rec(struct canvas2d_path *p, canvas2d_vec2 p0, canvas2d_vec2 p1, canvas2d_vec2 p2,
                     float tol2, int depth) {
    if (depth >= 16 || !(cross_chord2(p0, p2, p1) > tol2 * chord_len2(p0, p2))) {
        return canvas2d_path_line_to(p, p2);
    }
    canvas2d_vec2 const p01 = mid(p0, p1);
    canvas2d_vec2 const p12 = mid(p1, p2);
    canvas2d_vec2 const m = mid(p01, p12);
    return quad_rec(p, p0, p01, m, tol2, depth + 1) &&
           quad_rec(p, m, p12, p2, tol2, depth + 1);
}

bool canvas2d_path_quad_to(struct canvas2d_path *p, canvas2d_vec2 ctrl, canvas2d_vec2 end, float tol) {
    if (!p->has_cur && !canvas2d_path_move_to(p, ctrl)) {
        return false;
    }
    return quad_rec(p, p->cur, ctrl, end, tol * tol, 0);
}

static bool cubic_rec(struct canvas2d_path *p, canvas2d_vec2 p0, canvas2d_vec2 p1, canvas2d_vec2 p2,
                      canvas2d_vec2 p3, float tol2, int depth) {
    float const d = cross_chord2(p0, p3, p1) + cross_chord2(p0, p3, p2);
    if (depth >= 18 || !(d > tol2 * chord_len2(p0, p3))) {  // !(>): NaN is
        return canvas2d_path_line_to(p, p3);                    // flat (see quad_rec)
    }
    canvas2d_vec2 const p01 = mid(p0, p1);
    canvas2d_vec2 const p12 = mid(p1, p2);
    canvas2d_vec2 const p23 = mid(p2, p3);
    canvas2d_vec2 const p012 = mid(p01, p12);
    canvas2d_vec2 const p123 = mid(p12, p23);
    canvas2d_vec2 const m = mid(p012, p123);
    return cubic_rec(p, p0, p01, p012, m, tol2, depth + 1) &&
           cubic_rec(p, m, p123, p23, p3, tol2, depth + 1);
}

bool canvas2d_path_cubic_to(struct canvas2d_path *p, canvas2d_vec2 c1, canvas2d_vec2 c2,
                        canvas2d_vec2 end, float tol) {
    if (!p->has_cur && !canvas2d_path_move_to(p, c1)) {
        return false;
    }
    return cubic_rec(p, p->cur, c1, c2, end, tol * tol, 0);
}
