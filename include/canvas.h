#pragma once

// A C implementation of a subset of the HTML Canvas 2D API.  Coordinates are
// pixels, origin top-left, +y down, matching the web platform.

#include <ptrcheck.h>
#include <stdint.h>

typedef struct canvas canvas;

typedef enum { CANVAS_NONZERO, CANVAS_EVENODD } canvas_fill_rule;
typedef enum { CANVAS_JOIN_MITER, CANVAS_JOIN_ROUND, CANVAS_JOIN_BEVEL } canvas_line_join;
typedef enum { CANVAS_CAP_BUTT, CANVAS_CAP_ROUND, CANVAS_CAP_SQUARE } canvas_line_cap;

// NULL on failure; the canvas starts transparent black.
canvas *__single canvas_create(int width, int height);
void canvas_destroy(canvas *__single cv);

void canvas_save(canvas *__single cv);
void canvas_restore(canvas *__single cv);

void canvas_translate(canvas *__single cv, float tx, float ty);
void canvas_scale(canvas *__single cv, float sx, float sy);
void canvas_rotate(canvas *__single cv, float radians);
void canvas_transform(canvas *__single cv,
                      float a, float b, float c, float d, float e, float f);
void canvas_set_transform(canvas *__single cv,
                          float a, float b, float c, float d, float e, float f);
void canvas_reset_transform(canvas *__single cv);

void canvas_set_fill_rgba(canvas *__single cv, float r, float g, float b, float a);
void canvas_set_global_alpha(canvas *__single cv, float alpha);

// Set the fill paint to a gradient and clear its colour stops; subsequent
// fill() calls paint with it until the next canvas_set_fill_rgba.  Coordinates
// are in the current user space (the transform is baked in now), matching how
// path points are transformed when added.  Add at least two stops with
// canvas_add_fill_color_stop; offsets are clamped to [0,1].
void canvas_set_fill_linear_gradient(canvas *__single cv,
                                     float x0, float y0, float x1, float y1);
void canvas_set_fill_radial_gradient(canvas *__single cv, float x0, float y0,
                                     float r0, float x1, float y1, float r1);
void canvas_add_fill_color_stop(canvas *__single cv, float offset,
                                float r, float g, float b, float a);

void canvas_clear_rect(canvas *__single cv, float x, float y, float w, float h);
void canvas_fill_rect(canvas *__single cv, float x, float y, float w, float h);

// Path coordinates are transformed by the current transform as they are added.
void canvas_begin_path(canvas *__single cv);
void canvas_move_to(canvas *__single cv, float x, float y);
void canvas_line_to(canvas *__single cv, float x, float y);
void canvas_rect(canvas *__single cv, float x, float y, float w, float h);
void canvas_quadratic_curve_to(canvas *__single cv,
                               float cpx, float cpy, float x, float y);
void canvas_bezier_curve_to(canvas *__single cv, float c1x, float c1y,
                            float c2x, float c2y, float x, float y);
void canvas_arc(canvas *__single cv, float x, float y, float radius,
                float start_angle, float end_angle, bool anticlockwise);
void canvas_ellipse(canvas *__single cv, float x, float y, float rx, float ry,
                    float rotation, float start_angle, float end_angle,
                    bool anticlockwise);
void canvas_round_rect(canvas *__single cv, float x, float y, float w, float h,
                       float radius);
void canvas_arc_to(canvas *__single cv, float x1, float y1, float x2, float y2,
                   float radius);
void canvas_close_path(canvas *__single cv);

// Fill the current path under the current fill rule (default nonzero); handles
// holes and self-intersection.
void canvas_set_fill_rule(canvas *__single cv, canvas_fill_rule rule);
void canvas_fill(canvas *__single cv);

// Intersect the clip region with the current path (under the current fill rule).
// Subsequent draws are masked to the running intersection of all clips; the
// region is part of the saved state, so save()/restore() brackets a clip.
void canvas_clip(canvas *__single cv);

void canvas_set_stroke_rgba(canvas *__single cv, float r, float g, float b, float a);
// Gradient stroke paint, mirroring the fill gradient calls; stroke() uses it
// until the next canvas_set_stroke_rgba.  (Coordinates are baked through the
// transform now, as for fills.)
void canvas_set_stroke_linear_gradient(canvas *__single cv,
                                       float x0, float y0, float x1, float y1);
void canvas_set_stroke_radial_gradient(canvas *__single cv, float x0, float y0,
                                       float r0, float x1, float y1, float r1);
void canvas_add_stroke_color_stop(canvas *__single cv, float offset,
                                  float r, float g, float b, float a);
void canvas_set_line_width(canvas *__single cv, float width);
void canvas_set_line_join(canvas *__single cv, canvas_line_join join);
void canvas_set_line_cap(canvas *__single cv, canvas_line_cap cap);
void canvas_set_miter_limit(canvas *__single cv, float limit);
// `pattern` lists alternating on/off lengths (user units); count 0 = solid.
void canvas_set_line_dash(canvas *__single cv,
                          float const *__counted_by(count) pattern, int count);
void canvas_set_line_dash_offset(canvas *__single cv, float offset);
void canvas_stroke(canvas *__single cv);

// drawImage: a tightly packed RGBA8 source (sw*sh, top row first), sampled
// bilinearly, transformed by the current transform, composited source-over under
// the current global alpha and clip.  The three forms mirror the Canvas 2D
// overloads; the subrect form samples source rect [sx,sy,sw',sh'] into dest rect
// [dx,dy,dw,dh] (both user space).
void canvas_draw_image(canvas *__single cv,
                       uint8_t const *__counted_by(sw * sh * 4) src,
                       int sw, int sh, float dx, float dy);
void canvas_draw_image_scaled(canvas *__single cv,
                              uint8_t const *__counted_by(sw * sh * 4) src,
                              int sw, int sh, float dx, float dy,
                              float dw, float dh);
void canvas_draw_image_subrect(canvas *__single cv,
                               uint8_t const *__counted_by(sw * sh * 4) src,
                               int sw, int sh, float sx, float sy,
                               float sww, float shh, float dx, float dy,
                               float dw, float dh);

// Tightly packed RGBA8, top row first; len must be width*height*4.
void canvas_read_rgba(canvas *__single cv, uint8_t *__counted_by(len) out, int len);
bool canvas_write_png(canvas *__single cv, char const *__null_terminated path);

// Pixel I/O for a w*h sub-image (tightly packed RGBA8, len must be w*h*4).
// get: pixels outside the canvas read back transparent black.
// put: overwrites (no blending), clipped to the canvas.
void canvas_get_image_data(canvas *__single cv, int x, int y, int w, int h,
                           uint8_t *__counted_by(len) out, int len);
void canvas_put_image_data(canvas *__single cv,
                           uint8_t const *__counted_by(len) data, int len,
                           int w, int h, int dx, int dy);
