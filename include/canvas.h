#pragma once

// A C implementation of a subset of the HTML Canvas 2D API.  Coordinates are
// pixels, origin top-left, +y down, matching the web platform.

#include <ptrcheck.h>
#include <stdint.h>

struct canvas;  // the rendering context: canvas() constructs, canvas_free() frees

// The six components of a 2D affine transform: (x,y) maps to
// (a*x + c*y + e, b*x + d*y + f) -- the argument order of canvas_set_transform.
typedef struct { float a, b, c, d, e, f; } canvas_matrix;

enum canvas_fill_rule { CANVAS_NONZERO, CANVAS_EVENODD };
enum canvas_line_join { CANVAS_JOIN_MITER, CANVAS_JOIN_ROUND, CANVAS_JOIN_BEVEL };
enum canvas_line_cap { CANVAS_CAP_BUTT, CANVAS_CAP_ROUND, CANVAS_CAP_SQUARE };

// textAlign / textBaseline.  start/end resolve against the direction attribute:
// start == left and end == right under ltr, the opposite under rtl.
enum canvas_text_align {
    CANVAS_ALIGN_START, CANVAS_ALIGN_END,
    CANVAS_ALIGN_LEFT, CANVAS_ALIGN_RIGHT, CANVAS_ALIGN_CENTER,
};
// direction: the paragraph direction for text.  Headless, so no "inherit"; the
// default is ltr, matching what inherit resolves to in an undirected document.
enum canvas_direction { CANVAS_DIRECTION_LTR, CANVAS_DIRECTION_RTL };
enum canvas_text_baseline {
    CANVAS_BASELINE_ALPHABETIC, CANVAS_BASELINE_TOP, CANVAS_BASELINE_HANGING,
    CANVAS_BASELINE_MIDDLE, CANVAS_BASELINE_IDEOGRAPHIC, CANVAS_BASELINE_BOTTOM,
};

// imageSmoothingQuality (see canvas_set_image_smoothing_quality).
enum canvas_image_smoothing_quality {
    CANVAS_SMOOTHING_LOW, CANVAS_SMOOTHING_MEDIUM, CANVAS_SMOOTHING_HIGH,
};

// createPattern repetition mode.
enum canvas_pattern_repeat {
    CANVAS_REPEAT, CANVAS_REPEAT_X, CANVAS_REPEAT_Y, CANVAS_NO_REPEAT,
};

// Image pixel formats, two orthogonal axes (the Skia colorType/alphaType
// split): how a channel is stored, and whether colour rides premultiplied by
// alpha.  The canvas surface itself stays locked premultiplied
// f16; images parameterize over this 2x2, with no third axis or format
// until a real need shows up.
enum canvas_color_type { CANVAS_COLOR_UNORM8, CANVAS_COLOR_F16 };
enum canvas_alpha_type { CANVAS_ALPHA_UNPREMUL, CANVAS_ALPHA_PREMUL };

// globalCompositeOperation.  The blend kernels (canvas.c) dispatch on this
// order directly: the first 11 are Porter-Duff operators, then the separable
// blend modes, then the four non-separable ones.
enum canvas_composite_op {
    CANVAS_OP_SOURCE_OVER, CANVAS_OP_SOURCE_IN, CANVAS_OP_SOURCE_OUT,
    CANVAS_OP_SOURCE_ATOP, CANVAS_OP_DESTINATION_OVER, CANVAS_OP_DESTINATION_IN,
    CANVAS_OP_DESTINATION_OUT, CANVAS_OP_DESTINATION_ATOP, CANVAS_OP_XOR,
    CANVAS_OP_LIGHTER, CANVAS_OP_COPY,
    CANVAS_OP_MULTIPLY, CANVAS_OP_SCREEN, CANVAS_OP_OVERLAY, CANVAS_OP_DARKEN,
    CANVAS_OP_LIGHTEN, CANVAS_OP_COLOR_DODGE, CANVAS_OP_COLOR_BURN,
    CANVAS_OP_HARD_LIGHT, CANVAS_OP_SOFT_LIGHT, CANVAS_OP_DIFFERENCE,
    CANVAS_OP_EXCLUSION,
    CANVAS_OP_HUE, CANVAS_OP_SATURATION, CANVAS_OP_COLOR, CANVAS_OP_LUMINOSITY,
};

// The colour spaces this API names.  ONE enum spanning every role; each
// surface that takes a space accepts only the valid SUBSET for that role
// (canvas_in_space takes the two compositing spaces and returns NULL otherwise;
// the gradient interpolation setters take all three and silently ignore an
// out-of-range value).  Neither asserts.
//   CANVAS_CS_SRGB        -- encoded sRGB.
//   CANVAS_CS_LINEAR_SRGB -- extended linear sRGB (same primaries, linear
//                            transfer).
//   CANVAS_CS_OKLAB       -- Oklab; a perceptual interpolation space, not a
//                            compositing space.
//
// The canvas's WORKING COLOUR SPACE (canvas_in_space): the space untagged
// (encoded sRGB) colours are converted INTO for compositing.  Chosen at
// creation and immutable -- it lives on the canvas, not the save/restore
// drawing state, and reset/resize leave it alone.  Only the two sRGB-primaried
// spaces are valid here (Oklab is not a compositing space):
//   CANVAS_CS_SRGB        -- encoded sRGB; today's behaviour, byte for byte.
//                            NO transfer ever runs: entry, compositing, and
//                            exit are a literal bypass of the legacy path.
//   CANVAS_CS_LINEAR_SRGB -- extended linear sRGB; translucent overlaps
//                            composite in linear light (they stay bright rather
//                            than going muddy).  Untagged colours decode
//                            sRGB->linear on the way in and encode
//                            linear->sRGB on the way out; the only clamp is the
//                            output quantize, so extended (out-of-[0,1]) values
//                            propagate through compositing.
// Untagged colours are canonically ENCODED sRGB at every public boundary, in
// both spaces -- input (set_fill_rgba and siblings, gradient stops, shadow and
// drop-shadow colours, putImageData/bitmap/image pixels) and output (read_rgba,
// get_image_data).  The space changes only what they convert into internally,
// never how they are spelled at the boundary.  (write_png is separate: it has no
// colour parameter and emits 16-bit Rec.2020/PQ regardless -- see write_png.)
//
// A gradient's INTERPOLATION SPACE: the colour space its stop colours blend in
// between stops (the gradient interpolation setters).  Per gradient (not per
// canvas): it rides the fill/stroke gradient, so save/restore brackets it like
// any drawing state, and a fresh canvas_set_*_gradient resets it to the sRGB
// default.  The geometry of the gradient (which stop pair, how far between) is
// identical either way; only the colour lerp differs.  All THREE colour spaces
// are valid here (the orthogonal alpha knob, premul or unpremul, applies to
// each); an out-of-range value for either knob is ignored, leaving the
// gradient's interpolation untouched:
//   CANVAS_CS_SRGB        -- component-wise lerp of the stop colours as stored,
//                            today's behaviour byte for byte.  The default, so
//                            an untouched gradient -- and every legacy program
//                            with no interpolation line -- is unchanged.
//   CANVAS_CS_LINEAR_SRGB -- lerp in linear-light sRGB (the stops decode
//                            sRGB->linear, blend, encode back), so the ramp
//                            tracks physical light rather than the encoded
//                            values.
//   CANVAS_CS_OKLAB       -- lerp in PREMULTIPLIED Oklab (L,a,b each scaled by
//                            that stop's alpha, alpha lerped on its own,
//                            unpremultiplied before converting out) for a
//                            perceptually even ramp: the midpoint stays bright
//                            instead of going muddy/dark, and a transparent
//                            stop bleeds no colour into the ramp
//                            (transparent-red -> opaque-blue is pure blue at
//                            the midpoint).  Oklab is interpolation-only; the
//                            result re-enters the working-space compositing
//                            path unchanged.
enum canvas_color_space {
    CANVAS_CS_SRGB, CANVAS_CS_LINEAR_SRGB, CANVAS_CS_OKLAB,
};

