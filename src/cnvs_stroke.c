#include "cnvs_stroke.h"

#include <math.h>
#include <string.h>

static float const TAU = 6.2831853f;

// Planar (SoA) segment batches: x- and y-planes of four segments' directions
// and normals, so the per-segment normalize (one sqrt and two divides) runs
// four lanes per instruction.  Each lane computes the SAME operations in the
// SAME order as the scalar path -- batching across lanes, never reassociating
// within one -- so every emitted vertex is bit-identical and the
// order-sensitive coverage sums downstream can't tell the difference.
// Lane values the scalar join/bookkeeping code needs are spilled ONCE per
// block into small arrays: a variable-index vector subscript makes clang
// round-trip the whole register through the stack at every access.

// Unit direction and length of p0->p1; false if degenerate.
static bool seg_dir(cnvs_vec2 p0, cnvs_vec2 p1, cnvs_vec2 *dir, float *len) {
    float dx = p1.x - p0.x;
    float dy = p1.y - p0.y;
    float l = sqrtf(dx * dx + dy * dy);
    *dir = (cnvs_vec2){ .x = dx / l, .y = dy / l };
    *len = l;
    return !(l < 1e-6f);
}

// The quad's two triangles, staged: (a0,b0,b1) (a0,b1,a1).
static void stage_quad(cnvs_vec2 *__counted_by(6) stage, cnvs_vec2 a0, cnvs_vec2 b0,
                       cnvs_vec2 a1, cnvs_vec2 b1) {
    stage[0] = a0;
    stage[1] = b0;
    stage[2] = b1;
    stage[3] = a0;
    stage[4] = b1;
    stage[5] = a1;
}

