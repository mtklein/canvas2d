#pragma once

// Scanline rasterizer: fills arbitrary paths under a winding rule, emitting one
// span quad per scanline interval.  Correct for holes and self-intersection
// because winding is evaluated per scanline; runs in device space, so it also
// clips to the canvas.

#include "cnvs_geom.h"
#include "cnvs_path.h"

#include <ptrcheck.h>

typedef enum { CNVS_NONZERO, CNVS_EVENODD } cnvs_fill_rule;

typedef struct {
    float ytop, ybot, x_at_top, dxdy;
    int dir;  // +1 if the segment runs downward in y, else -1
} cnvs_edge;

typedef struct {
    cnvs_edge *__counted_by(cap) data;
    int len;
    int cap;
} cnvs_edges;

typedef struct {
    float x;
    int dir;
} cnvs_xing;

typedef struct {
    cnvs_xing *__counted_by(cap) data;
    int len;
    int cap;
} cnvs_xings;

void cnvs_edges_free(cnvs_edges *e);
void cnvs_xings_free(cnvs_xings *x);

// Each subpath of `path` is implicitly closed.  Span quads (gpu_vert triples)
// are appended to `out`, clipped to [0,width) x [0,height).  `edges`/`xings` are
// caller-owned scratch reused across calls.  False only on allocation failure.
bool cnvs_fill_path(cnvs_path const *path, cnvs_fill_rule rule,
                    int width, int height, cnvs_verts *out,
                    cnvs_edges *edges, cnvs_xings *xings);
