#pragma once

// Stroke geometry.  `half_width` is in device space -- the caller bakes in the
// CTM scale.  Dashed strokes use butt caps and no joins.

#include "cnvs_geom.h"
#include "cnvs_matrix.h"

#include <ptrcheck.h>

enum cnvs_line_join { CNVS_JOIN_MITER, CNVS_JOIN_ROUND, CNVS_JOIN_BEVEL };
enum cnvs_line_cap { CNVS_CAP_BUTT, CNVS_CAP_ROUND, CNVS_CAP_SQUARE };

bool cnvs_stroke_polyline(cnvs_vec2 const *__counted_by(n) pts, int n, bool closed,
                          float half_width, enum cnvs_line_join join,
                          enum cnvs_line_cap cap, float miter_limit,
                          struct cnvs_verts *out);

// Walk a cyclic dash pattern (device units, phase from `dash_offset`), stroking
// only the on-runs as butt-capped quads -- no joins across dash gaps.
bool cnvs_stroke_dashed(cnvs_vec2 const *__counted_by(n) pts, int n, bool closed,
                        float half_width, float const *__counted_by(ndash) dash,
                        int ndash, float dash_offset, struct cnvs_verts *out);
