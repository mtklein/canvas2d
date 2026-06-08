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

// One filled interval [xl, xr) on scanline `row`, already clamped to the canvas.
typedef struct {
    float xl, xr;
    int row;
} cnvs_span;

typedef struct {
    cnvs_span *__counted_by(cap) data;
    int len;
    int cap;
} cnvs_spans;

void cnvs_edges_free(cnvs_edges *e);
void cnvs_xings_free(cnvs_xings *x);
void cnvs_spans_free(cnvs_spans *s);

// Scan-convert all subpaths of `path` (each implicitly closed) under `rule` into
// inside-spans, clipped to [0,width) x [0,height); appended to `out`.  This is
// the shared core: callers turn spans into geometry (solid quads, or per-vertex
// gradient colours).  `edges`/`xings` are caller-owned scratch reused across
// calls.  False only on allocation failure.
bool cnvs_fill_spans(cnvs_path const *path, cnvs_fill_rule rule,
                     int width, int height, cnvs_spans *out,
                     cnvs_edges *edges, cnvs_xings *xings);

// Convenience over cnvs_fill_spans: emit each span as a solid quad (gpu_vert
// triples) into `out`.  `spans` is caller-owned scratch.  Used for plain fills
// and for building clip masks.
bool cnvs_fill_path(cnvs_path const *path, cnvs_fill_rule rule,
                    int width, int height, cnvs_verts *out,
                    cnvs_edges *edges, cnvs_xings *xings, cnvs_spans *spans);
