#pragma once

// Ear-clipping triangulation of a single simple polygon.
//
// Handles convex and concave simple polygons.  It does NOT yet resolve winding
// between subpaths (holes) or self-intersection; those await M2.  Each subpath
// is triangulated independently by the canvas fill path.

#include "cnvs_geom.h"
#include "cnvs_math.h"

#include <ptrcheck.h>

// Triangulate `poly` (n points) into `out` as gpu_vert triples.  `scratch` is a
// caller-owned working buffer reused across calls.  Returns false only on
// allocation failure.
bool cnvs_tess_polygon(const cnvs_vec2 *__counted_by(n) poly, int n,
                       cnvs_verts *out, cnvs_ints *scratch);