// The constructor: NULL on failure; the canvas starts transparent black.
// canvas_free accepts NULL, like free() itself.  canvas(w,h) is the sRGB
// working space (today's behaviour); canvas_in_space picks the space explicitly.
struct canvas *__single canvas(int width, int height);
struct canvas *__single canvas_in_space(int width, int height,
                                        enum canvas_color_space space);
void canvas_free(struct canvas *__single cv);

// Whether the rendering context has been lost (matching isContextLost).  This
// headless renderer owns its backing store and never loses it, so it is always
// false; provided for API parity.
bool canvas_is_context_lost(struct canvas *__single cv);

// Resize the canvas to width x height (like assigning canvas.width/height): the
// bitmap is reallocated and cleared to transparent black, and the drawing state
// is reset to its defaults -- both per the spec.  Returns false, leaving the
// canvas untouched, on invalid dimensions or allocation failure.
bool canvas_resize(struct canvas *__single cv, int width, int height);

void canvas_save(struct canvas *__single cv);
void canvas_restore(struct canvas *__single cv);
// Reset the context to its initial state: empty the save/restore stack, restore
// every drawing-state field to its default, discard the current path, open the
// clip, and clear the bitmap to transparent black.
void canvas_reset(struct canvas *__single cv);

void canvas_translate(struct canvas *__single cv, float tx, float ty);
void canvas_scale(struct canvas *__single cv, float sx, float sy);
void canvas_rotate(struct canvas *__single cv, float radians);
void canvas_transform(struct canvas *__single cv,
                      float a, float b, float c, float d, float e, float f);
void canvas_set_transform(struct canvas *__single cv,
                          float a, float b, float c, float d, float e, float f);
void canvas_reset_transform(struct canvas *__single cv);
// The current transform (matching getTransform): the matrix built up by
// translate/scale/rotate/transform and reset by set_transform/reset_transform.
canvas_matrix canvas_get_transform(struct canvas *__single cv);

// Solid fill paint.  `space` names the colour space the (r,g,b) are given in
// (the alpha is plain coverage, space-agnostic).  CANVAS_CS_SRGB -- encoded
// sRGB, today's behaviour byte for byte -- is the common case; CANVAS_CS_LINEAR_SRGB
// and CANVAS_CS_OKLAB let a caller speak linear or perceptual colour directly.
// Whatever the space, the colour is converted into the canvas's working space on
// the way in (see canvas_color_space); the boundary spelling is the only thing
// the space changes, never the stored result's meaning.
void canvas_set_fill_rgba(struct canvas *__single cv, enum canvas_color_space space,
                          float r, float g, float b, float a);
void canvas_set_global_alpha(struct canvas *__single cv, float alpha);
// globalCompositeOperation: how subsequent fills/strokes/text/images combine with
// what's already there (default source-over).  Applies to every painted op except
// clear_rect and put_image_data.
void canvas_set_global_composite_operation(struct canvas *__single cv,
                                           enum canvas_composite_op op);

// Shadows: a blurred, offset, colour-tinted copy of each fill/stroke/text/image
// is painted underneath it.  A shadow is cast only when shadow_color's alpha is
// > 0 and there is some blur or offset.  shadow_blur is a Gaussian-style radius;
// it and the offsets are in device pixels and are NOT affected by the current
// transform (per spec).  Defaults: transparent shadow (off), 0 blur, 0 offset.
// Negative/non-finite blur and non-finite offsets are ignored, as the spec says.
void canvas_set_shadow_color_rgba(struct canvas *__single cv,
                                  enum canvas_color_space space,
                                  float r, float g, float b, float a);
void canvas_set_shadow_blur(struct canvas *__single cv, float blur);
void canvas_set_shadow_offset_x(struct canvas *__single cv, float offset);
void canvas_set_shadow_offset_y(struct canvas *__single cv, float offset);

