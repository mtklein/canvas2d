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

// textAlign / textBaseline.  Direction is LTR, so start == left and end == right.
typedef enum {
    CANVAS_ALIGN_START, CANVAS_ALIGN_END,
    CANVAS_ALIGN_LEFT, CANVAS_ALIGN_RIGHT, CANVAS_ALIGN_CENTER,
} canvas_text_align;
typedef enum {
    CANVAS_BASELINE_ALPHABETIC, CANVAS_BASELINE_TOP, CANVAS_BASELINE_HANGING,
    CANVAS_BASELINE_MIDDLE, CANVAS_BASELINE_IDEOGRAPHIC, CANVAS_BASELINE_BOTTOM,
} canvas_text_baseline;

// imageSmoothingQuality (a hint; see canvas_set_image_smoothing_quality).
typedef enum {
    CANVAS_SMOOTHING_LOW, CANVAS_SMOOTHING_MEDIUM, CANVAS_SMOOTHING_HIGH,
} canvas_image_smoothing_quality;

// createPattern repetition mode.
typedef enum {
    CANVAS_REPEAT, CANVAS_REPEAT_X, CANVAS_REPEAT_Y, CANVAS_NO_REPEAT,
} canvas_pattern_repeat;

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

// Resize the canvas to width x height (like assigning canvas.width/height): the
// bitmap is reallocated and cleared to transparent black, and the drawing state
// is reset to its defaults -- both per the spec.  Returns false, leaving the
// canvas untouched, on invalid dimensions or allocation failure.
bool canvas_resize(canvas *__single cv, int width, int height);

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

// Shadows: a blurred, offset, colour-tinted copy of each fill/stroke/text/image
// is painted underneath it.  A shadow is cast only when shadow_color's alpha is
// > 0 and there is some blur or offset.  shadow_blur is a Gaussian-style radius;
// it and the offsets are in device pixels and are NOT affected by the current
// transform (per spec).  Defaults: transparent shadow (off), 0 blur, 0 offset.
// Negative/non-finite blur and non-finite offsets are ignored, as the spec says.
void canvas_set_shadow_color_rgba(canvas *__single cv,
                                  float r, float g, float b, float a);
void canvas_set_shadow_blur(canvas *__single cv, float blur);
void canvas_set_shadow_offset_x(canvas *__single cv, float offset);
void canvas_set_shadow_offset_y(canvas *__single cv, float offset);

// Set the fill paint to a gradient and clear its stops; fill() uses it until the
// next canvas_set_fill_rgba.  Coordinates are user space (the transform is baked in
// now).  Add stops with canvas_add_fill_color_stop; offsets clamp to [0,1].
void canvas_set_fill_linear_gradient(canvas *__single cv,
                                     float x0, float y0, float x1, float y1);
void canvas_set_fill_radial_gradient(canvas *__single cv, float x0, float y0,
                                     float r0, float x1, float y1, float r1);
// Conic gradient (createConicGradient): colours sweep clockwise around (x, y)
// from `start_angle` radians (measured from the +x axis).  Stops are added with
// canvas_add_fill_color_stop, as for the linear/radial gradients.
void canvas_set_fill_conic_gradient(canvas *__single cv, float start_angle,
                                    float x, float y);
void canvas_add_fill_color_stop(canvas *__single cv, float offset,
                                float r, float g, float b, float a);
// Image pattern fill paint (createPattern): the tightly packed RGBA8 source
// (w*h, top row first) tiles the plane per `repeat`.  The source is *borrowed* --
// the caller must keep it alive while it remains the fill paint (including across
// save/restore) -- and the pattern is pinned in device space at the current
// transform, like the gradients.  Sampling honours image smoothing (bilinear vs
// nearest).  Ignored if the dimensions are non-positive or overflow.
void canvas_set_fill_pattern(canvas *__single cv,
                             uint8_t const *__counted_by(w * h * 4) src,
                             int w, int h, canvas_pattern_repeat repeat);

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

// Hit testing: whether the point (x, y) is inside the current path under `rule`.
// (x, y) is transformed by the current transform before the test -- matching how
// path points are baked through the CTM as they are added -- so a point given in
// the same user space as the path hits.  Non-finite coordinates return false.
bool canvas_is_point_in_path(canvas *__single cv, float x, float y,
                             canvas_fill_rule rule);
// Whether (x, y) is inside the stroked area of the current path under the current
// line styles (width/join/cap/dash).  (x, y) is transformed by the current
// transform, as for is_point_in_path; non-finite coordinates return false.
bool canvas_is_point_in_stroke(canvas *__single cv, float x, float y);

