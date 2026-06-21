#pragma once

// Subpaths stored in device space -- points are transformed by the CTM as they
// are added, matching Canvas 2D.  All subpaths share one point array; `subs`
// holds each one's [start, start+count) range.

#include "canvas2d_matrix.h"

#include <ptrcheck.h>

typedef struct {
    int start;
    int count;
    bool closed;
} canvas2d_subpath;

struct canvas2d_path {
    canvas2d_vec2 *__counted_by(pt_cap) pts;
    int npts;
    int pt_cap;
    canvas2d_subpath *__counted_by(sp_cap) subs;
    int nsubs;
    int sp_cap;
    bool has_cur;
    canvas2d_vec2 cur;
};

void canvas2d_path_init(struct canvas2d_path *p);
void canvas2d_path_free(struct canvas2d_path *p);
void canvas2d_path_reset(struct canvas2d_path *p);

bool canvas2d_path_move_to(struct canvas2d_path *p, canvas2d_vec2 pt);
bool canvas2d_path_line_to(struct canvas2d_path *p, canvas2d_vec2 pt);
bool canvas2d_path_close(struct canvas2d_path *p);
bool canvas2d_path_rect(struct canvas2d_path *p,
                    canvas2d_vec2 a, canvas2d_vec2 b, canvas2d_vec2 c, canvas2d_vec2 d);

// Curves are flattened into line segments at chord tolerance `tol` (pixels).
bool canvas2d_path_quad_to(struct canvas2d_path *p, canvas2d_vec2 ctrl, canvas2d_vec2 end, float tol);
bool canvas2d_path_cubic_to(struct canvas2d_path *p, canvas2d_vec2 c1, canvas2d_vec2 c2,
                        canvas2d_vec2 end, float tol);
