#include "cnvs_stroke.h"

#include <math.h>

static gpu_vert vadd(cnvs_vec2 p, cnvs_vec2 d) {
    return (gpu_vert){ .x = p.x + d.x, .y = p.y + d.y };
}

static gpu_vert vsub(cnvs_vec2 p, cnvs_vec2 d) {
    return (gpu_vert){ .x = p.x - d.x, .y = p.y - d.y };
}

// Outward normal of segment p0->p1 scaled to half_width; false if degenerate.
static bool seg_normal(cnvs_vec2 p0, cnvs_vec2 p1, float hw, cnvs_vec2 *out_n) {
    float dx = p1.x - p0.x;
    float dy = p1.y - p0.y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-6f) {
        return false;
    }
    float s = hw / len;
    *out_n = (cnvs_vec2){ .x = -dy * s, .y = dx * s };
    return true;
}

static bool emit_quad(cnvs_verts *out, cnvs_vec2 p0, cnvs_vec2 p1, cnvs_vec2 nrm) {
    gpu_vert a0 = vadd(p0, nrm);
    gpu_vert b0 = vsub(p0, nrm);
    gpu_vert a1 = vadd(p1, nrm);
    gpu_vert b1 = vsub(p1, nrm);
    return cnvs_verts_tri(out, a0, b0, b1) && cnvs_verts_tri(out, a0, b1, a1);
}

static bool emit_join(cnvs_verts *out, cnvs_vec2 v,
                      cnvs_vec2 n_prev, cnvs_vec2 n_next) {
    gpu_vert c = { .x = v.x, .y = v.y };
    return cnvs_verts_tri(out, vadd(v, n_prev), c, vadd(v, n_next)) &&
           cnvs_verts_tri(out, vsub(v, n_prev), c, vsub(v, n_next));
}

bool cnvs_stroke_polyline(cnvs_vec2 const *__counted_by(n) pts, int n,
                          bool closed, float half_width, cnvs_verts *out) {
    if (n < 2 || half_width <= 0.0f) {
        return true;
    }
    int nseg = closed ? n : n - 1;
    bool have_prev = false;
    bool have_first = false;
    cnvs_vec2 prev_n = { .x = 0.0f, .y = 0.0f };
    cnvs_vec2 first_n = { .x = 0.0f, .y = 0.0f };

    for (int s = 0; s < nseg; s++) {
        cnvs_vec2 p0 = pts[s];
        cnvs_vec2 p1 = pts[(s + 1) % n];
        cnvs_vec2 nrm;
        if (!seg_normal(p0, p1, half_width, &nrm)) {
            continue;
        }
        if (!emit_quad(out, p0, p1, nrm)) {
            return false;
        }
        if (have_prev && !emit_join(out, p0, prev_n, nrm)) {
            return false;
        }
        if (!have_first) {
            first_n = nrm;
            have_first = true;
        }
        prev_n = nrm;
        have_prev = true;
    }

    // Closing join at the wrap vertex.
    if (closed && have_first && have_prev &&
        !emit_join(out, pts[0], prev_n, first_n)) {
        return false;
    }
    return true;
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

    int nseg = closed ? n : n - 1;
    for (int s = 0; s < nseg; s++) {
        cnvs_vec2 a = pts[s];
        cnvs_vec2 b = pts[(s + 1) % n];
        float dx = b.x - a.x;
        float dy = b.y - a.y;
        float seglen = sqrtf(dx * dx + dy * dy);
        if (seglen < 1e-6f) {
            continue;
        }
        float inv = 1.0f / seglen;
        cnvs_vec2 dir = { .x = dx * inv, .y = dy * inv };
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
