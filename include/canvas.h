#pragma once

// A C implementation of a subset of the HTML Canvas 2D API.  Coordinates are
// pixels, origin top-left, +y down, matching the web platform.

#include <ptrcheck.h>
#include <stdint.h>

typedef struct canvas canvas;

// The six components of a 2D affine transform: (x,y) maps to
// (a*x + c*y + e, b*x + d*y + f) -- the argument order of canvas_set_transform.
typedef struct { float a, b, c, d, e, f; } canvas_matrix;

typedef enum { CANVAS_NONZERO, CANVAS_EVENODD } canvas_fill_rule;
typedef enum { CANVAS_JOIN_MITER, CANVAS_JOIN_ROUND, CANVAS_JOIN_BEVEL } canvas_line_join;
typedef enum { CANVAS_CAP_BUTT, CANVAS_CAP_ROUND, CANVAS_CAP_SQUARE } canvas_line_cap;

// globalCompositeOperation.  Order mirrors compositor_blend_mode (src/compositor.h)
// value-for-value; the first 11 are Porter-Duff operators, then the separable
// blend modes, then the four non-separable ones.
typedef enum {
    CANVAS_OP_SOURCE_OVER, CANVAS_OP_SOURCE_IN, CANVAS_OP_SOURCE_OUT,
    CANVAS_OP_SOURCE_ATOP, CANVAS_OP_DESTINATION_OVER, CANVAS_OP_DESTINATION_IN,
    CANVAS_OP_DESTINATION_OUT, CANVAS_OP_DESTINATION_ATOP, CANVAS_OP_XOR,
    CANVAS_OP_LIGHTER, CANVAS_OP_COPY,
    CANVAS_OP_MULTIPLY, CANVAS_OP_SCREEN, CANVAS_OP_OVERLAY, CANVAS_OP_DARKEN,
    CANVAS_OP_LIGHTEN, CANVAS_OP_COLOR_DODGE, CANVAS_OP_COLOR_BURN,
    CANVAS_OP_HARD_LIGHT, CANVAS_OP_SOFT_LIGHT, CANVAS_OP_DIFFERENCE,
    CANVAS_OP_EXCLUSION,
    CANVAS_OP_HUE, CANVAS_OP_SATURATION, CANVAS_OP_COLOR, CANVAS_OP_LUMINOSITY,
} canvas_composite_op;

// NULL on failure; the canvas starts transparent black.
canvas *__single canvas_create(int width, int height);
void canvas_destroy(canvas *__single cv);

// Whether the rendering context has been lost (matching isContextLost).  This
// headless renderer owns its backing store and never loses it, so it is always
// false; provided for API parity.
bool canvas_is_context_lost(canvas *__single cv);

void canvas_save(canvas *__single cv);
void canvas_restore(canvas *__single cv);
// Reset the context to its initial state: empty the save/restore stack, restore
// every drawing-state field to its default, discard the current path, open the
// clip, and clear the bitmap to transparent black.
void canvas_reset(canvas *__single cv);

void canvas_translate(canvas *__single cv, float tx, float ty);
void canvas_scale(canvas *__single cv, float sx, float sy);
void canvas_rotate(canvas *__single cv, float radians);
void canvas_transform(canvas *__single cv,
                      float a, float b, float c, float d, float e, float f);
void canvas_set_transform(canvas *__single cv,
                          float a, float b, float c, float d, float e, float f);
void canvas_reset_transform(canvas *__single cv);
// The current transform (matching getTransform): the matrix built up by
// translate/scale/rotate/transform and reset by set_transform/reset_transform.
canvas_matrix canvas_get_transform(canvas *__single cv);

void canvas_set_fill_rgba(canvas *__single cv, float r, float g, float b, float a);
void canvas_set_global_alpha(canvas *__single cv, float alpha);
// globalCompositeOperation: how subsequent fills/strokes/text/images combine with
// what's already there (default source-over).  Applies to every painted op except
// clear_rect and put_image_data.
void canvas_set_global_composite_operation(canvas *__single cv,
                                           canvas_composite_op op);

// Set the fill paint to a gradient and clear its stops; fill() uses it until the
// next canvas_set_fill_rgba.  Coordinates are user space (the transform is baked in
// now).  Add stops with canvas_add_fill_color_stop; offsets clamp to [0,1].
void canvas_set_fill_linear_gradient(canvas *__single cv,
                                     float x0, float y0, float x1, float y1);
void canvas_set_fill_radial_gradient(canvas *__single cv, float x0, float y0,
                                     float r0, float x1, float y1, float r1);