// filter: the CSS filter functions, as a typed API (no CSS string parsing).
// Each canvas_add_filter_* appends one function to the context's filter list;
// the list applies in call order to every painted op (fills, strokes, text,
// images -- not clear_rect or put_image_data), filtering the drawing before
// its shadow is cast.  The list is part of the drawing state: save/restore
// brackets it, and reset/resize clear it.  set_filter_none (the default)
// clears the list.
//
// Amounts clamp like the spec: below 0 clamps to 0; grayscale/invert/opacity/
// sepia also cap at 1, while brightness/contrast/saturate are unbounded above.
// brightness/contrast/saturate/opacity at 1 are identity, grayscale/invert/
// sepia at 0, hue_rotate at 0 radians.  A non-finite amount is ignored (the
// call is a no-op), as the spec ignores unparseable filter values.
//
// blur(px): a Gaussian blur of the drawing with standard deviation `px`,
// approximated by three box passes over the op's premultiplied tile (the same
// approximation as shadow_blur, whose stdDev is blur/2 -- here px IS the
// stdDev).  The painted region grows by the blur's spread, so the soft edge
// extends past the shape's own bounds.  px is in device pixels; the transform
// does not apply (per spec, filter lengths ignore the CTM).  A negative or
// non-finite px is ignored, and px == 0 (an identity blur) appends nothing.
//
// drop_shadow(dx, dy, blur, colour): composites a blurred, offset,
// colour-tinted copy of the drawing's alpha silhouette UNDER the drawing --
// the entry's output is shadow-plus-drawing as one image, which any later
// entries in the list then filter (so a colour function after a drop_shadow
// recolours the shadow too).  blur is a Gaussian stdDev like blur(); dx/dy
// are device pixels, subpixel fractions honoured on a 1/256th-px grid like
// shadowOffset{X,Y} (per spec, filter coordinates and shadow offsets alike
// ignore the CTM).  The painted region grows by the shadow's
// offset and blur spread.  Colour channels follow set_fill_rgba: on an sRGB
// canvas they clamp to [0,1], on a linear canvas extended values carry (an HDR
// or wide-gamut shadow).  A non-finite dx/dy/blur or a negative blur is ignored
// (the call appends nothing), and so is a fully transparent colour (its shadow
// would composite as nothing).  Independent of the shadow_color state, which
// casts the op's own shadow from the tile's alpha after the whole filter
// list runs -- so a drop_shadow's skirt shapes that shadow too.
void canvas_set_filter_none(struct canvas *__single cv);
void canvas_add_filter_blur(struct canvas *__single cv, float px);
void canvas_add_filter_brightness(struct canvas *__single cv, float amount);
void canvas_add_filter_contrast(struct canvas *__single cv, float amount);
void canvas_add_filter_drop_shadow(struct canvas *__single cv,
                                   enum canvas_color_space space,
                                   float dx, float dy,
                                   float blur, float r, float g, float b,
                                   float a);
void canvas_add_filter_grayscale(struct canvas *__single cv, float amount);
void canvas_add_filter_hue_rotate(struct canvas *__single cv, float radians);
void canvas_add_filter_invert(struct canvas *__single cv, float amount);
void canvas_add_filter_opacity(struct canvas *__single cv, float amount);
void canvas_add_filter_saturate(struct canvas *__single cv, float amount);
void canvas_add_filter_sepia(struct canvas *__single cv, float amount);

// Set the fill paint to a gradient and clear its stops; fill() uses it until the
// next canvas_set_fill_rgba.  Coordinates are user space (the transform is baked in
// now).  Add stops with canvas_add_fill_color_stop; offsets clamp to [0,1].
void canvas_set_fill_linear_gradient(struct canvas *__single cv,
                                     float x0, float y0, float x1, float y1);
void canvas_set_fill_radial_gradient(struct canvas *__single cv, float x0, float y0,
                                     float r0, float x1, float y1, float r1);
// Conic gradient (createConicGradient): colours sweep clockwise around (x, y)
// from `start_angle` radians (measured from the +x axis).  Stops are added with
// canvas_add_fill_color_stop, as for the linear/radial gradients.
void canvas_set_fill_conic_gradient(struct canvas *__single cv, float start_angle,
                                    float x, float y);
void canvas_add_fill_color_stop(struct canvas *__single cv,
                                enum canvas_color_space space, float offset,
                                float r, float g, float b, float a);
// Choose the interpolation for the current fill gradient, two orthogonal knobs:
// the colour SPACE the stops lerp in (CANVAS_CS_SRGB / CANVAS_CS_LINEAR_SRGB /
// CANVAS_CS_OKLAB) and whether the colour coordinates are premultiplied by
// alpha before the lerp (CANVAS_ALPHA_PREMUL hygiene -- a transparent stop
// contributes no colour) or lerped directly (CANVAS_ALPHA_UNPREMUL).  Alpha
// itself always lerps linearly.  Default CANVAS_CS_SRGB + CANVAS_ALPHA_UNPREMUL
// (the legacy straight stored-value lerp, byte-for-byte).  Applies to whichever
// gradient is the fill paint now; a later canvas_set_fill_*_gradient resets it
// to the default, so set it after creating the gradient and before (or after)
// its stops.
void canvas_set_fill_gradient_interpolation(struct canvas *__single cv,
                                            enum canvas_color_space space,
                                            enum canvas_alpha_type alpha);
// Image pattern fill paint (createPattern): the tightly packed RGBA8 source
// (w*h, top row first) tiles the plane per `repeat`.  The source is *borrowed* --
// the caller must keep it alive while it remains the fill paint (including across
// save/restore) -- and the pattern is pinned in device space at the current
// transform, like the gradients.  Sampling honours image smoothing (bilinear vs
// nearest).  `space` tags how to interpret the source's colours -- the pattern
// is sampled in that space and the resolved sample converts to the canvas
// working space on deposit (a no-op when they match).  Ignored if the dimensions
// are non-positive or overflow.
void canvas_set_fill_pattern(struct canvas *__single cv, enum canvas_color_space space,
                             uint8_t const *__counted_by(w * h * 4) src,
                             int w, int h, enum canvas_pattern_repeat repeat);

void canvas_clear_rect(struct canvas *__single cv, float x, float y, float w, float h);
void canvas_fill_rect(struct canvas *__single cv, float x, float y, float w, float h);
// Stroke the outline of the rectangle with the current stroke style and line
// styles (width/join/cap/dash), without disturbing the current path.  Corners
// are transformed by the current transform (a rotated CTM strokes a rotated
// quad).  A zero-width-or-height rectangle degenerates to a stroked line.
void canvas_stroke_rect(struct canvas *__single cv, float x, float y, float w, float h);

// Path coordinates are transformed by the current transform as they are added.
void canvas_begin_path(struct canvas *__single cv);
void canvas_move_to(struct canvas *__single cv, float x, float y);
void canvas_line_to(struct canvas *__single cv, float x, float y);
void canvas_rect(struct canvas *__single cv, float x, float y, float w, float h);
void canvas_quadratic_curve_to(struct canvas *__single cv,
                               float cpx, float cpy, float x, float y);
