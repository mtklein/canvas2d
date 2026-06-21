#pragma once

// Subpaths stored in device space -- points are transformed by the CTM as they
// are added, matching Canvas 2D.  All subpaths share one point array; `subs`
// holds each one's [start, start+count) range.

#include "cnvs_matrix.h"

#include <ptrcheck.h>

typedef struct {
    int start;
    int count;
    bool closed;
} cnvs_subpath;

struct cnvs_path {
    cnvs_vec2 *__counted_by(pt_cap) pts;
    int npts;
    int pt_cap;
    cnvs_subpath *__counted_by(sp_cap) subs;
    int nsubs;
    int sp_cap;
    bool has_cur;
    cnvs_vec2 cur;
};

void cnvs_path_init(struct cnvs_path *p);
void cnvs_path_free(struct cnvs_path *p);
void cnvs_path_reset(struct cnvs_path *p);

bool cnvs_path_move_to(struct cnvs_path *p, cnvs_vec2 pt);
bool cnvs_path_line_to(struct cnvs_path *p, cnvs_vec2 pt);
bool cnvs_path_close(struct cnvs_path *p);
bool cnvs_path_rect(struct cnvs_path *p,
                    cnvs_vec2 a, cnvs_vec2 b, cnvs_vec2 c, cnvs_vec2 d);

// Curves are flattened into line segments at chord tolerance `tol` (pixels).
bool cnvs_path_quad_to(struct cnvs_path *p, cnvs_vec2 ctrl, cnvs_vec2 end, float tol);
bool cnvs_path_cubic_to(struct cnvs_path *p, cnvs_vec2 c1, cnvs_vec2 c2,
                        cnvs_vec2 end, float tol);
