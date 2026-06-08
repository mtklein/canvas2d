#include "cnvs_cover.h"

#include <math.h>
#include <stdlib.h>

void cnvs_cover_free(cnvs_cover *c) {
    free(c->acc);
    c->acc = NULL;
    c->cap = 0;
}

bool cnvs_cover_reset(cnvs_cover *c, int w, int h) {
    int need = w * h;
    if (need > c->cap) {
        float *na = realloc(c->acc, (size_t)need * sizeof *na);
        if (!na) {
            return false;
        }
        c->acc = na;            // pointer and its count updated together
        c->cap = need;
    }
    for (int i = 0; i < need; i++) {
        c->acc[i] = 0.0f;
    }
    return true;
}

// Deposit one cell's worth of signed cover: the edge passes through column
// `col` at fractional position `xmf` in [0,1], leaving cover `d` to its right.
// The cell it sits in gets the right fraction; the rest propagates via the
// prefix sum.  x left of the raster lumps full cover into column 0; x at or past
// the right edge contributes nothing on-screen.
static void deposit(cnvs_cover *c, int base, int w, int col, float xmf, float d) {
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
static void accum_row(cnvs_cover *c, int w, int y,
                      float xs, float ys, float xe, float ye, float dir) {
    int base = y * w;
    float dyt = ye - ys;
    if (dyt <= 0.0f) {
        return;
    }
    float xlo = xs < xe ? xs : xe;
    float xhi = xs < xe ? xe : xs;

    if (xhi - xlo < 1e-7f) {  // vertical within the row: all cover in one column
        float colf = floorf(xlo);
        deposit(c, base, w, (int)colf, xlo - colf, dir * dyt);
        return;
    }

    float dydx = dyt / (xhi - xlo);  // dy per unit x (both spans positive)

    // The part left of the raster contributes full cover to column 0.
    if (xlo < 0.0f) {
        float clipx = xhi < 0.0f ? xhi : 0.0f;
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

    float clof = floorf(xlo);
    float chif = floorf(xhi);
    int clo = (int)clof;
    int chi = (int)chif;
    for (int col = clo; col <= chi && col < w; col++) {
        float a = (float)col > xlo ? (float)col : xlo;
        float b = (float)(col + 1) < xhi ? (float)(col + 1) : xhi;
        if (b <= a) {
            continue;
        }
        deposit(c, base, w, col, 0.5f * (a + b) - (float)col, dir * (b - a) * dydx);
    }
}

void cnvs_cover_add_edge(cnvs_cover *c, int w, int h,
                         float x0, float y0, float x1, float y1) {
    float dir, xa, ya, xb, yb;
    if (y0 < y1) {
        dir = 1.0f;
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
    float dxdy = (xb - xa) / (yb - ya);
    if (ya < 0.0f) {  // clip to the top of the raster
        xa += dxdy * (0.0f - ya);
        ya = 0.0f;
    }
    if (yb > (float)h) {  // clip to the bottom
        xb += dxdy * ((float)h - yb);
        yb = (float)h;
    }
    float row0f = floorf(ya);
    float row1f = ceilf(yb);
    int row0 = (int)row0f;
    int row1 = (int)row1f;
    for (int y = row0; y < row1 && y < h; y++) {
        float rtop = ya > (float)y ? ya : (float)y;
        float rbot = yb < (float)(y + 1) ? yb : (float)(y + 1);
        if (rbot <= rtop) {
            continue;
        }
        accum_row(c, w, y, xa + dxdy * (rtop - ya), rtop,
                  xa + dxdy * (rbot - ya), rbot, dir);
    }
}

void cnvs_cover_resolve(cnvs_cover const *c, int w, int h, cnvs_fill_rule rule,
                        uint8_t *__counted_by(w * h) out) {
    for (int y = 0; y < h; y++) {
        int base = y * w;
        float run = 0.0f;
        for (int x = 0; x < w; x++) {
            run += c->acc[base + x];
            float cov;
            if (rule == CNVS_EVENODD) {
                float m = fmodf(run, 2.0f);
                if (m < 0.0f) {
                    m += 2.0f;
                }
                cov = m > 1.0f ? 2.0f - m : m;
            } else {
                cov = fabsf(run);
                if (cov > 1.0f) {
                    cov = 1.0f;
                }
            }
            out[base + x] = (uint8_t)(cov * 255.0f + 0.5f);
        }
    }
}