void canvas_bezier_curve_to(struct canvas *__single cv, float c1x, float c1y,
                            float c2x, float c2y, float x, float y);
void canvas_arc(struct canvas *__single cv, float x, float y, float radius,
                float start_angle, float end_angle, bool anticlockwise);
void canvas_ellipse(struct canvas *__single cv, float x, float y, float rx, float ry,
                    float rotation, float start_angle, float end_angle,
                    bool anticlockwise);
void canvas_round_rect(struct canvas *__single cv, float x, float y, float w, float h,
                       float radius);
// roundRect with independent, elliptical per-corner radii.  Corners are given in
// CSS order -- top-left, top-right, bottom-right, bottom-left -- each as an (x,y)
// radius pair (equal x and y is a circular corner; 0 is a sharp corner).
// Negative/non-finite radii are treated as 0, and radii too large for the rect
// are scaled down proportionally (the CSS border-radius overlap rule).
void canvas_round_rect_radii(struct canvas *__single cv, float x, float y,
                             float w, float h,
                             float tl_x, float tl_y, float tr_x, float tr_y,
                             float br_x, float br_y, float bl_x, float bl_y);
void canvas_arc_to(struct canvas *__single cv, float x1, float y1, float x2, float y2,
                   float radius);
void canvas_close_path(struct canvas *__single cv);

// Fill the current path under `rule` -- the rule rides each call, as on the web
// (fill() defaults to nonzero THERE; a C caller spells it) -- handling holes
// and self-intersection.
void canvas_fill(struct canvas *__single cv, enum canvas_fill_rule rule);

// Intersect the clip region with the current path under `rule`.  Subsequent
// draws are masked to the running intersection of all clips; the region is
// part of the saved state, so save()/restore() brackets a clip.
void canvas_clip(struct canvas *__single cv, enum canvas_fill_rule rule);

// Hit testing: whether the point (x, y) is inside the current path under `rule`.
// (x, y) is transformed by the current transform before the test -- matching how
// path points are baked through the CTM as they are added -- so a point given in
// the same user space as the path hits.  Non-finite coordinates return false.
bool canvas_is_point_in_path(struct canvas *__single cv, float x, float y,
                             enum canvas_fill_rule rule);
// Whether (x, y) is inside the stroked area of the current path under the current
// line styles (width/join/cap/dash).  (x, y) is transformed by the current
// transform, as for is_point_in_path; non-finite coordinates return false.
bool canvas_is_point_in_stroke(struct canvas *__single cv, float x, float y);

// Path2D: a constructible path object, independent of the canvas and its current
// path.  Coordinates are recorded in user space and transformed by the canvas's
// current transform when the path is filled/stroked/clipped/hit-tested -- so the
// same Path2D draws differently under different transforms (unlike the current
// path, whose points are baked at build time).  NULL on allocation failure; free
// with canvas_path2d_free.
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

// Fill / stroke / clip / hit-test a Path2D (the fill rule is explicit here, not
// taken from state).  None of these disturb the canvas's current path.
void canvas_fill_path(struct canvas *__single cv, struct canvas_path2d const *__single p,
                      enum canvas_fill_rule rule);
void canvas_stroke_path(struct canvas *__single cv, struct canvas_path2d const *__single p);
void canvas_clip_path(struct canvas *__single cv, struct canvas_path2d const *__single p,
                      enum canvas_fill_rule rule);
bool canvas_is_point_in_path2d(struct canvas *__single cv, struct canvas_path2d const *__single p,
                               float x, float y, enum canvas_fill_rule rule);
bool canvas_is_point_in_stroke_path(struct canvas *__single cv,
                                    struct canvas_path2d const *__single p,
                                    float x, float y);

void canvas_set_stroke_rgba(struct canvas *__single cv, enum canvas_color_space space,
                            float r, float g, float b, float a);
// Gradient stroke paint, mirroring the fill gradient calls; stroke() uses it
// until the next canvas_set_stroke_rgba.  (Coordinates are baked through the
// transform now, as for fills.)
void canvas_set_stroke_linear_gradient(struct canvas *__single cv,
                                       float x0, float y0, float x1, float y1);
void canvas_set_stroke_radial_gradient(struct canvas *__single cv, float x0, float y0,
                                       float r0, float x1, float y1, float r1);
void canvas_set_stroke_conic_gradient(struct canvas *__single cv, float start_angle,
                                      float x, float y);
void canvas_add_stroke_color_stop(struct canvas *__single cv,
                                  enum canvas_color_space space, float offset,
                                  float r, float g, float b, float a);
// Interpolation space x alpha for the current stroke gradient (the fill twin above).
void canvas_set_stroke_gradient_interpolation(struct canvas *__single cv,
                                              enum canvas_color_space space,
                                              enum canvas_alpha_type alpha);
// Image pattern stroke paint, mirroring canvas_set_fill_pattern (the same
// `space` tag, sampled in-space and converted to the working space on deposit).
void canvas_set_stroke_pattern(struct canvas *__single cv, enum canvas_color_space space,
                               uint8_t const *__counted_by(w * h * 4) src,
                               int w, int h, enum canvas_pattern_repeat repeat);
void canvas_set_line_width(struct canvas *__single cv, float width);
void canvas_set_line_join(struct canvas *__single cv, enum canvas_line_join join);
void canvas_set_line_cap(struct canvas *__single cv, enum canvas_line_cap cap);
void canvas_set_miter_limit(struct canvas *__single cv, float limit);
// `pattern` lists alternating on/off lengths (user units); count 0 = solid.
void canvas_set_line_dash(struct canvas *__single cv,
                          float const *__counted_by(count) pattern, int count);
// Copy the current dash pattern into `out` (up to `cap` entries) and return its
// full length.  Pass cap 0 (out may be NULL) to query the length, then size a
// buffer; the returned float values are a copy, so mutating them is harmless.
int canvas_get_line_dash(struct canvas *__single cv,
                         float *__counted_by(cap) out, int cap);
void canvas_set_line_dash_offset(struct canvas *__single cv, float offset);
void canvas_stroke(struct canvas *__single cv);

