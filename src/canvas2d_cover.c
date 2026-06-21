#include "canvas2d_cover.h"

#include "canvas2d_math.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void canvas2d_cover_free(struct canvas2d_cover *c) {
    free(c->acc);
    c->acc = NULL;
    c->cap = 0;
}

bool canvas2d_cover_reset(struct canvas2d_cover *c, int w, int h) {
    int const need = w * h;
    if (need > c->cap) {
        float *na = realloc(c->acc, (size_t)need * sizeof *na);
        if (!na) {
            return false;
        }
        c->acc = na;
        c->cap = need;
    }
    memset(c->acc, 0, (size_t)need * sizeof *c->acc);  // 0.0f is all-zero bits
    return true;
}

// Deposit one cell of signed cover: the edge crosses column `col` at fraction
// `xmf` in [0,1], leaving cover `d` to its right (the rest propagates via the
// prefix sum).  Left of the raster, full cover lumps into column 0; at or past
// the right edge, nothing lands on-screen.  always_inline: it's the innermost
// step of the per-edge loop and -Os otherwise leaves it out-of-line.
__attribute__((always_inline))
static inline void deposit(struct canvas2d_cover *c, int base, int w, int col, float xmf, float d) {
    if (col < 0) {
        c->acc[base] += d;
    } else if (col < w) {
        c->acc[base + col] += d * (1.0f - xmf);
        if (col + 1 < w) {
            c->acc[base + col + 1] += d * xmf;
        }
    }
}

// Accumulate a segment that lies within a single scanline row `y`: it runs from
// (xs, ys) to (xe, ye) with ys < ye, both in [y, y+1].
static void accum_row(struct canvas2d_cover *c, int w, int y,
                      float xs, float ys, float xe, float ye, float dir) {
    int const base = y * w;
    float const dyt = ye - ys;
    if (dyt <= 0.0f) {
        return;
    }
    float xlo = xs < xe ? xs : xe;
    float xhi = xs < xe ? xe : xs;

    if (xhi - xlo < 1e-7f) {  // vertical within the row: all cover in one column
        float const colf = floorf(xlo);
        deposit(c, base, w, canvas2d_f2i(colf), xlo - colf, dir * dyt);
        return;
    }

    float const dydx = dyt / (xhi - xlo);  // dy per unit x (both spans positive)

    // The part left of the raster contributes full cover to column 0.
    if (xlo < 0.0f) {
        float const clipx = xhi < 0.0f ? xhi : 0.0f;
        c->acc[base] += dir * (clipx - xlo) * dydx;
        if (xhi <= 0.0f) {
            return;
        }
        xlo = 0.0f;
    }
    // The part at/right of the raster is left of the edge: no on-screen cover.
    if (xhi > (float)w) {
        if (xlo >= (float)w) {
            return;
        }
        xhi = (float)w;
    }

    float const clof = floorf(xlo);
    float const chif = floorf(xhi);
    int const clo = canvas2d_f2i(clof);
    int const chi = canvas2d_f2i(chif);

    if (clo == chi) {  // the whole span lies in one column
        // Area from the in-raster width, NOT dir*dyt: when the segment was
        // clipped above, the off-raster part of dyt is already lumped into
        // column 0 (left) or dropped (right).
        deposit(c, base, w, clo, 0.5f * (xlo + xhi) - clof, dir * (xhi - xlo) * dydx);
        return;
    }

    // First partial column [xlo, clo+1).
    deposit(c, base, w, clo, 0.5f * (xlo + clof + 1.0f) - clof,
            dir * (clof + 1.0f - xlo) * dydx);
    // Last partial column [chi, xhi); zero-width when xhi sits on the boundary.
    if (chi < w && xhi > chif) {
        deposit(c, base, w, chi, 0.5f * (xhi - chif), dir * (xhi - chif) * dydx);
    }

    // Interior columns in (clo, chi) each cover a full unit of x -- the same
    // signed area d, half deposited in their own cell, half spilled right.
    // Telescoped along the run, that's +d/2 at the run's two outer cells and a
    // constant +d on every cell between: a contiguous add, done 8-wide with one
    // whole-vector bounds check per block (the resolve's memcpy idiom) instead
    // of two scattered checked writes per column.
    if (chi - clo >= 2) {
        float const d = dir * dydx;
        c->acc[base + clo + 1] += 0.5f * d;
        if (chi < w) {
            c->acc[base + chi] += 0.5f * d;
        }
        int x = clo + 2;
        for (; x + 8 <= chi; x += 8) {
            f32x8 v;
            memcpy(&v, c->acc + base + x, sizeof v);  // bounds-checked vector load
            v += d;
            memcpy(c->acc + base + x, &v, sizeof v);  // bounds-checked vector store
        }
        for (; x < chi; x++) {
            c->acc[base + x] += d;
        }
    }
}

