#include "cnvs_stroke.h"

#include <math.h>

static float const TAU = 6.2831853f;

// Unit direction and length of p0->p1; false if degenerate.
static bool seg_dir(cnvs_vec2 p0, cnvs_vec2 p1, cnvs_vec2 *dir, float *len) {
    float dx = p1.x - p0.x;
    float dy = p1.y - p0.y;
    float l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-6f) {
        return false;
    }
    *dir = (cnvs_vec2){ .x = dx / l, .y = dy / l };
    *len = l;
    return true;
}

// Rectangle from p0 to p1 offset by +/-nrm, as two triangles.
static bool emit_quad(cnvs_verts *out, cnvs_vec2 p0, cnvs_vec2 p1, cnvs_vec2 nrm) {
    cnvs_vec2 a0 = { .x = p0.x + nrm.x, .y = p0.y + nrm.y };
    cnvs_vec2 b0 = { .x = p0.x - nrm.x, .y = p0.y - nrm.y };
    cnvs_vec2 a1 = { .x = p1.x + nrm.x, .y = p1.y + nrm.y };
    cnvs_vec2 b1 = { .x = p1.x - nrm.x, .y = p1.y - nrm.y };
    return cnvs_verts_tri(out, a0, b0, b1) && cnvs_verts_tri(out, a0, b1, a1);
}

static int disc_segs(float r) {
    float rr = r > 0.5f ? r : 0.5f;
    float dtheta = 2.0f * acosf(fmaxf(0.0f, 1.0f - 0.25f / rr));
    if (!(dtheta > 1e-3f)) {
        dtheta = 1e-3f;
    }
    float fs = ceilf(TAU / dtheta);
    int s = cnvs_f2i(fs);
    if (s < 8) {
        s = 8;
    }
    if (s > 64) {
        s = 64;
    }
    return s;
}

// A filled disc as a triangle fan -- serves round joins and round caps (the
// extra coverage over a half-disc lands on already-stroked geometry).
static bool emit_disc(cnvs_verts *out, cnvs_vec2 c, float r) {
    int segs = disc_segs(r);
    cnvs_vec2 prev = { .x = c.x + r, .y = c.y };
    for (int i = 1; i <= segs; i++) {
        float a = TAU * (float)i / (float)segs;
        cnvs_vec2 cur = { .x = c.x + r * cosf(a), .y = c.y + r * sinf(a) };
        if (!cnvs_verts_tri(out, c, prev, cur)) {
            return false;
        }
        prev = cur;
    }
    return true;
}

// Fill the join at vertex v between incoming dir d0 and outgoing dir d1.
static bool emit_join(cnvs_verts *out, cnvs_vec2 v, cnvs_vec2 d0, cnvs_vec2 d1,
                      float hw, cnvs_line_join join, float miter_limit) {
    float cross = d0.x * d1.y - d0.y * d1.x;
    if (cross > -1e-6f && cross < 1e-6f) {
        return true;  // collinear: no gap to fill
    }
    if (join == CNVS_JOIN_ROUND) {
        return emit_disc(out, v, hw);
    }
    // Outer side is opposite the turn.
    float sgn = cross > 0.0f ? -1.0f : 1.0f;
    cnvs_vec2 pa = { .x = v.x + sgn * -d0.y * hw, .y = v.y + sgn * d0.x * hw };
    cnvs_vec2 pb = { .x = v.x + sgn * -d1.y * hw, .y = v.y + sgn * d1.x * hw };
    if (!cnvs_verts_tri(out, pa, v, pb)) {  // bevel wedge
        return false;
    }
    if (join == CNVS_JOIN_MITER) {
        // Miter tip = intersection of the two outer edges (pa,d0) and (pb,d1).
        float s = ((pb.x - pa.x) * d1.y - (pb.y - pa.y) * d1.x) / cross;
        cnvs_vec2 tip = { .x = pa.x + d0.x * s, .y = pa.y + d0.y * s };
        float mx = tip.x - v.x;
        float my = tip.y - v.y;
        if (sqrtf(mx * mx + my * my) <= miter_limit * hw) {
            if (!cnvs_verts_tri(out, pa, tip, pb)) {
                return false;
            }
        }
    }
    return true;
}

// Cap at open end `e`, with `capdir` pointing outward along the line.
static bool emit_cap(cnvs_verts *out, cnvs_vec2 e, cnvs_vec2 capdir, float hw,
                     cnvs_line_cap cap) {
    if (cap == CNVS_CAP_BUTT) {
        return true;
    }
    if (cap == CNVS_CAP_ROUND) {
        return emit_disc(out, e, hw);
    }
    cnvs_vec2 nrm = { .x = -capdir.y * hw, .y = capdir.x * hw };
    cnvs_vec2 e2 = { .x = e.x + capdir.x * hw, .y = e.y + capdir.y * hw };
    return emit_quad(out, e, e2, nrm);  // square: extend by hw
}