// drawImage: a tightly packed RGBA8 source (sw*sh, top row first), sampled
// bilinearly, transformed by the current transform, composited source-over under
// the current global alpha and clip.  The three forms mirror the Canvas 2D
// overloads; the subrect form samples source rect [sx,sy,sw',sh'] into dest rect
// [dx,dy,dw,dh] (both user space).  Sampling is bilinear unless image smoothing
// is disabled (then nearest-neighbour).
//
// imageSmoothingEnabled: when true (default), drawImage samples bilinearly; when
// false, nearest-neighbour -- crisp, blocky upscaling with no blending.
void canvas_set_image_smoothing_enabled(struct canvas *__single cv, bool enabled);
// imageSmoothingQuality, applied while smoothing is enabled.  low (the spec
// default) is plain bilinear.  medium and high antialias minification: a
// drawImage whose source footprint passes one source px per device px samples
// a premultiplied mip pyramid (2x2 box-halved levels) with trilinear
// filtering -- a reified image's pyramid caches via the explicit
// canvas_image_build_mips (without it, the draw stays bilinear); a borrowed
// bitmap's rebuilds per such draw, the quality knob being the caller's
// opt-in to that cost.  high additionally upgrades magnification to a 4x4
// Catmull-Rom (the BC-spline family; the (B, C) pair is one line in canvas.c
// if Mitchell is wanted instead), its taps premultiplied so negative lobes
// cannot synthesize colour from transparent texels.
void canvas_set_image_smoothing_quality(struct canvas *__single cv,
                                        enum canvas_image_smoothing_quality quality);
// drawImage from a borrowed bitmap: straight (unpremultiplied) RGBA8, top row
// first, the caller's buffer for the duration of the call only.  Because a
// borrowed buffer has no identity to cache derived data against, a minifying
// draw at medium/high quality rebuilds its mip chain per call -- reified
// images below pay that cost once, explicitly.  `space` tags how to interpret
// the buffer's colours -- sampled in that space, the resolved sample converted
// to the canvas working space on deposit (a no-op when they match).
void canvas_draw_bitmap(struct canvas *__single cv, enum canvas_color_space space,
                       uint8_t const *__counted_by(sw * sh * 4) src,
                       int sw, int sh, float dx, float dy);
void canvas_draw_bitmap_scaled(struct canvas *__single cv, enum canvas_color_space space,
                              uint8_t const *__counted_by(sw * sh * 4) src,
                              int sw, int sh, float dx, float dy,
                              float dw, float dh);
void canvas_draw_bitmap_subrect(struct canvas *__single cv, enum canvas_color_space space,
                               uint8_t const *__counted_by(sw * sh * 4) src,
                               int sw, int sh, float sx, float sy,
                               float sww, float shh, float dx, float dy,
                               float dw, float dh);

// Reified images, in the Skia vocabulary: an image is a thing you draw FROM,
// a surface (the canvas) a thing you draw TO, a bitmap the raw RGBA8 memory
// realizing either.  An image owns a copy of its pixels, which gives them
// identity -- the hook derived data caches against.
struct canvas_image;

// Construct an image, one typed constructor per colour type, each taking its
// alpha type -- the four formats are peers, none favoured.  Pixels are RGBA,
// top row first, copied in (lossless: a 1:1 draw of an unorm8 unpremul image
// in the working space is byte-identical to drawing the bitmap directly).
// `space` tags how to interpret the pixels' colours: the image is filtered in
// that space (its mip chain too), and the resolved sample converts to the
// canvas working space on deposit -- a no-op when they match.  NULL on bad
// dimensions or allocation failure; free with canvas_image_free (which, like
// free(), accepts NULL).
struct canvas_image *__single canvas_image_unorm8(
    enum canvas_color_space space,
    uint8_t const *__counted_by(w * h * 4) px, int w, int h,
    enum canvas_alpha_type at);
struct canvas_image *__single canvas_image_f16(
    enum canvas_color_space space,
    _Float16 const *__counted_by(w * h * 4) px, int w, int h,
    enum canvas_alpha_type at);

// Snapshot a canvas as an image (canvas-as-source).  The
// surface is premultiplied f16 and so is the snapshot: a straight memcpy,
// bit-lossless, no quantize and no unpremultiply anywhere between surface
// and sample.  A copy: drawing on the canvas afterwards does not change the
// snapshot.  The snapshot's colour space is the canvas's working space (the
// snapped pixels ARE in the working space -- CANVAS_CS_SRGB
// for an sRGB canvas, CANVAS_CS_LINEAR_SRGB for a linear one).  NULL on OOM.
struct canvas_image *__single canvas_snapshot(struct canvas *__single cv);

// Build the image's mip pyramid -- the one-time cost that minifying draws at
// medium/high smoothing quality then sample with trilinear filtering.
// Deliberately explicit: WITHOUT built mips a minifying image draw falls
// back to plain bilinear rather than hiding a rebuild (the bitmap entry
// points' behaviour) -- the caller decides if and when to pay.  Idempotent;
// false on allocation failure (the image stays valid, mip-less).
bool canvas_image_build_mips(struct canvas_image *__single img);

int canvas_image_width(struct canvas_image const *__single img);
int canvas_image_height(struct canvas_image const *__single img);
void canvas_image_free(struct canvas_image *__single img);

// drawImage from a reified image: the bitmap trio's three overloads, same
// transform/clip/alpha/quality semantics, sourcing the image's pixels (and
// its cached mips, once built).
void canvas_draw_image(struct canvas *__single cv,
                       struct canvas_image const *__single img,
                       float dx, float dy);
void canvas_draw_image_scaled(struct canvas *__single cv,
                              struct canvas_image const *__single img,
                              float dx, float dy, float dw, float dh);
void canvas_draw_image_subrect(struct canvas *__single cv,
                               struct canvas_image const *__single img,
                               float sx, float sy, float sww, float shh,
                               float dx, float dy, float dw, float dh);

