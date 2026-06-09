#pragma once

// Linear and radial colour gradients.  Stops are kept sorted by offset in a small
// fixed array, so a gradient is a plain value copied with the canvas state on
// save/restore.  Fills and strokes sample it per pixel on the CPU into the tile the
// compositor blends.

#include "cnvs_math.h"

#include <ptrcheck.h>

#define CNVS_MAX_STOPS 16

typedef enum { CNVS_GRAD_LINEAR, CNVS_GRAD_RADIAL } cnvs_grad_kind;

typedef struct {
    float offset;  // in [0,1]
    cnvs_unpremul color;
} cnvs_stop;

// Coordinates and radii are device space (the CTM is baked in when the gradient
// is created).
typedef struct {
    cnvs_grad_kind kind;
    cnvs_vec2 p0, p1;
    float r0, r1;
    cnvs_stop stops[CNVS_MAX_STOPS];
    int stop_count;
} cnvs_gradient;

// Insert a stop in offset order (offset clamped to [0,1]); a no-op once full.
void cnvs_gradient_add_stop(cnvs_gradient *gr, float offset, cnvs_unpremul color);

// Colour at parameter t (clamped to [0,1]), piecewise-linear across the stops.
// With no stops the result is transparent black.
cnvs_unpremul cnvs_gradient_color_at(cnvs_gradient const *gr, float t);

// Gradient parameter for a device-space point, written to *t (clamped to
// [0,1]).  Returns false when a radial point lies outside the gradient (no
// circle in the family passes through it) -- such samples paint transparent.
bool cnvs_gradient_param(cnvs_gradient const *gr, cnvs_vec2 p, float *__single t);

// Unpremultiplied colour to paint at a device-space point, with `alpha` (global
// alpha) folded into the result's alpha.
cnvs_unpremul cnvs_gradient_sample(cnvs_gradient const *gr, cnvs_vec2 p, float alpha);