// Path2D: a constructible path object, independent of the canvas and its current
// path.  Coordinates are recorded in user space and transformed by the canvas's
// current transform when the path is filled/stroked/clipped/hit-tested -- so the
// same Path2D draws differently under different transforms (unlike the current
// path, whose points are baked at build time).  NULL on allocation failure; free
// with canvas_path2d_destroy.
typedef struct canvas_path2d canvas_path2d;
canvas_path2d *__single canvas_path2d_create(void);
void canvas_path2d_destroy(canvas_path2d *__single p);

// Build a Path2D.  These mirror the canvas path methods (a zero-radius corner is
// sharp; arc/ellipse angles are radians; round_rect takes one scalar radius).
void canvas_path2d_move_to(canvas_path2d *__single p, float x, float y);
void canvas_path2d_line_to(canvas_path2d *__single p, float x, float y);
void canvas_path2d_quadratic_curve_to(canvas_path2d *__single p,
                                      float cpx, float cpy, float x, float y);
void canvas_path2d_bezier_curve_to(canvas_path2d *__single p, float c1x, float c1y,
                                   float c2x, float c2y, float x, float y);
void canvas_path2d_arc(canvas_path2d *__single p, float x, float y, float radius,
                       float start_angle, float end_angle, bool anticlockwise);
void canvas_path2d_ellipse(canvas_path2d *__single p, float x, float y,
                           float rx, float ry, float rotation,
                           float start_angle, float end_angle, bool anticlockwise);
void canvas_path2d_arc_to(canvas_path2d *__single p, float x1, float y1,
                          float x2, float y2, float radius);
void canvas_path2d_rect(canvas_path2d *__single p, float x, float y,
                        float w, float h);
void canvas_path2d_round_rect(canvas_path2d *__single p, float x, float y,
                              float w, float h, float radius);
void canvas_path2d_close_path(canvas_path2d *__single p);
// Append all of `src`'s commands to `dst` (addPath, without a transform).
void canvas_path2d_add_path(canvas_path2d *__single dst,
                            canvas_path2d const *__single src);

// Fill / stroke / clip / hit-test a Path2D (the fill rule is explicit here, not
// taken from state).  None of these disturb the canvas's current path.
void canvas_fill_path(canvas *__single cv, canvas_path2d const *__single p,
                      canvas_fill_rule rule);
void canvas_stroke_path(canvas *__single cv, canvas_path2d const *__single p);
void canvas_clip_path(canvas *__single cv, canvas_path2d const *__single p,
                      canvas_fill_rule rule);
bool canvas_is_point_in_path2d(canvas *__single cv, canvas_path2d const *__single p,
                               float x, float y, canvas_fill_rule rule);
bool canvas_is_point_in_stroke_path(canvas *__single cv,
                                    canvas_path2d const *__single p,
                                    float x, float y);

void canvas_set_stroke_rgba(canvas *__single cv, float r, float g, float b, float a);
// Gradient stroke paint, mirroring the fill gradient calls; stroke() uses it
// until the next canvas_set_stroke_rgba.  (Coordinates are baked through the
// transform now, as for fills.)
void canvas_set_stroke_linear_gradient(canvas *__single cv,
                                       float x0, float y0, float x1, float y1);
void canvas_set_stroke_radial_gradient(canvas *__single cv, float x0, float y0,
                                       float r0, float x1, float y1, float r1);
void canvas_set_stroke_conic_gradient(canvas *__single cv, float start_angle,
                                      float x, float y);
void canvas_add_stroke_color_stop(canvas *__single cv, float offset,
                                  float r, float g, float b, float a);
// Image pattern stroke paint, mirroring canvas_set_fill_pattern.
void canvas_set_stroke_pattern(canvas *__single cv,
                               uint8_t const *__counted_by(w * h * 4) src,
                               int w, int h, canvas_pattern_repeat repeat);
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
// [dx,dy,dw,dh] (both user space).  Sampling is bilinear unless image smoothing
// is disabled (then nearest-neighbour).
//
// imageSmoothingEnabled: when true (default), drawImage samples bilinearly; when
// false, nearest-neighbour -- crisp, blocky upscaling with no blending.
void canvas_set_image_smoothing_enabled(canvas *__single cv, bool enabled);
// imageSmoothingQuality: a hint (low/medium/high) that applies only while
// smoothing is enabled.  Stored for API parity; our sampler is one bilinear
// quality, so it does not change the output.
void canvas_set_image_smoothing_quality(canvas *__single cv,
                                        canvas_image_smoothing_quality quality);
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
// textAlign: horizontal placement of the text relative to the (x, y) passed to
// fill_text/stroke_text, by the advance width.  start/left (default) puts x at
// the left edge, end/right at the right edge, center centres it.
void canvas_set_text_align(canvas *__single cv, canvas_text_align align);
// textBaseline: vertical placement of the baseline relative to (x, y).
// alphabetic (default) draws the baseline at y; top/hanging/middle/ideographic/
// bottom shift it by the font's ascent/descent.
void canvas_set_text_baseline(canvas *__single cv, canvas_text_baseline baseline);
float canvas_measure_text(canvas *__single cv, char const *__null_terminated text);