// Rectangle from p0 to p1 offset by +/-nrm, as two triangles.
static bool emit_quad(cnvs_verts *out, cnvs_vec2 p0, cnvs_vec2 p1, cnvs_vec2 nrm) {
    cnvs_vec2 a0 = { .x = p0.x + nrm.x, .y = p0.y + nrm.y };
    cnvs_vec2 b0 = { .x = p0.x - nrm.x, .y = p0.y - nrm.y };
    cnvs_vec2 a1 = { .x = p1.x + nrm.x, .y = p1.y + nrm.y };
    cnvs_vec2 b1 = { .x = p1.x - nrm.x, .y = p1.y - nrm.y };
    cnvs_vec2 stage[6];
    stage_quad(stage, a0, b0, a1, b1);
    return cnvs_verts_append(out, stage, 6);
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
// extra coverage over a half-disc lands on already-stroked geometry).  The
// whole fan (at most 64 triangles) is staged and lands as one block.
// Vertices come from a rotation recurrence: sin/cos of the step once, then a
// 2x2 rotation per vertex; the recurrence's drift over the worst-case 64
// segments stays far below the coverage quantizer.
static bool emit_disc(cnvs_verts *out, cnvs_vec2 c, float r) {
    int segs = disc_segs(r);
    cnvs_vec2 stage[3 * 64];
    float const step = TAU / (float)segs;
    float const cs = cosf(step), sn = sinf(step);
    float dx = r, dy = 0.0f;
    cnvs_vec2 prev = { .x = c.x + r, .y = c.y };
    int k = 0;
    for (int i = 1; i <= segs; i++) {
        float const nx = dx * cs - dy * sn;
        float const ny = dx * sn + dy * cs;
        dx = nx;
        dy = ny;
        cnvs_vec2 cur = { .x = c.x + dx, .y = c.y + dy };
        cnvs_vec2 *__counted_by(3) tri = stage + k;
        tri[0] = c;
        tri[1] = prev;
        tri[2] = cur;
        k += 3;
        prev = cur;
    }
    return cnvs_verts_append(out, stage, k);
}

// Stage the bevel wedge (and the miter tip when within the limit) at vertex v
// between incoming dir d0 and outgoing dir d1; `cross` is d0 x d1, nonzero.
// Returns the vertex count staged: 3 or 6.
static int stage_wedge(cnvs_vec2 *__counted_by(6) stage, cnvs_vec2 v, cnvs_vec2 d0,
                       cnvs_vec2 d1, float cross, float hw, enum cnvs_line_join join,
                       float miter_limit) {
    // Outer side is opposite the turn.
    float sgn = cross > 0.0f ? -1.0f : 1.0f;
    cnvs_vec2 pa = { .x = v.x + sgn * -d0.y * hw, .y = v.y + sgn * d0.x * hw };
    cnvs_vec2 pb = { .x = v.x + sgn * -d1.y * hw, .y = v.y + sgn * d1.x * hw };
    stage[0] = pa;  // bevel wedge
    stage[1] = v;
    stage[2] = pb;
    if (join == CNVS_JOIN_MITER) {
        // Miter tip = intersection of the two outer edges (pa,d0) and (pb,d1).
        float s = ((pb.x - pa.x) * d1.y - (pb.y - pa.y) * d1.x) / cross;
        cnvs_vec2 tip = { .x = pa.x + d0.x * s, .y = pa.y + d0.y * s };
        float mx = tip.x - v.x;
        float my = tip.y - v.y;
        if (sqrtf(mx * mx + my * my) <= miter_limit * hw) {
            stage[3] = pa;
            stage[4] = tip;
            stage[5] = pb;
            return 6;
        }
    }
    return 3;
}

// The join at vertex v between incoming dir d0 and outgoing dir d1, staged at
// stage[k..] (wedge joins) or landed directly (round joins -- the staged verts
// flush first so emission order holds).  Returns the new stage cursor, or -1
// on allocation failure.
static int join_at(cnvs_verts *out, cnvs_vec2 *__counted_by(48) stage, int k,
                   cnvs_vec2 v, cnvs_vec2 d0, cnvs_vec2 d1, float hw,
                   enum cnvs_line_join join, float miter_limit) {
    float cross = d0.x * d1.y - d0.y * d1.x;
    if (cross > -1e-6f && cross < 1e-6f) {
        return k;  // collinear: no gap to fill
    }
    if (join == CNVS_JOIN_ROUND) {
        if (!cnvs_verts_append(out, stage, k) || !emit_disc(out, v, hw)) {
            return -1;
        }
        return 0;
    }
    return k + stage_wedge(stage + k, v, d0, d1, cross, hw, join, miter_limit);
}

// Fill the join at vertex v between incoming dir d0 and outgoing dir d1.
static bool emit_join(cnvs_verts *out, cnvs_vec2 v, cnvs_vec2 d0, cnvs_vec2 d1,
                      float hw, enum cnvs_line_join join, float miter_limit) {
    cnvs_vec2 stage[48];
    int k = join_at(out, stage, 0, v, d0, d1, hw, join, miter_limit);
    return k >= 0 && cnvs_verts_append(out, stage, k);
}

// Cap at open end `e`, with `capdir` pointing outward along the line.
static bool emit_cap(cnvs_verts *out, cnvs_vec2 e, cnvs_vec2 capdir, float hw,
                     enum cnvs_line_cap cap) {
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
                          float half_width, enum cnvs_line_join join, enum cnvs_line_cap cap,
                          float miter_limit, cnvs_verts *out) {
    if (n < 2 || half_width <= 0.0f) {
        return true;
    }
    float hw = half_width;
    // A closed loop whose last vertex (nearly) coincides with the first -- e.g. a
    // full-circle arc that returns to its start, where sinf(2*pi) leaves a sub-
    // pixel gap -- would stroke a microscopic closing segment.  That segment is
    // too short to skip (it clears seg_dir's degeneracy cutoff) yet too short to
    // stroke cleanly, so it splits the seam into two bad joins and bites a notch
    // out of the outline.  Drop such trailing duplicates (within 0.01 px -- far
    // below any real vertex spacing, well above the float noise even at the max
    // canvas size) so the real closing chord is stroked instead.  `m` is the
    // working count; pts stays bounded by n (m <= n), so every index is in range.
    int m = n;
    while (closed && m > 2) {
        float dx = pts[m - 1].x - pts[0].x;
        float dy = pts[m - 1].y - pts[0].y;
        if (dx * dx + dy * dy > 1e-4f) {
            break;
        }
        m -= 1;
    }
    int nseg = closed ? m : m - 1;

    bool have_prev = false;
    bool have_first = false;
    cnvs_vec2 prev_dir = { .x = 0.0f, .y = 0.0f };
    cnvs_vec2 first_dir = { .x = 0.0f, .y = 0.0f };
    cnvs_vec2 first_pt = { .x = 0.0f, .y = 0.0f };
    cnvs_vec2 last_pt = { .x = 0.0f, .y = 0.0f };

    // Segments go four per block: two AoS point loads, direction/normal math
    // on x/y planes (see the batching comment above), the four quads transposed
    // back to vertex order with constant-index shuffles, then a scalar pass
    // over the lanes stages each segment's quad and join in emission order and
    // lands the block with one checked append.  The loads want pts[s..s+4]
    // contiguous, so a closed loop's wrapping tail (at most four segments) and
    // short tails take the segment-at-a-time path below -- same expressions,
    // same bits, just unbatched.
    int s = 0;
    while (s < nseg) {
        if (nseg - s >= 4 && s + 4 < m) {
            float8 q0, q1;
            memcpy(&q0, pts + s, sizeof q0);      // one bounds check, 4 points
            memcpy(&q1, pts + s + 1, sizeof q1);  // (x0,y0,x1,y1,...)
            float8 dq = q1 - q0;                 // lane-wise p1 - p0
            float4 dx = __builtin_shufflevector(dq, dq, 0, 2, 4, 6);
            float4 dy = __builtin_shufflevector(dq, dq, 1, 3, 5, 7);
            float4 len = __builtin_elementwise_sqrt(dx * dx + dy * dy);
            // Degenerate lanes divide too (IEEE: huge/inf/NaN, all discarded);
            // the emission pass skips them exactly where seg_dir bails.
            float4 dirx = dx / len;
            float4 diry = dy / len;
            float4 nrmx = -diry * hw;
            float4 nrmy =  dirx * hw;
            // Corners in AoS form: nrm re-interleaved, then p +/- nrm is the
            // same lane-wise fadd/fsub the scalar corner math does.
            float8 nrm = __builtin_shufflevector(nrmx, nrmy, 0, 4, 1, 5, 2, 6, 3, 7);
            float8 za0 = q0 + nrm;
            float8 zb0 = q0 - nrm;
            float8 za1 = q1 + nrm;
            float8 zb1 = q1 - nrm;

            // Transpose each quad's corners into vertex order -- (a0,b0,b1)
            // (a0,b1,a1) -- as three 16-byte stores.  A macro because shuffle
            // indices must be literal constants.
            cnvs_vec2 quads[24];
#define CNVS_PUT_QUAD(i)                                                         \
    do {                                                                         \
        float4 t0 = __builtin_shufflevector(za0, zb0, 2 * (i), 2 * (i) + 1,      \
                                            8 + 2 * (i), 9 + 2 * (i));           \
        float4 t1 = __builtin_shufflevector(zb1, za0, 2 * (i), 2 * (i) + 1,      \
                                            8 + 2 * (i), 9 + 2 * (i));           \
        float4 t2 = __builtin_shufflevector(zb1, za1, 2 * (i), 2 * (i) + 1,      \
                                            8 + 2 * (i), 9 + 2 * (i));           \
        memcpy(quads + 6 * (i), &t0, sizeof t0);                                 \
        memcpy(quads + 6 * (i) + 2, &t1, sizeof t1);                             \
        memcpy(quads + 6 * (i) + 4, &t2, sizeof t2);                             \
    } while (0)
            CNVS_PUT_QUAD(0);
            CNVS_PUT_QUAD(1);
            CNVS_PUT_QUAD(2);
            CNVS_PUT_QUAD(3);
#undef CNVS_PUT_QUAD

            // Spill the lanes the scalar pass reads (see the typedef comment).
            float l4[4], dx4[4], dy4[4];
            cnvs_vec2 pp0[4], pp1[4];
            memcpy(l4, &len, sizeof l4);
            memcpy(dx4, &dirx, sizeof dx4);
            memcpy(dy4, &diry, sizeof dy4);
            memcpy(pp0, &q0, sizeof pp0);
            memcpy(pp1, &q1, sizeof pp1);

            // Wedge joins, four at once.  Lane i joins lane i-1's direction to
            // lane i's; lane 0's incoming direction is the carry from the
            // previous block.  Valid only when no lane is degenerate (a skipped
            // segment would break that adjacency) and the join isn't ROUND (the
            // disc path is trig, not wedge math) -- otherwise the scalar
            // join_at below handles it.  Every expression mirrors stage_wedge
            // term for term, so the lanes are bit-identical to the scalar
            // wedges; degenerate-adjacent garbage lanes (e.g. a first block's
            // lane 0 with no carry yet) are computed and discarded.  Like the
            // quads, the wedges transpose to vertex order in-register; only
            // the two decision masks spill.
            int4 degen = len < (float4)1e-6f;
            bool vjoin = join != CNVS_JOIN_ROUND && !__builtin_reduce_or(degen);
            cnvs_vec2 wedges[24];  // 4 joins x (pa, v, pb, pa, tip, pb)
            int col4[4], ok4[4];
            if (vjoin) {
                float4 p0x = __builtin_shufflevector(q0,   q0,   0, 2, 4, 6);
                float4 p0y = __builtin_shufflevector(q0,   q0,   1, 3, 5, 7);
                float4 d0x = __builtin_shufflevector(dirx, dirx, 0, 0, 1, 2);
                float4 d0y = __builtin_shufflevector(diry, diry, 0, 0, 1, 2);
                d0x[0] = prev_dir.x;  // constant-index insert: stays in-register
                d0y[0] = prev_dir.y;
                float4 crs = d0x * diry - d0y * dirx;
                int4 col = (crs > (float4)-1e-6f) & (crs < (float4)1e-6f);
                int4 pos = crs > (float4)0.0f;
                // Outer side is opposite the turn (bit-exact +/-1 select).
                float4 sgn = (float4)((pos & (int4)(float4)-1.0f) |
                                      (~pos & (int4)(float4)1.0f));
                float4 pax = p0x + sgn * -d0y  * hw;
                float4 pay = p0y + sgn *  d0x  * hw;
                float4 pbx = p0x + sgn * -diry * hw;
                float4 pby = p0y + sgn *  dirx * hw;
                // Miter tip = intersection of the outer edges (pa,d0),(pb,d1).
                float4 sm = ((pbx - pax) * diry - (pby - pay) * dirx) / crs;
                float4 tipx = pax + d0x * sm;
                float4 tipy = pay + d0y * sm;
                float4 mx = tipx - p0x;
                float4 my = tipy - p0y;
                float mlim = miter_limit * hw;
                int4 ok = __builtin_elementwise_sqrt(mx * mx + my * my) <= mlim;
                float8 zpa  = __builtin_shufflevector(pax,  pay,  0, 4, 1, 5, 2, 6, 3, 7);
                float8 zpb  = __builtin_shufflevector(pbx,  pby,  0, 4, 1, 5, 2, 6, 3, 7);
                float8 ztip = __builtin_shufflevector(tipx, tipy, 0, 4, 1, 5, 2, 6, 3, 7);
#define CNVS_PUT_WEDGE(i)                                                        \
    do {                                                                         \
        float4 t0 = __builtin_shufflevector(zpa, q0, 2 * (i), 2 * (i) + 1,       \
                                            8 + 2 * (i), 9 + 2 * (i));           \
        float4 t1 = __builtin_shufflevector(zpb, zpa, 2 * (i), 2 * (i) + 1,      \
                                            8 + 2 * (i), 9 + 2 * (i));           \
        float4 t2 = __builtin_shufflevector(ztip, zpb, 2 * (i), 2 * (i) + 1,     \
                                            8 + 2 * (i), 9 + 2 * (i));           \
        memcpy(wedges + 6 * (i), &t0, sizeof t0);                                \
        memcpy(wedges + 6 * (i) + 2, &t1, sizeof t1);                            \
        memcpy(wedges + 6 * (i) + 4, &t2, sizeof t2);                            \
    } while (0)
                CNVS_PUT_WEDGE(0);
                CNVS_PUT_WEDGE(1);
                CNVS_PUT_WEDGE(2);
                CNVS_PUT_WEDGE(3);
#undef CNVS_PUT_WEDGE
                memcpy(col4, &col, sizeof col4);
                memcpy(ok4, &ok, sizeof ok4);
            }

            cnvs_vec2 stage[48];  // 4 segments x (6 quad verts + 6 join verts)
            int k = 0;
            for (int i = 0; i < 4; i++) {
                if (l4[i] < 1e-6f) {
                    continue;  // degenerate: seg_dir's cutoff, lane form
                }
                memcpy(stage + k, quads + 6 * i, 6 * sizeof *stage);
                k += 6;
                cnvs_vec2 dir = { .x = dx4[i], .y = dy4[i] };
                if (have_prev) {
                    if (vjoin) {
                        if (!col4[i]) {  // collinear: no gap to fill
                            if (join == CNVS_JOIN_MITER && ok4[i]) {
                                memcpy(stage + k, wedges + 6 * i, 6 * sizeof *stage);
                                k += 6;  // bevel wedge + miter tip
                            } else {
                                memcpy(stage + k, wedges + 6 * i, 3 * sizeof *stage);
                                k += 3;  // bevel wedge only
                            }
                        }
                    } else {
                        k = join_at(out, stage, k, pp0[i], prev_dir, dir, hw,
                                    join, miter_limit);
                        if (k < 0) {
                            return false;
                        }
                    }
                }
                if (!have_first) {
                    first_dir = dir;
                    first_pt = pp0[i];
                    have_first = true;
                }
                prev_dir = dir;
                last_pt = pp1[i];
                have_prev = true;
            }
            if (!cnvs_verts_append(out, stage, k)) {
                return false;
            }
            s += 4;
        } else {
            cnvs_vec2 p0 = pts[s];
            cnvs_vec2 p1 = pts[(s + 1) % m];
            cnvs_vec2 dir;
            float len;
            if (seg_dir(p0, p1, &dir, &len)) {
                cnvs_vec2 nrm = { .x = -dir.y * hw, .y = dir.x * hw };
                if (!emit_quad(out, p0, p1, nrm)) {
                    return false;
                }
                if (have_prev &&
                    !emit_join(out, p0, prev_dir, dir, hw, join, miter_limit)) {
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
            s += 1;
        }
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

    // A pathological dash (sub-pixel lengths, or a path whose transformed
    // length is enormous) would iterate the inner loop unboundedly and emit a
    // quad per span -- a memory/CPU DoS.  Cap the total spans; past the cap
    // the dashing carries no visual information anyway, so truncate.
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
