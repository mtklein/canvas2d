#pragma once

// Analytic coverage rasterizer by signed-area accumulation.  Edges of a closed
// path accumulate in device space; resolve() turns the accumulation into per-pixel
// coverage in [0,255] for a w*h raster.
//
// Each edge deposits, into a per-pixel float buffer, the signed fractional area it
// leaves to its right on every row it crosses.  A left-to-right prefix sum per row
// turns that into running winding-weighted coverage, which the fill rule folds to
// [0,1].

#include <ptrcheck.h>
#include <stdint.h>

typedef enum { CNVS_NONZERO, CNVS_EVENODD } cnvs_fill_rule;

typedef struct {
    float *__counted_by(cap) acc;  // w*h signed-area accumulation, reused across calls
    int cap;
} cnvs_cover;

void cnvs_cover_free(cnvs_cover *c);

// Zero the accumulation for a w*h raster (growing the buffer as needed).  False
// only on allocation failure.
bool cnvs_cover_reset(cnvs_cover *c, int w, int h);

// Accumulate one path edge (a device-space line segment).  Horizontal segments
// contribute nothing; segments are clipped to the raster.
void cnvs_cover_add_edge(cnvs_cover *c, int w, int h,
                         float x0, float y0, float x1, float y1);

// Resolve the accumulation to per-pixel coverage (0..255) under `rule`, into
// `out` (length w*h, row-major top-first).  Consumes the accumulator (it is
// rewritten to the per-row prefix sums in place), so reset() before reusing `c`.
void cnvs_cover_resolve(cnvs_cover *c, int w, int h, cnvs_fill_rule rule,
                        uint8_t *__counted_by(w * h) out);