// Full measureText() TextMetrics, all in user px, relative to the text's origin
// and the alphabetic baseline (the defaults: textAlign start, textBaseline
// alphabetic).  Sign conventions match the web TextMetrics: *_left and *_ascent
// are positive measured leftward / upward from the origin, *_right and *_descent
// positive rightward / downward.  `width` equals canvas_measure_text.
typedef struct {
    float width;
    float actual_bounding_box_left, actual_bounding_box_right;
    float actual_bounding_box_ascent, actual_bounding_box_descent;
    float font_bounding_box_ascent, font_bounding_box_descent;
    float em_height_ascent, em_height_descent;
    float alphabetic_baseline, hanging_baseline, ideographic_baseline;
} canvas_text_metrics;

canvas_text_metrics canvas_measure_text_full(canvas *__single cv,
                                             char const *__null_terminated text);
void canvas_fill_text(canvas *__single cv, char const *__null_terminated text,
                      float x, float y);
void canvas_stroke_text(canvas *__single cv, char const *__null_terminated text,
                        float x, float y);
// Length-counted variants: `text` is `len` bytes of UTF-8 and need not be
// NUL-terminated, so a caller can pass a slice of a larger buffer directly.  The
// NUL-terminated fill_text/stroke_text above are conveniences over these.
void canvas_fill_text_n(canvas *__single cv, char const *__counted_by(len) text,
                        int len, float x, float y);
void canvas_stroke_text_n(canvas *__single cv, char const *__counted_by(len) text,
                          int len, float x, float y);
// fillText/strokeText with a maxWidth: when the text's advance exceeds a finite,
// positive `max_width`, it is condensed horizontally (scaled in x about the
// alignment anchor) to fit.  A non-positive or non-finite `max_width` imposes no
// limit, rendering exactly like canvas_fill_text/canvas_stroke_text.
void canvas_fill_text_max(canvas *__single cv, char const *__null_terminated text,
                          float x, float y, float max_width);
void canvas_stroke_text_max(canvas *__single cv, char const *__null_terminated text,
                            float x, float y, float max_width);

// Tightly packed RGBA8, top row first; len must be width*height*4.
void canvas_read_rgba(canvas *__single cv, uint8_t *__counted_by(len) out, int len);
bool canvas_write_png(canvas *__single cv, char const *__null_terminated path);

// Coarse GPU profiling forwarded from the compositor backend: total GPU execution
// time since canvas_create (nanoseconds) and the number of GPU dispatches that
// contributed (ns/dispatch = total_ns/dispatches).  The CPU backend reports 0/0;
// the Metal backend reports 0/0 unless CANVAS_GPU_TIMING was set in the environment
// at canvas_create time.  Either output pointer may be NULL.  Call after a readback
// (or canvas_destroy's drain) so all in-flight GPU work has been accounted for.
void canvas_gpu_timing(canvas *__single cv, double *__single total_ns, long *__single dispatches);

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

// Begin recording subsequent drawing calls to `path` as a text canvas-program in
// exactly the format canvas_replay_from reads -- the write-side inverse of replay.
// Each recordable call is appended as one line (the canvas_* name minus the
// canvas_ prefix, then its arguments); replaying the file onto a fresh canvas of
// the same size reproduces the same image.  Recording continues until the canvas
// is destroyed (or record_to is called again, which starts a new file); pass the
// same `path` again only after the first is closed.  Returns false (recording
// nothing) if the file cannot be opened.
//
// Only the ops the text format covers are recorded -- the same subset
// replay_from understands.  arc/round_rect/arc_to are written as themselves;
// calls outside the format (stroke_rect, round_rect_radii, conic gradients,
// patterns, image smoothing, text align/baseline, fill_text_max, draw_image,
// get/put_image_data, Path2D fill/stroke/clip, reset, resize) are not recorded,
// so a session that uses them does not round-trip through the text format.
bool canvas_record_to(canvas *__single cv, char const *__null_terminated path);

// createImageData: allocate a blank (transparent black) RGBA8 image of sw*sh
// pixels -- the layout get/put_image_data use.  Returns a freshly malloc'd,
// zeroed buffer of sw*sh*4 bytes (free it with free()) and stores that byte
// length in *len, ready to hand to put_image_data.  On non-positive dimensions,
// a size that would overflow, or out of memory, returns NULL and stores 0.  The
// canvas argument is accepted for API symmetry; the image is independent of its
// contents.
uint8_t *__counted_by_or_null(*len)
canvas_create_image_data(canvas *__single cv, int sw, int sh, int *__single len);
