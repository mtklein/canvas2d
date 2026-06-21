#pragma once

// Stroke geometry.  `half_width` is in device space -- the caller bakes in the
// CTM scale.  Dashed strokes use butt caps and no joins.

#include "canvas2d_geom.h"
#include "canvas2d_matrix.h"
#include "canvas2d_paint_style.h"  // enum canvas2d_line_join, enum canvas2d_line_cap

#include <ptrcheck.h>

bool canvas2d_stroke_polyline(canvas2d_vec2 const *__counted_by(n) pts, int n, bool closed,
                          float half_width, enum canvas2d_line_join join,
                          enum canvas2d_line_cap cap, float miter_limit,
                          struct canvas2d_verts *out);

// Walk a cyclic dash pattern (device units, phase from `dash_offset`), stroking
// only the on-runs as butt-capped quads -- no joins across dash gaps.
bool canvas2d_stroke_dashed(canvas2d_vec2 const *__counted_by(n) pts, int n, bool closed,
                        float half_width, float const *__counted_by(ndash) dash,
                        int ndash, float dash_offset, struct canvas2d_verts *out);
