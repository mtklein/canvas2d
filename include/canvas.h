#pragma once

// A C implementation of (a growing subset of) the HTML Canvas 2D API,
// rasterised on the GPU via Metal.  Coordinates are in pixels with the origin
// at the top-left and +y pointing down, matching the web platform.

#include <ptrcheck.h>
#include <stdint.h>

typedef struct canvas canvas;

// Create a canvas with an offscreen RGBA8 target of the given pixel size,
// initialised to transparent black.  Returns NULL on failure.
canvas *__single canvas_create(int width, int height);
void canvas_destroy(canvas *__single cv);

// Drawing-state stack (transform, styles).
void canvas_save(canvas *__single cv);
void canvas_restore(canvas *__single cv);

// Current-transform manipulation.
void canvas_translate(canvas *__single cv, float tx, float ty);
void canvas_scale(canvas *__single cv, float sx, float sy);
void canvas_rotate(canvas *__single cv, float radians);
void canvas_transform(canvas *__single cv,
                      float a, float b, float c, float d, float e, float f);
void canvas_set_transform(canvas *__single cv,
                          float a, float b, float c, float d, float e, float f);
void canvas_reset_transform(canvas *__single cv);

// Styles.
void canvas_set_fill_rgba(canvas *__single cv, float r, float g, float b, float a);
void canvas_set_global_alpha(canvas *__single cv, float alpha);

// Rectangles.
void canvas_clear_rect(canvas *__single cv, float x, float y, float w, float h);
void canvas_fill_rect(canvas *__single cv, float x, float y, float w, float h);

// Path building.  Coordinates are transformed by the current transform when
// added, matching Canvas 2D.
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
void canvas_close_path(canvas *__single cv);

// Fill the current path with the current fill style (each subpath is filled).
void canvas_fill(canvas *__single cv);

// Stroke the current path with the current stroke style and line width.
void canvas_set_stroke_rgba(canvas *__single cv, float r, float g, float b, float a);
void canvas_set_line_width(canvas *__single cv, float width);
void canvas_stroke(canvas *__single cv);

// Read back / export.

// Copy the rendered image into `out` as tightly packed RGBA8, top row first.
// `len` must equal width*height*4.
void canvas_read_rgba(canvas *__single cv, uint8_t *__counted_by(len) out, int len);

bool canvas_write_png(canvas *__single cv, const char *__null_terminated path);
