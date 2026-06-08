#pragma once

// A path under construction, stored in device space (points are already
// transformed by the CTM at the time they were added, matching Canvas 2D).
// Points for all subpaths live in one flat array; `subs` records each
// subpath's [start, start+count) range.

#include "cnvs_math.h"

#include <ptrcheck.h>

typedef struct {
    int start;
    int count;
    bool closed;
} cnvs_subpath;

typedef struct {
    cnvs_vec2 *__counted_by(pt_cap) pts;
    int pt_len;
    int pt_cap;
    cnvs_subpath *__counted_by(sp_cap) subs;
    int sp_len;
    int sp_cap;
    bool has_cur;
    cnvs_vec2 cur;
} cnvs_path;

void cnvs_path_init(cnvs_path *p);
void cnvs_path_free(cnvs_path *p);
void cnvs_path_reset(cnvs_path *p);  // clear contents, keep capacity

bool cnvs_path_move_to(cnvs_path *p, cnvs_vec2 pt);
bool cnvs_path_line_to(cnvs_path *p, cnvs_vec2 pt);
bool cnvs_path_close(cnvs_path *p);
bool cnvs_path_rect(cnvs_path *p,
                    cnvs_vec2 a, cnvs_vec2 b, cnvs_vec2 c, cnvs_vec2 d);

// Flatten curves from the current point into line segments (device space).
// `tol` is the maximum chord deviation in pixels.
bool cnvs_path_quad_to(cnvs_path *p, cnvs_vec2 ctrl, cnvs_vec2 end, float tol);
bool cnvs_path_cubic_to(cnvs_path *p, cnvs_vec2 c1, cnvs_vec2 c2,
                        cnvs_vec2 end, float tol);
