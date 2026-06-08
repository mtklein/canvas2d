#pragma once

// `half_width` is in device space -- the caller bakes in the CTM scale.
// Bevel joins and butt caps only; joins double-cover on the inner side, which
// shows only for translucent strokes.

#include "cnvs_geom.h"
#include "cnvs_math.h"

#include <ptrcheck.h>

bool cnvs_stroke_polyline(cnvs_vec2 const *__counted_by(n) pts, int n,
                          bool closed, float half_width, cnvs_verts *out);

// Dashed variant: walk the polyline against `dash` (alternating on/off lengths,
// cycled, device units) starting at `dash_offset`, stroking only the on runs as
// butt-capped quads -- no joins across dash gaps.
bool cnvs_stroke_dashed(cnvs_vec2 const *__counted_by(n) pts, int n, bool closed,
                        float half_width, float const *__counted_by(ndash) dash,
                        int ndash, float dash_offset, cnvs_verts *out);
