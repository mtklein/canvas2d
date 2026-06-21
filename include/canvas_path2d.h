#pragma once

#include <ptrcheck.h>
#include <stdbool.h>

// Path2D: a constructible path object, independent of the canvas and its current
// path.  Coordinates are recorded in user space and transformed by the canvas's
// current transform when the path is filled/stroked/clipped/hit-tested -- so the
// same Path2D draws differently under different transforms (unlike the current
// path, whose points are baked at build time).  NULL on allocation failure; free
// with canvas_path2d_free.  The canvas methods that consume a Path2D
// (canvas_fill_path / canvas_stroke_path / canvas_clip_path / the hit tests) live
// in canvas.h.
struct canvas_path2d;
struct canvas_path2d *__single canvas_path2d(void);
void canvas_path2d_free(struct canvas_path2d *__single p);

// Build a Path2D.  These mirror the canvas path methods (a zero-radius corner is
// sharp; arc/ellipse angles are radians; round_rect takes one scalar radius).
void canvas_path2d_move_to(struct canvas_path2d *__single p, float x, float y);
void canvas_path2d_line_to(struct canvas_path2d *__single p, float x, float y);
void canvas_path2d_quadratic_curve_to(struct canvas_path2d *__single p,
                                      float cpx, float cpy, float x, float y);
void canvas_path2d_bezier_curve_to(struct canvas_path2d *__single p, float c1x, float c1y,
                                   float c2x, float c2y, float x, float y);
void canvas_path2d_arc(struct canvas_path2d *__single p, float x, float y, float radius,
                       float start_angle, float end_angle, bool anticlockwise);
void canvas_path2d_ellipse(struct canvas_path2d *__single p, float x, float y,
                           float rx, float ry, float rotation,
                           float start_angle, float end_angle, bool anticlockwise);
void canvas_path2d_arc_to(struct canvas_path2d *__single p, float x1, float y1,
                          float x2, float y2, float radius);
void canvas_path2d_rect(struct canvas_path2d *__single p, float x, float y,
                        float w, float h);
void canvas_path2d_round_rect(struct canvas_path2d *__single p, float x, float y,
                              float w, float h, float radius);
void canvas_path2d_close_path(struct canvas_path2d *__single p);
// Append all of `src`'s commands to `dst` (addPath, without a transform).
void canvas_path2d_add_path(struct canvas_path2d *__single dst,
                            struct canvas_path2d const *__single src);
