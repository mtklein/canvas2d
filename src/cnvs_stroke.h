#pragma once

// Expand a polyline into stroke triangles: one quad per segment plus a bevel
// join at each shared vertex.  Caps are butt (no extension).  Widths are in
// device space (the caller bakes in the CTM scale).
//
// M1 limitation: bevel joins only (no miter/round), and joins may double-cover
// on the inner side -- visible only for translucent strokes.

#include "cnvs_geom.h"
#include "cnvs_math.h"

#include <ptrcheck.h>

bool cnvs_stroke_polyline(const cnvs_vec2 *__counted_by(n) pts, int n,
                          bool closed, float half_width, cnvs_verts *out);