// Text.  The typeface is fixed to Libian TC (clerical-script 隸書), which carries
// both Latin and Chinese.  Size is user-space px (default 10).  fill_text and
// stroke_text lay out `text` (UTF-8) with its baseline origin at (x, y), advance
// +x, and paint the glyph outlines like fill()/stroke(): transform, clip, gradient
// and global alpha all apply.  measure_text returns the advance width in user px.
void canvas_set_font_size(struct canvas *__single cv, float px);
// letterSpacing: extra advance added after each typographic cluster, in user px
// (default 0; may be negative).  wordSpacing: extra advance added at each
// word-separator (U+0020 SPACE), in user px (default 0; may be negative).  Both
// affect the drawing pen position and measure_text/measure_text_full width.  A
// NaN or infinite value is treated as 0.  Part of the drawing state:
// save/restore brackets each, reset/resize restore the 0 default.
void canvas_set_letter_spacing(struct canvas *__single cv, float px);
void canvas_set_word_spacing(struct canvas *__single cv, float px);
// textAlign: horizontal placement of the text relative to the (x, y) passed to
// fill_text/stroke_text, by the advance width.  left puts x at the left edge,
// right at the right edge, center centres it; start (default) and end resolve
// through the direction attribute (start == left under ltr, == right under rtl;
// end the opposite).
void canvas_set_text_align(struct canvas *__single cv, enum canvas_text_align align);
// direction: the paragraph direction (default ltr).  It resolves textAlign
// start/end, and it is the base direction text is shaped under -- a
// mixed-direction string lays its runs out in the visual order that base
// implies, and neutrals (spaces, punctuation) resolve against it -- so the same
// string can draw differently under ltr and rtl.  Part of the drawing state:
// save/restore brackets it, reset/resize restore the default.
void canvas_set_direction(struct canvas *__single cv, enum canvas_direction dir);
// textBaseline: vertical placement of the baseline relative to (x, y).
// alphabetic (default) draws the baseline at y; top/hanging/middle/ideographic/
// bottom shift it by the font's ascent/descent.
void canvas_set_text_baseline(struct canvas *__single cv, enum canvas_text_baseline baseline);
float canvas_measure_text(struct canvas *__single cv, char const *__null_terminated text);

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

canvas_text_metrics canvas_measure_text_full(struct canvas *__single cv,
                                             char const *__null_terminated text);
void canvas_fill_text(struct canvas *__single cv, char const *__null_terminated text,
                      float x, float y);
void canvas_stroke_text(struct canvas *__single cv, char const *__null_terminated text,
                        float x, float y);
// Length-counted variants: `text` is `len` bytes of UTF-8 and need not be
// NUL-terminated, so a caller can pass a slice of a larger buffer directly.  The
// NUL-terminated fill_text/stroke_text above are conveniences over these.
void canvas_fill_text_n(struct canvas *__single cv, char const *__counted_by(len) text,
                        int len, float x, float y);
void canvas_stroke_text_n(struct canvas *__single cv, char const *__counted_by(len) text,
                          int len, float x, float y);
// fillText/strokeText with a maxWidth: when the text's advance exceeds a finite,
// positive `max_width`, it is condensed horizontally (scaled in x about the
// alignment anchor) to fit.  A non-positive or non-finite `max_width` imposes no
// limit, rendering exactly like canvas_fill_text/canvas_stroke_text.
void canvas_fill_text_max(struct canvas *__single cv, char const *__null_terminated text,
                          float x, float y, float max_width);
void canvas_stroke_text_max(struct canvas *__single cv, char const *__null_terminated text,
                            float x, float y, float max_width);
// Length-counted variants of the maxWidth path (the slice forms replay hands
// its text tail to, and the NUL-terminated conveniences delegate to).
void canvas_fill_text_max_n(struct canvas *__single cv, char const *__counted_by(len) text,
                            int len, float x, float y, float max_width);
void canvas_stroke_text_max_n(struct canvas *__single cv,
                              char const *__counted_by(len) text,
                              int len, float x, float y, float max_width);

// Tightly packed RGBA8, top row first; len must be width*height*4.  `space` names
// the OUTPUT colour space (see canvas_color_space): CANVAS_CS_SRGB is today's
// behaviour byte for byte (and the common case); CANVAS_CS_LINEAR_SRGB emits
// linear-sRGB bytes and CANVAS_CS_OKLAB emits Oklab (L,a,b) bytes.
void canvas_read_rgba(struct canvas *__single cv, enum canvas_color_space space,
                      uint8_t *__counted_by(len) out, int len);
// canvas_write_png emits a BT.2100 PNG: 16-bit, Rec.2020 primaries, PQ (ST 2084)
// transfer, cICP-signalled, regardless of the working space (the surface is
// transformed into that encoding on the way out -- wide-gamut and HDR values
// from a linear canvas carry through, an sRGB canvas's content lands in the same
// container).  There is no decoder; PNG is output only.
bool canvas_write_png(struct canvas *__single cv, char const *__null_terminated path);

// canvas_write_png's in-memory sibling: the same BT.2100 PNG bytes in a malloc'd
// buffer (caller frees), *outlen its length.  NULL on OOM.
uint8_t *__counted_by_or_null(*outlen)
canvas_encode_png(struct canvas *__single cv, int *__single outlen);

// Pixel I/O for a w*h sub-image (tightly packed RGBA8, len must be w*h*4).
// get: pixels outside the canvas read back transparent black.
// put: overwrites (no blending), clipped to the canvas.
// `space` (see canvas_color_space) is the space the bytes are in: for get, the
// OUTPUT space (routed through read_rgba); for put, the space the INCOMING bytes
// are interpreted in.  CANVAS_CS_SRGB is today's behaviour byte for byte.
void canvas_get_image_data(struct canvas *__single cv,
                           enum canvas_color_space space,
                           int x, int y, int w, int h,
                           uint8_t *__counted_by(len) out, int len);
void canvas_put_image_data(struct canvas *__single cv,
                           enum canvas_color_space space,
                           uint8_t const *__counted_by(len) data, int len,
                           int w, int h, int dx, int dy);
// putImageData with a dirty rectangle: only the source sub-rect
// [dirtyX, dirtyX+dirtyWidth) x [dirtyY, dirtyY+dirtyHeight) (ImageData
// coordinates) is written, still placed with the image origin at (dx, dy).  The
// dirty rect is normalised like the spec -- negative extents flip, then it is
// clamped to the source; an empty result is a no-op.  `space` is the incoming
// bytes' space, as for canvas_put_image_data.
void canvas_put_image_data_dirty(struct canvas *__single cv,
                                 enum canvas_color_space space,
                                 uint8_t const *__counted_by(len) data, int len,
                                 int w, int h, int dx, int dy,
                                 int dirty_x, int dirty_y,
                                 int dirty_w, int dirty_h);

