#pragma once

// Ear-clipping triangulation of one simple polygon (convex or concave).  Does
// not resolve winding between subpaths (holes) or self-intersection; the canvas
// fills each subpath independently.

#include "cnvs_geom.h"
#include "cnvs_math.h"

#include <ptrcheck.h>

// Appends gpu_vert triples to `out`; `scratch` is reused across calls to avoid
// per-call allocation.  False only on allocation failure.
bool cnvs_tess_polygon(cnvs_vec2 const *__counted_by(n) poly, int n,
                       cnvs_verts *out, cnvs_ints *scratch);