void canvas_add_fill_color_stop(canvas *__single cv, float offset,
                                float r, float g, float b, float a);

void canvas_clear_rect(canvas *__single cv, float x, float y, float w, float h);
void canvas_fill_rect(canvas *__single cv, float x, float y, float w, float h);
// Stroke the outline of the rectangle with the current stroke style and line
// styles (width/join/cap/dash), without disturbing the current path.  Corners
// are transformed by the current transform (a rotated CTM strokes a rotated
// quad).  A zero-width-or-height rectangle degenerates to a stroked line.
void canvas_stroke_rect(canvas *__single cv, float x, float y, float w, float h);

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
// roundRect with independent, elliptical per-corner radii.  Corners are given in
// CSS order -- top-left, top-right, bottom-right, bottom-left -- each as an (x,y)
// radius pair (equal x and y is a circular corner; 0 is a sharp corner).
// Negative/non-finite radii are treated as 0, and radii too large for the rect
// are scaled down proportionally (the CSS border-radius overlap rule).
void canvas_round_rect_radii(canvas *__single cv, float x, float y,
                             float w, float h,
                             float tl_x, float tl_y, float tr_x, float tr_y,
                             float br_x, float br_y, float bl_x, float bl_y);
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
// Copy the current dash pattern into `out` (up to `cap` entries) and return its
// full length.  Pass cap 0 (out may be NULL) to query the length, then size a
// buffer; the returned float values are a copy, so mutating them is harmless.
int canvas_get_line_dash(canvas *__single cv,
                         float *__counted_by(cap) out, int cap);
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

// Text.  The typeface is fixed to Libian TC (clerical-script 隸書), which carries
// both Latin and Chinese.  Size is user-space px (default 10).  fill_text and
// stroke_text lay out `text` (UTF-8) with its baseline origin at (x, y), advance
// +x, and paint the glyph outlines like fill()/stroke(): transform, clip, gradient
// and global alpha all apply.  measure_text returns the advance width in user px.
void canvas_set_font_size(canvas *__single cv, float px);
float canvas_measure_text(canvas *__single cv, char const *__null_terminated text);
void canvas_fill_text(canvas *__single cv, char const *__null_terminated text,
                      float x, float y);
void canvas_stroke_text(canvas *__single cv, char const *__null_terminated text,
                        float x, float y);

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
// putImageData with a dirty rectangle: only the source sub-rect
// [dirtyX, dirtyX+dirtyWidth) x [dirtyY, dirtyY+dirtyHeight) (ImageData
// coordinates) is written, still placed with the image origin at (dx, dy).  The
// dirty rect is normalised like the spec -- negative extents flip, then it is
// clamped to the source; an empty result is a no-op.
void canvas_put_image_data_dirty(canvas *__single cv,
                                 uint8_t const *__counted_by(len) data, int len,
                                 int w, int h, int dx, int dy,
                                 int dirty_x, int dirty_y,
                                 int dirty_w, int dirty_h);

// Read a text canvas-program from `path` and replay it onto cv via the calls
// above.  One command per line (UTF-8): each command is exactly the canvas_*
// function name *without* the `canvas_` prefix, followed by its whitespace-
// separated arguments, e.g.:
//     set_fill_rgba 0.8 0.2 0.2 1
//     move_to 10 20
//     set_global_composite_operation multiply
//     fill_text 12 30 Hello
// Enums (set_global_composite_operation / set_line_join / set_line_cap /
// set_fill_rule) are written by name; the bool arg of arc/ellipse is 0/1 or
// false/true; set_line_dash takes a variable number of lengths; fill_text and
// stroke_text take the rest of the line as their text.  Blank lines and lines
// whose first non-space character is `#` are ignored.  Parsing is strict: an
// unknown command, a bad argument, or an over-long line stops replay and returns
// false (commands before the faulty line have already been applied).  The query
// and image ops (measure_text, read_rgba, write_png, get/put_image_data,
// draw_image) are not part of the text format.
bool canvas_replay_from(canvas *__single cv, char const *__null_terminated path);

// createImageData: allocate a blank (transparent black) RGBA8 image of sw*sh
// pixels -- the layout get/put_image_data use.  Returns a freshly malloc'd,
// zeroed buffer of sw*sh*4 bytes (free it with free()) and stores that byte
// length in *len, ready to hand to put_image_data.  On non-positive dimensions,
// a size that would overflow, or out of memory, returns NULL and stores 0.  The
// canvas argument is accepted for API symmetry; the image is independent of its
// contents.
uint8_t *__counted_by_or_null(*len)
canvas_create_image_data(canvas *__single cv, int sw, int sh, int *__single len);