void canvas2d_cover_add_edge(struct canvas2d_cover *c, int w, int h,
                         float x0, float y0, float x1, float y1) {
    float dir, xa, ya, xb, yb;
    if (y0 < y1) {
        dir =  1.0f;
        xa = x0; ya = y0; xb = x1; yb = y1;
    } else if (y0 > y1) {
        dir = -1.0f;
        xa = x1; ya = y1; xb = x0; yb = y0;
    } else {
        return;  // horizontal: crosses no scanline
    }
    if (yb <= 0.0f || ya >= (float)h) {
        return;
    }
    float const dxdy = (xb - xa) / (yb - ya);
    if (ya < 0.0f) {  // clip to the top of the raster
        xa += dxdy * (0.0f - ya);
        ya = 0.0f;
    }
    if (yb > (float)h) {  // clip to the bottom
        xb += dxdy * ((float)h - yb);
        yb = (float)h;
    }
    float const row0f = floorf(ya);
    float const row1f = ceilf(yb);
    int const row0 = canvas2d_f2i(row0f);
    int const row1 = canvas2d_f2i(row1f);
    for (int y = row0; y < row1 && y < h; y++) {
        float const rtop = ya > (float)y ? ya : (float)y;
        float const rbot = yb < (float)(y + 1) ? yb : (float)(y + 1);
        if (rbot <= rtop) {
            continue;
        }
        accum_row(c, w, y, xa + dxdy * (rtop - ya), rtop,
                  xa + dxdy * (rbot - ya), rbot, dir);
    }
}

// Fold an accumulated winding value to 8-bit coverage under the fill rule.
static uint8_t cover_to_u8(enum canvas2d_fill_rule rule, float run) {
    float cov;
    if (rule == CANVAS2D_EVENODD) {
        // Triangle wave of period 2: fold the winding count to coverage without
        // fmodf's libm call (floorf lowers to a single frintm).
        float const t = run * 0.5f;
        float const m = (t - floorf(t)) * 2.0f;  // in [0, 2) for any sign of run
        cov = m > 1.0f ? 2.0f - m : m;
    } else {
        cov = fabsf(run);
        if (cov > 1.0f) {
            cov = 1.0f;
        }
    }
    return canvas2d_f2u8(cov * 255.0f + 0.5f);
}

// Coverage-fold a vector of 8 winding values to 0..255, matching cover_to_u8 lane
// by lane.  run values are finite (a prefix sum of finite areas), so the saturating
// guards in canvas2d_f2u8 reduce to a [0,255] clamp the convert handles by construction.
static u8x8 cover_to_u8x8(enum canvas2d_fill_rule rule, f32x8 run) {
    f32x8 cov;
    if (rule == CANVAS2D_EVENODD) {
        f32x8 const t = run * 0.5f;
        f32x8 const m = (t - __builtin_elementwise_floor(t)) * 2.0f;  // [0, 2)
        cov = __builtin_elementwise_min(m, 2.0f - m);           // == m>1 ? 2-m : m, bit-exact
    } else {
        cov = __builtin_elementwise_min((f32x8)1.0f, __builtin_elementwise_abs(run));
    }
    f32x8 const v = cov * 255.0f + 0.5f;  // in [0.5, 255.5]; truncating the convert rounds
    return __builtin_convertvector(v, u8x8);
}

// Inclusive prefix sum across the 8 lanes, in-register (Hillis-Steele: 3
// shift-add steps, each adding the lane d positions back, low lanes
// zero-filled).  The tree association differs from a left-to-right scalar
// sum, so the float rounding differs by <=1 ULP before the 8-bit quantize.
static inline f32x8 prefix_sum8(f32x8 v) {
    f32x8 const z = (f32x8){ 0, 0, 0, 0, 0, 0, 0, 0 };
    v += __builtin_shufflevector(v, z, 8, 0, 1, 2, 3, 4, 5, 6);  // += lane-1 (zero-fill)
    v += __builtin_shufflevector(v, z, 8, 8, 0, 1, 2, 3, 4, 5);  // += lane-2
    v += __builtin_shufflevector(v, z, 8, 8, 8, 8, 0, 1, 2, 3);  // += lane-4
    return v;
}

void canvas2d_cover_resolve(struct canvas2d_cover *c, int w, int h, enum canvas2d_fill_rule rule,
                        uint8_t *__counted_by(w * h) out) {
    // Fold each row's signed-area deltas to coverage in one pass: an 8-wide
    // in-register prefix sum, plus a running scalar carry from earlier
    // blocks, feeds the coverage fold + 8-bit convert directly.
    for (int y = 0; y < h; y++) {
        int const base = y * w;
        float carry = 0.0f;  // running total of all deltas before this block, this row
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            f32x8 v;
            memcpy(&v, c->acc + base + x, sizeof v);  // bounds-checked vector load
            v = prefix_sum8(v) + carry;               // inclusive prefix sum + carry-in
            u8x8 b = cover_to_u8x8(rule, v);
            memcpy(out + base + x, &b, sizeof b);     // bounds-checked vector store
            carry = v[7];                             // running total through this block
        }
        float run = carry;
        for (; x < w; x++) {  // scalar tail: continue the running sum
            run += c->acc[base + x];
            out[base + x] = cover_to_u8(rule, run);
        }
    }
}