// The rgba-float16 ImageData twins (the spec's `rgba-float16` storage format):
// straight (unpremultiplied) _Float16 RGBA instead of u8, `len` the ELEMENT
// count (w*h*4) rather than a byte count.  Unlike the u8 path these preserve
// EXTENDED range -- HDR (>1) and wide-gamut (negative) colour a CANVAS_CS_LINEAR_SRGB
// canvas stores are neither clamped to [0,1] nor quantized to 8-bit.  `space`
// (see canvas_color_space) is the f16 values' colour space, with the same
// meaning as the u8 versions: for get the OUTPUT space, for put the space the
// incoming values are interpreted in.  On a linear canvas extended values
// survive end to end; on an sRGB canvas they bound through the sRGB encode/blend.
void canvas_get_image_data_f16(struct canvas *__single cv,
                               enum canvas_color_space space,
                               int x, int y, int w, int h,
                               _Float16 *__counted_by(len) out, int len);
void canvas_put_image_data_f16(struct canvas *__single cv,
                               enum canvas_color_space space,
                               _Float16 const *__counted_by(len) data, int len,
                               int w, int h, int dx, int dy);
void canvas_put_image_data_dirty_f16(struct canvas *__single cv,
                                     enum canvas_color_space space,
                                     _Float16 const *__counted_by(len) data, int len,
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
// set_text_align / set_text_baseline / set_direction /
// set_image_smoothing_quality / the pattern repeat modes) are written by
// name; fill and clip carry their fill rule by name (`fill nonzero`,
// `clip evenodd`), like fill_path/clip_path; bool args (arc/ellipse winding,
// set_image_smoothing_enabled) are 0/1
// or false/true; set_line_dash takes a variable number of lengths; the text
// ops take the rest of the line as their text.  The int-typed ops
// (put_image_data placement and dirty rects, resize) write integers.  reset
// records as itself -- it restarts the file's font-id space along with the
// text cache it clears -- and resize (which records only when it succeeds)
// implies the same.  Blank lines and lines whose first non-space character
// is `#` are ignored.
//
// Colour ops -- set_fill_rgba, set_stroke_rgba, set_shadow_color_rgba,
// add_fill_color_stop, add_stroke_color_stop, add_filter_drop_shadow -- carry
// an OPTIONAL trailing colour-space token (srgb|linear|oklab) after their
// floats, the space the (r,g,b[,a]) are given in (see canvas_color_space).  It
// is emitted ONLY when the space is non-sRGB; absence means sRGB, so an sRGB
// colour records exactly as it always has and every existing .canvas stays
// byte-identical:
//     set_fill_rgba 0.8 0.2 0.2 1            # sRGB (no token)
//     set_fill_rgba 0.8 0.2 0.2 1 oklab      # tagged
// put_image_data / put_image_data_dirty carry their input's space the same
// emit-when-non-sRGB way, but on the image BLOCK's optional <space> tag (see
// below), not the op line, since their pixels ride a block.  An unknown
// trailing token is rejected (strict parsing, below).
//
// Text-block lines make a recorded program self-contained (no fonts needed to
// replay): the recorder writes, ahead of each text op that first needs them,
//     font <id> <ascent> <descent> <name...>
//     glyph <font-id> <gid> <units-per-em> <x0 y0 x1 y1> <m/l/q/c/z curves...>
//     bitmap <font-id> <gid> <w> <h> <x0 y0 x1 y1> <zlen> <nlines>
//     bits <base64...>                            (exactly nlines of these)
//     shaping <size_px> <rtl 0|1> <ls> <ws> <utf16-len> <nruns> <byte-len> <text...>
//     run <font-id|-1> <rtl 0|1> <color 0|1> <nglyphs> (gid adv cluster)*
// font declares a file-local font id: its name (rest of the line, spaces
// allowed) and vertical metrics at size 1.0.  glyph carries one glyph's
// canonical outline -- verbs + control points and its ink box, all in FONT
// UNITS (units-per-em to the em, y up, baseline-relative), so one block serves
// every size and transform.  bitmap + its immediately-following bits lines
// carry one color (emoji) glyph's canonical capture: w x h premultiplied RGBA8
// rendered once at a fixed 160 px to the em, with its ink box in capture px --
// likewise one block per glyph serving every size and transform -- DEFLATED
// (an in-house zlib stream of exactly zlen bytes that inflates back to exactly
// w*h*4) and base64-chunked, because the raw capture (160x160x4 bytes, ~137 KB
// encoded) cannot fit one line and the compressed stream is a third to half
// the bytes.  shaping + its immediately-following run lines carry one
// shaped line (the text is exactly byte-len raw bytes after one space; the
// rtl token is the paragraph direction the line was shaped under, and ls/ws
// the letterSpacing/wordSpacing -- all parts of the cache key, since the same
// bytes shape and space differently under each.  ls/ws are always written, even
// when 0; the spacing is already baked into the run advances, so they only key
// the rebuilt line).  Replaying the blocks pre-populates the canvas's text cache, so the text ops
// that follow never call the platform text system -- a recorded program,
// emoji included, replays byte-identically on machines without the recording
// machine's fonts.
//
// Image-block lines carry the RGBA8 sources the image ops need, the capture
// machinery reused wholesale:
//     image <id> <unorm8|f16> <unpremul|premul> <w> <h> <zlen> <nlines> [<space>]
//     bits <base64...>                            (exactly nlines of these)
// declares file-local numbered image <id>, its colour and alpha types named
// on the line like every other enum in the format -- the four combinations
// are peers, none the unmarked default (a recorded canvas_snapshot is
// `f16 premul`, bit-lossless in the file too) -- w x h x 4 channels,
// deflated and base64-chunked exactly like a capture, content-deduplicated
// by the recorder so one buffer used many ways costs one block.  The trailing
// <space> (srgb|linear|oklab) is the source's colour-space tag -- OPTIONAL,
// emitted only when non-sRGB; absence means sRGB, so every legacy image block
// stays byte-identical.  (The tag is honoured on replay: the source is sampled
// in that space and the resolved sample converts to the working space on
// deposit.)  An optional
//     image_mips <id>
// line marks the block's draws as carrying mip-chain semantics: the bitmap
// entry points (per-draw chain rebuild) emit it as soon as their block is
// declared, and an image-object draw emits it only once
// canvas_image_build_mips has run -- so a mip-less image's bilinear-fallback
// draws replay faithfully (the replayer rebuilds the chain per draw,
// byte-identical to a live cached one).  The ops reference blocks by id (the
// source dims come from the block; put_image_data's int-typed args are
// written as integers):
//     draw_image <id> <dx> <dy>
//     draw_image_scaled <id> <dx> <dy> <dw> <dh>
//     draw_image_subrect <id> <sx> <sy> <sw> <sh> <dx> <dy> <dw> <dh>
//     put_image_data <id> <dx> <dy>
//     put_image_data_dirty <id> <dx> <dy> <dirty-x> <dirty-y> <dirty-w> <dirty-h>
//     set_fill_pattern <id> <repeat|repeat-x|repeat-y|no-repeat>
//     set_stroke_pattern <id> <repeat...>
// The format speaks in images: both the bitmap and reified-image draw trios
// record as the same draw_image* spellings, a block plus op lines.  A
// replayed image lives as long as the canvas (a pattern borrows its source),
// and one program declares at most 256 images of at most 64 MiB (w*h*4) each
// -- both validated before the block's buffers are allocated.
//
// Path-block lines carry Path2D objects the same numbered way:
//     path <id> <ncmds>
//     <verb> <args...>                            (exactly ncmds of these)
// declares file-local numbered path <id>; each command line is one builder
// call -- m/l <x y>, q <cpx cpy x y>, c <c1x c1y c2x c2y x y>,
// a <x y r sa ea ccw>, e <x y rx ry rot sa ea ccw>, t <x1 y1 x2 y2 r>
// (arcTo), r <x y w h> (rect), rr <x y w h radius>, z (closePath) -- in user
// space, transformed by the CTM at draw time like any Path2D.  The recorder
// serializes a path at its first fill_path/stroke_path/clip_path (the
// builders are canvas-free, so there is nothing to hook until a draw),
// deduplicated by content so one object stamped under many transforms costs
// one block, at most 256 per program.  addPath needs no spelling of its own:
// a Path2D holds its composed command list, which is what serializes.  The
// ops reference the id, the fill rule explicit where the API takes one:
//     fill_path <id> <nonzero|evenodd>
//     stroke_path <id>
//     clip_path <id> <nonzero|evenodd>
//
// Parsing is strict: an unknown command, a bad argument, an unknown trailing
// colour-space token on a colour op or image block, an over-long line, or
// a malformed block (an undeclared font id, a bad verb token, a cluster index
// past the text length, a non-finite number where the recorder writes finite
// ones, a bitmap whose bits lines are miscounted, mis-padded, or decode to
// anything but exactly zlen bytes, or whose declared stream is no zlib at all
// or inflates to anything but exactly w*h*4 bytes) stops replay and returns
// false (commands before the faulty line have already been applied; the canvas
// remains valid and drawable).  Numbers are
// written with %.9g and reparse to the identical float32.  The pure queries
// (measure_text, is_point_in_*, get_image_data, read_rgba, write_png) move no
// pixels and are not part of the text format.
bool canvas_replay_from(struct canvas *__single cv, char const *__null_terminated path);

// Begin recording subsequent drawing calls to `path` as a text canvas-program in
// exactly the format canvas_replay_from reads -- the write-side inverse of replay.
// Each recordable call is appended as one line (the canvas_* name minus the
// canvas_ prefix, then its arguments); replaying the file onto a fresh canvas of
// the same size reproduces the same image.  Recording continues until the canvas
// is destroyed (or record_to is called again, which starts a new file); pass the
// same `path` again only after the first is closed.  Returns false (recording
// nothing) if the file cannot be opened.
//
// Text ops round-trip self-contained: before each fill_text/stroke_text the
// recorder emits the font/glyph/bitmap/shaping blocks (see canvas_replay_from)
// for the derived data the op uses -- the shaped runs, each glyph's canonical
// font-unit curves and ink bounds, and each color (emoji) glyph's canonical
// capture (one fixed-size RGBA8 render per glyph, fetched at record time if
// the draw hasn't already, deflated on the way out so a one-emoji program
// runs tens of KB rather than ~137) -- deduplicated within the file, so a recorded
// program replays byte-identically (pixels and measureText alike) on a
// machine without the recording machine's fonts, with no platform text call
// at all, emoji included.
//
// Image ops round-trip the same way: draw_image (all three forms),
// put_image_data (plain + dirty-rect), and set_fill_pattern/set_stroke_pattern
// embed their RGBA8 source as an `image` block (deflated, deduplicated by
// content within the file) and write one op line referencing it.  Path2D
// draws likewise: fill_path/stroke_path/clip_path serialize the path's
// command list as a content-deduplicated `path` block at first use and write
// one op line referencing it (the builders themselves record nothing -- they
// have no canvas -- and the hit-test overloads, being queries, record
// nothing either).  See canvas_replay_from for the caps past which an image
// or path degrades un-recorded.
//
// EVERY pixel-affecting public op records -- the same set replay_from
// understands; there is no excluded op.  The compound path helpers
// (arc/round_rect/round_rect_radii/arc_to) are written as themselves, not
// their expansion; a setter that ignores its call per spec (a non-finite
// shadow offset or filter amount, an invalid resize) records nothing for it;
// and the pure queries (measure_text, is_point_in_*, get_image_data,
// read_rgba, write_png, get_transform, get_line_dash) move no pixels and are
// not part of the format.
bool canvas_record_to(struct canvas *__single cv, char const *__null_terminated path);

// createImageData: allocate a blank (transparent black) RGBA8 image of sw*sh
// pixels -- the layout get/put_image_data use.  Returns a freshly malloc'd,
// zeroed buffer of sw*sh*4 bytes (free it with free()) and stores that byte
// length in *len, ready to hand to put_image_data.  On non-positive dimensions,
// a size that would overflow, or out of memory, returns NULL and stores 0.
// The image is independent of any canvas, so none is taken.
uint8_t *__counted_by_or_null(*len)
canvas_create_image_data(int sw, int sh, int *__single len);

// createImageData's rgba-float16 twin: a blank (transparent black) _Float16
// RGBA image of sw*sh pixels -- the layout get/put_image_data_f16 use.  Returns
// a freshly calloc'd buffer of sw*sh*4 _Float16 elements (free it with free())
// and stores that ELEMENT count in *len, ready to hand to put_image_data_f16.
// On non-positive dimensions, a size that would overflow, or out of memory,
// returns NULL and stores 0.
_Float16 *__counted_by_or_null(*len)
canvas_create_image_data_f16(int sw, int sh, int *__single len);
