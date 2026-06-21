#pragma once

// Path2D's recorded command list -- the user-space commands a struct canvas_path2d
// holds, shared between canvas.c (which builds them and replays them through
// the canvas path methods at draw time) and the record/replay pair
// (cnvs_record.c serializes a path as a numbered `path` block when
// fill_path/stroke_path/clip_path records; cnvs_replay.c verifies a rebuilt
// block's command count).  Coordinates are user space; the canvas's current
// transform applies when the path is drawn, not here (canvas.h).

#include "canvas.h"
#include "canvas_path2d.h"  // the public Path2D API; this header adds the internals

#include <ptrcheck.h>

enum p2d_verb {
    P2D_MOVE, P2D_LINE, P2D_QUAD, P2D_CUBIC, P2D_ARC, P2D_ELLIPSE,
    P2D_ARC_TO, P2D_RECT, P2D_ROUND_RECT, P2D_CLOSE
};

typedef struct {
    enum p2d_verb verb;
    float a[8];  // op arguments (ellipse uses the most: x,y,rx,ry,rotation,sa,ea)
    bool ccw;    // arc / ellipse winding direction
} p2d_cmd;

struct canvas_path2d {
    p2d_cmd *__counted_by(cap) cmds;
    int ncmds;
    int cap;
};