bool cnvs_stroke_polyline(cnvs_vec2 const *__counted_by(n) pts, int n, bool closed,
                          float half_width, cnvs_line_join join, cnvs_line_cap cap,
                          float miter_limit, cnvs_verts *out) {
    if (n < 2 || half_width <= 0.0f) {
        return true;
    }
    float hw = half_width;
    int nseg = closed ? n : n - 1;

    bool have_prev = false;
    bool have_first = false;
    cnvs_vec2 prev_dir = { .x = 0.0f, .y = 0.0f };
    cnvs_vec2 first_dir = { .x = 0.0f, .y = 0.0f };
    cnvs_vec2 first_pt = { .x = 0.0f, .y = 0.0f };
    cnvs_vec2 last_pt = { .x = 0.0f, .y = 0.0f };

    for (int s = 0; s < nseg; s++) {
        cnvs_vec2 p0 = pts[s];
        cnvs_vec2 p1 = pts[(s + 1) % n];
        cnvs_vec2 dir;
        float len;
        if (!seg_dir(p0, p1, &dir, &len)) {
            continue;
        }
        cnvs_vec2 nrm = { .x = -dir.y * hw, .y = dir.x * hw };
        if (!emit_quad(out, p0, p1, nrm)) {
            return false;
        }
        if (have_prev && !emit_join(out, p0, prev_dir, dir, hw, join, miter_limit)) {
            return false;
        }
        if (!have_first) {
            first_dir = dir;
            first_pt = p0;
            have_first = true;
        }
        prev_dir = dir;
        last_pt = p1;
        have_prev = true;
    }
    if (!have_first) {
        return true;
    }

    if (closed) {
        return emit_join(out, pts[0], prev_dir, first_dir, hw, join, miter_limit);
    }
    cnvs_vec2 back = { .x = -first_dir.x, .y = -first_dir.y };
    return emit_cap(out, first_pt, back, hw, cap) &&
           emit_cap(out, last_pt, prev_dir, hw, cap);
}

bool cnvs_stroke_dashed(cnvs_vec2 const *__counted_by(n) pts, int n, bool closed,
                        float half_width, float const *__counted_by(ndash) dash,
                        int ndash, float dash_offset, cnvs_verts *out) {
    if (n < 2 || half_width <= 0.0f || ndash <= 0) {
        return true;
    }
    float total = 0.0f;
    for (int i = 0; i < ndash; i++) {
        total += dash[i];
    }
    if (!(total > 0.0f)) {
        return true;
    }

    // Place the dash cursor at the offset: index `di`, `remain` left in it, on/off.
    float phase = fmodf(dash_offset, total);
    if (phase < 0.0f) {
        phase += total;
    }
    int di = 0;
    while (di < ndash && phase >= dash[di]) {
        phase -= dash[di];
        di += 1;
    }
    if (di >= ndash) {
        di = ndash - 1;
    }
    float remain = dash[di] - phase;
    bool on = (di % 2) == 0;

    // A pathological dash (sub-pixel lengths, or a path whose transformed length
    // is enormous) would iterate the inner loop unboundedly and emit a quad per
    // span -- a memory/CPU DoS (a ~2GB vertex buffer was reachable this way).
    // Cap the total spans; past it the dashing carries no visual information
    // anyway, so truncating is the safe, graceful choice.
    int spans = 0;
    int const span_cap = 1 << 20;

    int nseg = closed ? n : n - 1;
    for (int s = 0; s < nseg; s++) {
        cnvs_vec2 a = pts[s];
        cnvs_vec2 b = pts[(s + 1) % n];
        cnvs_vec2 dir;
        float seglen;
        if (!seg_dir(a, b, &dir, &seglen)) {
            continue;
        }
        cnvs_vec2 nrm = { .x = -dir.y * half_width, .y = dir.x * half_width };

        float pos = 0.0f;
        while (pos < seglen) {
            float step = remain;
            if (step > seglen - pos) {
                step = seglen - pos;
            }
            if (on) {
                cnvs_vec2 p0 = { .x = a.x + dir.x * pos, .y = a.y + dir.y * pos };
                cnvs_vec2 p1 = { .x = a.x + dir.x * (pos + step),
                                 .y = a.y + dir.y * (pos + step) };
                if (!emit_quad(out, p0, p1, nrm)) {
                    return false;
                }
            }
            pos += step;
            remain -= step;
            if (++spans > span_cap) {
                return true;  // pathological dash: stop after a bounded amount
            }
            if (remain <= 1e-6f) {
                do {
                    di = (di + 1) % ndash;
                    remain = dash[di];
                    on = !on;
                } while (remain <= 0.0f);  // skip zero-length entries (total > 0)
            }
        }
    }
    return true;
}
