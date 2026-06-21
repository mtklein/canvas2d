#pragma once

// A C implementation of a subset of the HTML Canvas 2D API.  Coordinates are
// pixels, origin top-left, +y down, matching the web platform.

#include "canvas2d_paint_style.h"  // fill_rule, line_join, line_cap (shared with the leaf modules)

#include <ptrcheck.h>
#include <stdint.h>

struct canvas2d_context;  // the rendering context: canvas2d() constructs, canvas2d_free() frees
                // (see canvas2d() below; the working colour space is required)

// The six components of a 2D affine transform: (x,y) maps to
// (a*x + c*y + e, b*x + d*y + f) -- the argument order of canvas2d_set_transform.
typedef struct { float a, b, c, d, e, f; } canvas2d_matrix;

// textAlign / textBaseline.  start/end resolve against the direction attribute:
// start == left and end == right under ltr, the opposite under rtl.
enum canvas2d_text_align {
    CANVAS2D_ALIGN_START, CANVAS2D_ALIGN_END,
    CANVAS2D_ALIGN_LEFT, CANVAS2D_ALIGN_RIGHT, CANVAS2D_ALIGN_CENTER,
};
// direction: the paragraph direction for text.  Headless, so no "inherit"; the
// default is ltr, matching what inherit resolves to in an undirected document.
enum canvas2d_direction { CANVAS2D_DIRECTION_LTR, CANVAS2D_DIRECTION_RTL };
enum canvas2d_text_baseline {
    CANVAS2D_BASELINE_ALPHABETIC, CANVAS2D_BASELINE_TOP, CANVAS2D_BASELINE_HANGING,
    CANVAS2D_BASELINE_MIDDLE, CANVAS2D_BASELINE_IDEOGRAPHIC, CANVAS2D_BASELINE_BOTTOM,
};
// fontStyle: upright (normal) or slanted (italic).  An unrecognized value passed
// to canvas2d_set_font_style is ignored, keeping the current style.
enum canvas2d_font_style { CANVAS2D_FONT_STYLE_NORMAL, CANVAS2D_FONT_STYLE_ITALIC };

// fontKerning: whether the shaper applies the font's kerning (the pair-adjusted
// advances a kerned face carries).  auto (the default) and normal both leave
// Core Text's default kerning on; none disables it (advances become the
// unkerned per-glyph defaults).  An unrecognized value passed to
// canvas2d_set_font_kerning is ignored, keeping the current setting.
enum canvas2d_font_kerning {
    CANVAS2D_FONT_KERNING_AUTO, CANVAS2D_FONT_KERNING_NORMAL, CANVAS2D_FONT_KERNING_NONE,
};
// textRendering: a hint trading shaping richness for speed.  auto (the default),
// optimizeLegibility, and geometricPrecision all leave Core Text's default
// kerning and ligatures on; optimizeSpeed disables BOTH (a pragmatic mapping --
// Core Text exposes no single speed knob, so "speed" is spelled as no kerning
// and no ligatures).  An unrecognized value is ignored, keeping the current
// setting.
enum canvas2d_text_rendering {
    CANVAS2D_TEXT_RENDERING_AUTO, CANVAS2D_TEXT_RENDERING_OPTIMIZE_SPEED,
    CANVAS2D_TEXT_RENDERING_OPTIMIZE_LEGIBILITY,
    CANVAS2D_TEXT_RENDERING_GEOMETRIC_PRECISION,
};

// fontVariantCaps: small-capitals glyph selection.  normal (the default) leaves
// the text unchanged; small_caps applies the font's smcp feature (lowercase
// letters draw as small capitals); all_small_caps applies smcp AND c2sc (both
// lowercase and uppercase draw as small capitals).  This SELECTS substitute
// glyphs within the SAME resolved font, so it changes shaping (advances and
// glyph ids), not the resolved face -- a font with no smcp feature is a no-op.
// An unrecognized value passed to canvas2d_set_font_variant_caps is ignored.
enum canvas2d_font_variant_caps {
    CANVAS2D_FONT_VARIANT_CAPS_NORMAL, CANVAS2D_FONT_VARIANT_CAPS_SMALL_CAPS,
    CANVAS2D_FONT_VARIANT_CAPS_ALL_SMALL_CAPS,
};
// fontStretch: the typeface's width on the nine-keyword CSS axis, normal (the
// default) the centre.  It maps onto Core Text's width trait (condensed
// negative, expanded positive) so the descriptor resolves a real WIDTH face of
// the family (e.g. "Avenir Next Condensed") or a variable-width instance -- a
// distinct resolved name, so the glyph cache separates it.  No width is
// synthesized: a family with no width face stays at its nearest face (a no-op).
// An unrecognized value passed to canvas2d_set_font_stretch is ignored.
enum canvas2d_font_stretch {
    CANVAS2D_FONT_STRETCH_ULTRA_CONDENSED, CANVAS2D_FONT_STRETCH_EXTRA_CONDENSED,
    CANVAS2D_FONT_STRETCH_CONDENSED, CANVAS2D_FONT_STRETCH_SEMI_CONDENSED,
    CANVAS2D_FONT_STRETCH_NORMAL,
    CANVAS2D_FONT_STRETCH_SEMI_EXPANDED, CANVAS2D_FONT_STRETCH_EXPANDED,
    CANVAS2D_FONT_STRETCH_EXTRA_EXPANDED, CANVAS2D_FONT_STRETCH_ULTRA_EXPANDED,
};

// imageSmoothingQuality (see canvas2d_set_image_smoothing_quality).
enum canvas2d_image_smoothing_quality {
    CANVAS2D_SMOOTHING_LOW, CANVAS2D_SMOOTHING_MEDIUM, CANVAS2D_SMOOTHING_HIGH,
};

// createPattern repetition mode.
enum canvas2d_pattern_repeat {
    CANVAS2D_REPEAT, CANVAS2D_REPEAT_X, CANVAS2D_REPEAT_Y, CANVAS2D_NO_REPEAT,
};

// Image pixel formats, two orthogonal axes (the Skia colorType/alphaType
// split): how a channel is stored, and whether colour rides premultiplied by
// alpha.  The canvas surface itself stays locked premultiplied
// f16; images parameterize over this 2x2, with no third axis or format
// until a real need shows up.
enum canvas2d_color_type { CANVAS2D_COLOR_UNORM8, CANVAS2D_COLOR_F16 };
enum canvas2d_alpha_type { CANVAS2D_ALPHA_UNPREMUL, CANVAS2D_ALPHA_PREMUL };

// globalCompositeOperation.  The blend kernels (canvas2d.c) dispatch on this
// order directly: the first 11 are Porter-Duff operators, then the separable
// blend modes, then the four non-separable ones.
enum canvas2d_composite_op {
    CANVAS2D_OP_SOURCE_OVER, CANVAS2D_OP_SOURCE_IN, CANVAS2D_OP_SOURCE_OUT,
    CANVAS2D_OP_SOURCE_ATOP, CANVAS2D_OP_DESTINATION_OVER, CANVAS2D_OP_DESTINATION_IN,
    CANVAS2D_OP_DESTINATION_OUT, CANVAS2D_OP_DESTINATION_ATOP, CANVAS2D_OP_XOR,
    CANVAS2D_OP_LIGHTER, CANVAS2D_OP_COPY,
    CANVAS2D_OP_MULTIPLY, CANVAS2D_OP_SCREEN, CANVAS2D_OP_OVERLAY, CANVAS2D_OP_DARKEN,
    CANVAS2D_OP_LIGHTEN, CANVAS2D_OP_COLOR_DODGE, CANVAS2D_OP_COLOR_BURN,
    CANVAS2D_OP_HARD_LIGHT, CANVAS2D_OP_SOFT_LIGHT, CANVAS2D_OP_DIFFERENCE,
    CANVAS2D_OP_EXCLUSION,
    CANVAS2D_OP_HUE, CANVAS2D_OP_SATURATION, CANVAS2D_OP_COLOR, CANVAS2D_OP_LUMINOSITY,
};

// The colour spaces this API names.  ONE enum spanning every role; each
// surface that takes a space accepts only the valid SUBSET for that role
// (canvas takes the two compositing spaces and returns NULL otherwise; a
// gradient's interpolation argument takes all three).  Neither asserts.
//   CANVAS2D_CS_SRGB        -- encoded sRGB.
//   CANVAS2D_CS_LINEAR_SRGB -- extended linear sRGB (same primaries, linear
//                            transfer).
//   CANVAS2D_CS_OKLAB       -- Oklab; a perceptual interpolation space, not a
//                            compositing space.
//
// The canvas's WORKING COLOUR SPACE (canvas): the space boundary colours are
// converted INTO for compositing.  Chosen at creation and immutable -- it lives
// on the canvas, not the save/restore drawing state, and reset/resize leave it
// alone.  The two sRGB-primaried spaces are equal peer choices here (Oklab is
// not a compositing space):
//   CANVAS2D_CS_SRGB        -- encoded sRGB.  Compositing happens directly on the
//                            encoded bytes: no transfer runs at entry,
//                            compositing, or exit, so an encoded-sRGB colour or
//                            pixel passes through as-is.
//   CANVAS2D_CS_LINEAR_SRGB -- extended linear sRGB; translucent overlaps
//                            composite in linear light (they stay bright rather
//                            than going muddy).  Encoded-sRGB boundary colours
//                            decode sRGB->linear on the way in and encode
//                            linear->sRGB on the way out; the only clamp is the
//                            output quantize, so extended (out-of-[0,1]) values
//                            propagate through compositing.
// The boundary SPELLING is fixed independently of the working space: a colour or
// pixel given as CANVAS2D_CS_SRGB is encoded sRGB, as CANVAS2D_CS_LINEAR_SRGB linear
// sRGB, as CANVAS2D_CS_OKLAB Oklab, at every public boundary -- input
// (set_fill_rgba and siblings, gradient stops, shadow and drop-shadow colours,
// putImageData/bitmap/image pixels) and output (read_rgba, get_image_data).  The
// working space changes only what those values convert into internally, never
// how they are spelled at the boundary.  (write_png is separate: it has no
// colour parameter and emits 16-bit Rec.2020/PQ regardless -- see write_png.)
//
// A gradient's INTERPOLATION SPACE: the colour space its stop colours blend in
// between stops, a required choice at the gradient's creation (the first
// argument of every canvas2d_set_*_gradient).  Per gradient (not per canvas): it
// rides the fill/stroke gradient, so save/restore brackets it like any drawing
// state.  The geometry of the gradient (which stop pair, how far between) is
// identical across the three; only the colour lerp differs.  All THREE colour
// spaces are equal peers here (the orthogonal alpha knob, premul or unpremul,
// applies to each):
//   CANVAS2D_CS_SRGB        -- component-wise lerp of the stop colours as stored.
//   CANVAS2D_CS_LINEAR_SRGB -- lerp in linear-light sRGB (the stops decode
//                            sRGB->linear, blend, encode back), so the ramp
//                            tracks physical light rather than the encoded
//                            values.
//   CANVAS2D_CS_OKLAB       -- lerp in PREMULTIPLIED Oklab (L,a,b each scaled by
//                            that stop's alpha, alpha lerped on its own,
//                            unpremultiplied before converting out) for a
//                            perceptually even ramp: the midpoint stays bright
//                            instead of going muddy/dark, and a transparent
//                            stop bleeds no colour into the ramp
//                            (transparent-red -> opaque-blue is pure blue at
//                            the midpoint).  Oklab is interpolation-only; the
//                            result re-enters the working-space compositing
//                            path unchanged.
enum canvas2d_color_space {
    CANVAS2D_CS_SRGB, CANVAS2D_CS_LINEAR_SRGB, CANVAS2D_CS_OKLAB,
};

// The constructor: NULL on failure; the canvas starts transparent black.
// canvas2d_free accepts NULL, like free() itself.  `space` is the working colour
// space (see canvas2d_color_space), a required choice between the two compositing
// spaces; NULL is returned for a non-compositing space (CANVAS2D_CS_OKLAB) or bad
// dimensions.
struct canvas2d_context *__single canvas2d(int width, int height,
                               enum canvas2d_color_space space);
void canvas2d_free(struct canvas2d_context *__single cv);

// Whether the rendering context has been lost (matching isContextLost).  This
// headless renderer owns its backing store and never loses it, so it is always
// false; provided for API parity.
bool canvas2d_is_context_lost(struct canvas2d_context *__single cv);

// Resize the canvas to width x height (like assigning canvas.width/height): the
// bitmap is reallocated and cleared to transparent black, and the drawing state
// is reset to its defaults -- both per the spec.  Returns false, leaving the
// canvas untouched, on invalid dimensions or allocation failure.
bool canvas2d_resize(struct canvas2d_context *__single cv, int width, int height);

void canvas2d_save(struct canvas2d_context *__single cv);
void canvas2d_restore(struct canvas2d_context *__single cv);
// Reset the context to its initial state: empty the save/restore stack, restore
// every drawing-state field to its default, discard the current path, open the
// clip, and clear the bitmap to transparent black.
void canvas2d_reset(struct canvas2d_context *__single cv);

void canvas2d_translate(struct canvas2d_context *__single cv, float tx, float ty);
void canvas2d_scale(struct canvas2d_context *__single cv, float sx, float sy);
void canvas2d_rotate(struct canvas2d_context *__single cv, float radians);
void canvas2d_transform(struct canvas2d_context *__single cv,
                      float a, float b, float c, float d, float e, float f);
void canvas2d_set_transform(struct canvas2d_context *__single cv,
                          float a, float b, float c, float d, float e, float f);
void canvas2d_reset_transform(struct canvas2d_context *__single cv);

// Projective (perspective) transforms -- a deliberate extension beyond the
// affine Canvas 2D spec (docs/decisions/perspective.md).  The CTM is a 3x3
// homography applied to (x, y, 1):
//     x' = (a*x + c*y + e) / w,  y' = (b*x + d*y + f) / w,  w = g*x + h*y + i.
// Affine is the (g, h, i) = (0, 0, 1) subset; the six-argument setters above set
// it and stay on a divide-free, byte-identical path.  Geometry (fills, strokes,
// text) and sampling (gradients/images/patterns) both render projectively; the
// affine subset keeps the divide-free linear path.
void canvas2d_set_transform_3x3(struct canvas2d_context *__single cv, float a, float b, float c,
                              float d, float e, float f, float g, float h, float i);
void canvas2d_transform_3x3(struct canvas2d_context *__single cv, float a, float b, float c,
                          float d, float e, float f, float g, float h, float i);

// Set the CTM to the homography that maps the source rect (sx, sy, sw, sh)
// corners onto four destination points, corner order TL=(x0,y0), TR=(x1,y1),
// BR=(x2,y2), BL=(x3,y3).  A degenerate destination (sw or sh zero, or collinear
// destination corners) leaves the CTM unchanged.
void canvas2d_set_perspective_quad(struct canvas2d_context *__single cv, float sx, float sy,
                                 float sw, float sh, float x0, float y0,
                                 float x1, float y1, float x2, float y2,
                                 float x3, float y3);

// The current transform (matching getTransform): the matrix built up by
// translate/scale/rotate/transform and reset by set_transform/reset_transform.
// Reports only the affine (a..f) part of the CTM.
canvas2d_matrix canvas2d_get_transform(struct canvas2d_context *__single cv);

// Solid fill paint.  `space` names the colour space the (r,g,b) are given in
// (the alpha is plain coverage, space-agnostic).  The three spaces are peers:
// CANVAS2D_CS_SRGB takes encoded sRGB, CANVAS2D_CS_LINEAR_SRGB linear-light sRGB,
// and CANVAS2D_CS_OKLAB perceptual Oklab.  Whatever the space, the colour is
// converted into the canvas's working space on the way in (see
// canvas2d_color_space); the boundary spelling is the only thing the space
// changes, never the stored result's meaning.
void canvas2d_set_fill_rgba(struct canvas2d_context *__single cv, enum canvas2d_color_space space,
                          float r, float g, float b, float a);
void canvas2d_set_global_alpha(struct canvas2d_context *__single cv, float alpha);
// globalCompositeOperation: how subsequent fills/strokes/text/images combine with
// what's already there (default source-over).  Applies to every painted op except
// clear_rect and put_image_data.
void canvas2d_set_global_composite_operation(struct canvas2d_context *__single cv,
                                           enum canvas2d_composite_op op);

// Shadows: a blurred, offset, colour-tinted copy of each fill/stroke/text/image
// is painted underneath it.  A shadow is cast only when shadow_color's alpha is
// > 0 and there is some blur or offset.  shadow_blur is a Gaussian-style radius;
// it and the offsets are in device pixels and are NOT affected by the current
// transform (per spec).  Defaults: transparent shadow (off), 0 blur, 0 offset.
// Negative/non-finite blur and non-finite offsets are ignored, as the spec says.
void canvas2d_set_shadow_color_rgba(struct canvas2d_context *__single cv,
                                  enum canvas2d_color_space space,
                                  float r, float g, float b, float a);
void canvas2d_set_shadow_blur(struct canvas2d_context *__single cv, float blur);
void canvas2d_set_shadow_offset_x(struct canvas2d_context *__single cv, float offset);
void canvas2d_set_shadow_offset_y(struct canvas2d_context *__single cv, float offset);

// filter: the CSS filter functions, as a typed API (no CSS string parsing).
// Each canvas2d_add_filter_* appends one function to the context's filter list;
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
void canvas2d_set_filter_none(struct canvas2d_context *__single cv);
void canvas2d_add_filter_blur(struct canvas2d_context *__single cv, float px);
void canvas2d_add_filter_brightness(struct canvas2d_context *__single cv, float amount);
void canvas2d_add_filter_contrast(struct canvas2d_context *__single cv, float amount);
void canvas2d_add_filter_drop_shadow(struct canvas2d_context *__single cv,
                                   enum canvas2d_color_space space,
                                   float dx, float dy,
                                   float blur, float r, float g, float b,
                                   float a);
void canvas2d_add_filter_grayscale(struct canvas2d_context *__single cv, float amount);
void canvas2d_add_filter_hue_rotate(struct canvas2d_context *__single cv, float radians);
void canvas2d_add_filter_invert(struct canvas2d_context *__single cv, float amount);
void canvas2d_add_filter_opacity(struct canvas2d_context *__single cv, float amount);
void canvas2d_add_filter_saturate(struct canvas2d_context *__single cv, float amount);
void canvas2d_add_filter_sepia(struct canvas2d_context *__single cv, float amount);

// Set the fill paint to a gradient and clear its stops; fill() uses it until the
// next canvas2d_set_fill_rgba.  Coordinates are user space (the transform is baked in
// now).  Add stops with canvas2d_add_fill_color_stop; offsets clamp to [0,1].
//
// The interpolation is chosen at creation, two orthogonal required knobs: the
// colour SPACE the stops lerp in (interp_space, any of CANVAS2D_CS_SRGB /
// CANVAS2D_CS_LINEAR_SRGB / CANVAS2D_CS_OKLAB -- equal peers, see canvas2d_color_space)
// and whether the colour coordinates are premultiplied by alpha before the lerp
// (interp_alpha, CANVAS2D_ALPHA_PREMUL hygiene -- a transparent stop contributes
// no colour -- or CANVAS2D_ALPHA_UNPREMUL, lerped directly; the two are equal
// peers).  Alpha itself always lerps linearly.
void canvas2d_set_fill_linear_gradient(struct canvas2d_context *__single cv,
                                     enum canvas2d_color_space interp_space,
                                     enum canvas2d_alpha_type interp_alpha,
                                     float x0, float y0, float x1, float y1);
void canvas2d_set_fill_radial_gradient(struct canvas2d_context *__single cv,
                                     enum canvas2d_color_space interp_space,
                                     enum canvas2d_alpha_type interp_alpha,
                                     float x0, float y0,
                                     float r0, float x1, float y1, float r1);
// Conic gradient (createConicGradient): colours sweep clockwise around (x, y)
// from `start_angle` radians (measured from the +x axis).  Stops are added with
// canvas2d_add_fill_color_stop, as for the linear/radial gradients.
void canvas2d_set_fill_conic_gradient(struct canvas2d_context *__single cv,
                                    enum canvas2d_color_space interp_space,
                                    enum canvas2d_alpha_type interp_alpha,
                                    float start_angle, float x, float y);
void canvas2d_add_fill_color_stop(struct canvas2d_context *__single cv,
                                enum canvas2d_color_space space, float offset,
                                float r, float g, float b, float a);
// Image pattern fill paint (createPattern): the tightly packed RGBA8 source
// (w*h, top row first) tiles the plane per `repeat`.  The source is *borrowed* --
// the caller must keep it alive while it remains the fill paint (including across
// save/restore) -- and the pattern is pinned in device space at the current
// transform, like the gradients.  Sampling honours image smoothing (bilinear vs
// nearest).  `space` tags how to interpret the source's colours -- the pattern
// is sampled in that space and the resolved sample converts to the canvas
// working space on deposit (a no-op when they match).  Ignored if the dimensions
// are non-positive or overflow.
void canvas2d_set_fill_pattern(struct canvas2d_context *__single cv, enum canvas2d_color_space space,
                             uint8_t const *__counted_by(w * h * 4) src,
                             int w, int h, enum canvas2d_pattern_repeat repeat);

void canvas2d_clear_rect(struct canvas2d_context *__single cv, float x, float y, float w, float h);
void canvas2d_fill_rect(struct canvas2d_context *__single cv, float x, float y, float w, float h);
// Stroke the outline of the rectangle with the current stroke style and line
// styles (width/join/cap/dash), without disturbing the current path.  Corners
// are transformed by the current transform (a rotated CTM strokes a rotated
// quad).  A zero-width-or-height rectangle degenerates to a stroked line.
void canvas2d_stroke_rect(struct canvas2d_context *__single cv, float x, float y, float w, float h);

// Path coordinates are transformed by the current transform as they are added.
void canvas2d_begin_path(struct canvas2d_context *__single cv);
void canvas2d_move_to(struct canvas2d_context *__single cv, float x, float y);
void canvas2d_line_to(struct canvas2d_context *__single cv, float x, float y);
void canvas2d_rect(struct canvas2d_context *__single cv, float x, float y, float w, float h);
void canvas2d_quadratic_curve_to(struct canvas2d_context *__single cv,
                               float cpx, float cpy, float x, float y);
void canvas2d_bezier_curve_to(struct canvas2d_context *__single cv, float c1x, float c1y,
                            float c2x, float c2y, float x, float y);
void canvas2d_arc(struct canvas2d_context *__single cv, float x, float y, float radius,
                float start_angle, float end_angle, bool anticlockwise);
void canvas2d_ellipse(struct canvas2d_context *__single cv, float x, float y, float rx, float ry,
                    float rotation, float start_angle, float end_angle,
                    bool anticlockwise);
void canvas2d_round_rect(struct canvas2d_context *__single cv, float x, float y, float w, float h,
                       float radius);
// roundRect with independent, elliptical per-corner radii.  Corners are given in
// CSS order -- top-left, top-right, bottom-right, bottom-left -- each as an (x,y)
// radius pair (equal x and y is a circular corner; 0 is a sharp corner).
// Negative/non-finite radii are treated as 0, and radii too large for the rect
// are scaled down proportionally (the CSS border-radius overlap rule).
void canvas2d_round_rect_radii(struct canvas2d_context *__single cv, float x, float y,
                             float w, float h,
                             float tl_x, float tl_y, float tr_x, float tr_y,
                             float br_x, float br_y, float bl_x, float bl_y);
void canvas2d_arc_to(struct canvas2d_context *__single cv, float x1, float y1, float x2, float y2,
                   float radius);
void canvas2d_close_path(struct canvas2d_context *__single cv);

// Fill the current path under `rule` -- the rule rides each call, as on the web
// (fill() defaults to nonzero THERE; a C caller spells it) -- handling holes
// and self-intersection.
void canvas2d_fill(struct canvas2d_context *__single cv, enum canvas2d_fill_rule rule);

// Intersect the clip region with the current path under `rule`.  Subsequent
// draws are masked to the running intersection of all clips; the region is
// part of the saved state, so save()/restore() brackets a clip.
void canvas2d_clip(struct canvas2d_context *__single cv, enum canvas2d_fill_rule rule);

// Hit testing: whether the point (x, y) is inside the current path under `rule`.
// (x, y) is transformed by the current transform before the test -- matching how
// path points are baked through the CTM as they are added -- so a point given in
// the same user space as the path hits.  Non-finite coordinates return false.
bool canvas2d_is_point_in_path(struct canvas2d_context *__single cv, float x, float y,
                             enum canvas2d_fill_rule rule);
// Whether (x, y) is inside the stroked area of the current path under the current
// line styles (width/join/cap/dash).  (x, y) is transformed by the current
// transform, as for is_point_in_path; non-finite coordinates return false.
bool canvas2d_is_point_in_stroke(struct canvas2d_context *__single cv, float x, float y);

// Path2D: a constructible path object, independent of the canvas and its current
// path.  The object itself -- struct canvas2d_path2d, its constructor/free, and the
// canvas2d_path2d_* builders -- lives in canvas2d_path2d.h; forward-declared here for
// the canvas methods below that consume one.
struct canvas2d_path2d;

// Fill / stroke / clip / hit-test a Path2D (the fill rule is explicit here, not
// taken from state).  None of these disturb the canvas's current path.
void canvas2d_fill_path(struct canvas2d_context *__single cv, struct canvas2d_path2d const *__single p,
                      enum canvas2d_fill_rule rule);
void canvas2d_stroke_path(struct canvas2d_context *__single cv, struct canvas2d_path2d const *__single p);
void canvas2d_clip_path(struct canvas2d_context *__single cv, struct canvas2d_path2d const *__single p,
                      enum canvas2d_fill_rule rule);
bool canvas2d_is_point_in_path2d(struct canvas2d_context *__single cv, struct canvas2d_path2d const *__single p,
                               float x, float y, enum canvas2d_fill_rule rule);
bool canvas2d_is_point_in_stroke_path(struct canvas2d_context *__single cv,
                                    struct canvas2d_path2d const *__single p,
                                    float x, float y);

void canvas2d_set_stroke_rgba(struct canvas2d_context *__single cv, enum canvas2d_color_space space,
                            float r, float g, float b, float a);
// Gradient stroke paint, mirroring the fill gradient calls; stroke() uses it
// until the next canvas2d_set_stroke_rgba.  (Coordinates are baked through the
// transform now, as for fills.)  interp_space and interp_alpha are the required
// interpolation knobs, exactly as for the fill twins above.
void canvas2d_set_stroke_linear_gradient(struct canvas2d_context *__single cv,
                                       enum canvas2d_color_space interp_space,
                                       enum canvas2d_alpha_type interp_alpha,
                                       float x0, float y0, float x1, float y1);
void canvas2d_set_stroke_radial_gradient(struct canvas2d_context *__single cv,
                                       enum canvas2d_color_space interp_space,
                                       enum canvas2d_alpha_type interp_alpha,
                                       float x0, float y0,
                                       float r0, float x1, float y1, float r1);
void canvas2d_set_stroke_conic_gradient(struct canvas2d_context *__single cv,
                                      enum canvas2d_color_space interp_space,
                                      enum canvas2d_alpha_type interp_alpha,
                                      float start_angle, float x, float y);
void canvas2d_add_stroke_color_stop(struct canvas2d_context *__single cv,
                                  enum canvas2d_color_space space, float offset,
                                  float r, float g, float b, float a);
// Image pattern stroke paint, mirroring canvas2d_set_fill_pattern (the same
// `space` tag, sampled in-space and converted to the working space on deposit).
void canvas2d_set_stroke_pattern(struct canvas2d_context *__single cv, enum canvas2d_color_space space,
                               uint8_t const *__counted_by(w * h * 4) src,
                               int w, int h, enum canvas2d_pattern_repeat repeat);
void canvas2d_set_line_width(struct canvas2d_context *__single cv, float width);
void canvas2d_set_line_join(struct canvas2d_context *__single cv, enum canvas2d_line_join join);
void canvas2d_set_line_cap(struct canvas2d_context *__single cv, enum canvas2d_line_cap cap);
void canvas2d_set_miter_limit(struct canvas2d_context *__single cv, float limit);
// `pattern` lists alternating on/off lengths (user units); count 0 = solid.
void canvas2d_set_line_dash(struct canvas2d_context *__single cv,
                          float const *__counted_by(count) pattern, int count);
// Copy the current dash pattern into `out` (up to `cap` entries) and return its
// full length.  Pass cap 0 (out may be NULL) to query the length, then size a
// buffer; the returned float values are a copy, so mutating them is harmless.
int canvas2d_get_line_dash(struct canvas2d_context *__single cv,
                         float *__counted_by(cap) out, int cap);
void canvas2d_set_line_dash_offset(struct canvas2d_context *__single cv, float offset);
void canvas2d_stroke(struct canvas2d_context *__single cv);

// drawImage: a tightly packed RGBA8 source (sw*sh, top row first), sampled
// bilinearly, transformed by the current transform, composited source-over under
// the current global alpha and clip.  The three forms mirror the Canvas 2D
// overloads; the subrect form samples source rect [sx,sy,sw',sh'] into dest rect
// [dx,dy,dw,dh] (both user space).  Sampling is bilinear unless image smoothing
// is disabled (then nearest-neighbour).
//
// imageSmoothingEnabled: when true (default), drawImage samples bilinearly; when
// false, nearest-neighbour -- crisp, blocky upscaling with no blending.
void canvas2d_set_image_smoothing_enabled(struct canvas2d_context *__single cv, bool enabled);
// imageSmoothingQuality, applied while smoothing is enabled.  low (the spec
// default) is plain bilinear.  medium and high antialias minification: a
// drawImage whose source footprint passes one source px per device px samples
// a premultiplied mip pyramid (2x2 box-halved levels) with trilinear
// filtering -- a reified image's pyramid caches via the explicit
// canvas2d_image_build_mips (without it, the draw stays bilinear); a borrowed
// bitmap's rebuilds per such draw, the quality knob being the caller's
// opt-in to that cost.  high additionally upgrades magnification to a 4x4
// Catmull-Rom (the BC-spline family; the (B, C) pair is one line in canvas2d.c
// if Mitchell is wanted instead), its taps premultiplied so negative lobes
// cannot synthesize colour from transparent texels.
void canvas2d_set_image_smoothing_quality(struct canvas2d_context *__single cv,
                                        enum canvas2d_image_smoothing_quality quality);
// drawImage from a borrowed bitmap: straight (unpremultiplied) RGBA8, top row
// first, the caller's buffer for the duration of the call only.  Because a
// borrowed buffer has no identity to cache derived data against, a minifying
// draw at medium/high quality rebuilds its mip chain per call -- reified
// images below pay that cost once, explicitly.  `space` tags how to interpret
// the buffer's colours -- sampled in that space, the resolved sample converted
// to the canvas working space on deposit (a no-op when they match).
void canvas2d_draw_bitmap(struct canvas2d_context *__single cv, enum canvas2d_color_space space,
                       uint8_t const *__counted_by(sw * sh * 4) src,
                       int sw, int sh, float dx, float dy);
void canvas2d_draw_bitmap_scaled(struct canvas2d_context *__single cv, enum canvas2d_color_space space,
                              uint8_t const *__counted_by(sw * sh * 4) src,
                              int sw, int sh, float dx, float dy,
                              float dw, float dh);
void canvas2d_draw_bitmap_subrect(struct canvas2d_context *__single cv, enum canvas2d_color_space space,
                               uint8_t const *__counted_by(sw * sh * 4) src,
                               int sw, int sh, float sx, float sy,
                               float sww, float shh, float dx, float dy,
                               float dw, float dh);

// Reified images, in the Skia vocabulary: an image is a thing you draw FROM,
// a surface (the canvas) a thing you draw TO, a bitmap the raw RGBA8 memory
// realizing either.  The image object -- struct canvas2d_image, its typed
// constructors (canvas2d_image_unorm8 / _f16), canvas2d_image_build_mips, and
// canvas2d_image_width / _height / _free -- lives in canvas2d_image.h; forward-
// declared here for the canvas methods below that produce or consume one.
struct canvas2d_image;

// Snapshot a canvas as an image (canvas-as-source).  The
// surface is premultiplied f16 and so is the snapshot: a straight memcpy,
// bit-lossless, no quantize and no unpremultiply anywhere between surface
// and sample.  A copy: drawing on the canvas afterwards does not change the
// snapshot.  The snapshot's colour space is the canvas's working space (the
// snapped pixels ARE in the working space -- CANVAS2D_CS_SRGB
// for an sRGB canvas, CANVAS2D_CS_LINEAR_SRGB for a linear one).  NULL on OOM.
struct canvas2d_image *__single canvas2d_snapshot(struct canvas2d_context *__single cv);

// drawImage from a reified image: the bitmap trio's three overloads, same
// transform/clip/alpha/quality semantics, sourcing the image's pixels (and
// its cached mips, once built).
void canvas2d_draw_image(struct canvas2d_context *__single cv,
                       struct canvas2d_image const *__single img,
                       float dx, float dy);
void canvas2d_draw_image_scaled(struct canvas2d_context *__single cv,
                              struct canvas2d_image const *__single img,
                              float dx, float dy, float dw, float dh);
void canvas2d_draw_image_subrect(struct canvas2d_context *__single cv,
                               struct canvas2d_image const *__single img,
                               float sx, float sy, float sww, float shh,
                               float dx, float dy, float dw, float dh);

// Text.  The default typeface is Libian TC (clerical-script 隸書), which carries
// both Latin and Chinese; canvas2d_set_font_family chooses another.  Size is
// user-space px (default 10).  fill_text and stroke_text lay out `text` (UTF-8)
// with its baseline origin at (x, y), advance +x, and paint the glyph outlines
// like fill()/stroke(): transform, clip, gradient and global alpha all apply.
// measure_text returns the advance width in user px.
void canvas2d_set_font_size(struct canvas2d_context *__single cv, float px);
// fontFamily: the typeface used by fill_text/stroke_text, measure_text, and the
// text queries (default "Libian TC").  `name` is copied in (truncated if it
// exceeds the internal capacity); a NULL or empty name is ignored, keeping the
// current family.  An unavailable family falls back through Core Text's font
// cascade and records as the resolved font.  Part of the drawing state:
// save/restore brackets it, reset/resize restore the default.
void canvas2d_set_font_family(struct canvas2d_context *__single cv, char const *__null_terminated name);
// The length-counted form (the name need not be NUL-terminated), like
// canvas2d_fill_text_n beside canvas2d_fill_text; len <= 0 is ignored.
void canvas2d_set_font_family_n(struct canvas2d_context *__single cv,
                              char const *__counted_by(len) name, int len);
// fontWeight: the typeface weight on the CSS 100..900 axis (400 == normal/
// regular, 700 == bold), default 400.  The value is clamped to [100, 900].
// fontStyle: upright (CANVAS2D_FONT_STYLE_NORMAL, the default) or slanted
// (CANVAS2D_FONT_STYLE_ITALIC); an unrecognized enum value is ignored.  Both pick
// the nearest real face of the current family and, when the family has no such
// face, let the platform SYNTHESIZE the bold/italic (a thicker / slanted
// rendering of the regular face) rather than falling back to regular.  Part of
// the drawing state: save/restore brackets each, reset/resize restore the
// 400 / NORMAL defaults.
void canvas2d_set_font_weight(struct canvas2d_context *__single cv, int weight);
void canvas2d_set_font_style(struct canvas2d_context *__single cv, enum canvas2d_font_style style);
// fontKerning / textRendering / lang: shaping-attribute toggles, drawing state
// like the font setters above (save/restore brackets each, reset/resize restore
// the defaults).  They feed Core Text shaping, so they affect the runs' advances
// (kerning), ligature formation (textRendering optimizeSpeed), and
// locale-dependent glyph selection (lang) -- and so measure_text/fill_text/
// stroke_text and the text queries.  The defaults (AUTO / AUTO / "") reproduce
// exactly what the shaper draws today.
//   fontKerning: none disables kerning; auto/normal leave the default kerning
//     (canvas2d_font_kerning; an unrecognized enum is ignored).
//   textRendering: optimizeSpeed disables kerning AND ligatures; the others
//     leave the defaults (canvas2d_text_rendering; an unrecognized enum is
//     ignored).
//   lang: a BCP-47 language tag (e.g. "en", "zh-Hant") set on the shaped run for
//     locale-dependent glyph selection; "" (the default) sets none.  The tag is
//     copied in (truncated if it exceeds the internal capacity); a NULL tag is
//     ignored, keeping the current value (pass "" to clear it).
void canvas2d_set_font_kerning(struct canvas2d_context *__single cv, enum canvas2d_font_kerning kerning);
void canvas2d_set_text_rendering(struct canvas2d_context *__single cv, enum canvas2d_text_rendering rendering);
void canvas2d_set_lang(struct canvas2d_context *__single cv, char const *__null_terminated tag);
// The length-counted form of canvas2d_set_lang (the tag need not be
// NUL-terminated), like canvas2d_set_font_family_n; len < 0 is ignored, len == 0
// clears the language.
void canvas2d_set_lang_n(struct canvas2d_context *__single cv,
                       char const *__counted_by(len) tag, int len);
// fontVariantCaps / fontStretch: more shaping/face state, drawing state like the
// font setters above (save/restore brackets each, reset/resize restore the
// defaults).  fontVariantCaps feeds Core Text's small-cap features and so
// changes shaping (glyph ids and advances); fontStretch feeds the width trait
// and so resolves a different real face.  The defaults (NORMAL / NORMAL)
// reproduce exactly what the shaper draws today.
//   fontVariantCaps: small_caps applies smcp, all_small_caps smcp + c2sc;
//     normal applies neither (canvas2d_font_variant_caps; an unrecognized enum is
//     ignored).  A no-op on a font without the feature.
//   fontStretch: the nine width keywords map onto the width trait so the
//     descriptor resolves a real condensed/expanded face; no width is
//     synthesized, so a family with no width face is a no-op
//     (canvas2d_font_stretch; an unrecognized enum is ignored).
void canvas2d_set_font_variant_caps(struct canvas2d_context *__single cv,
                                  enum canvas2d_font_variant_caps variant_caps);
void canvas2d_set_font_stretch(struct canvas2d_context *__single cv, enum canvas2d_font_stretch stretch);
// letterSpacing: extra advance added after each typographic cluster, in user px
// (default 0; may be negative).  wordSpacing: extra advance added at each
// word-separator (U+0020 SPACE), in user px (default 0; may be negative).  Both
// affect the drawing pen position and measure_text/measure_text_full width.  A
// NaN or infinite value is treated as 0.  Part of the drawing state:
// save/restore brackets each, reset/resize restore the 0 default.
void canvas2d_set_letter_spacing(struct canvas2d_context *__single cv, float px);
void canvas2d_set_word_spacing(struct canvas2d_context *__single cv, float px);
// textAlign: horizontal placement of the text relative to the (x, y) passed to
// fill_text/stroke_text, by the advance width.  left puts x at the left edge,
// right at the right edge, center centres it; start (default) and end resolve
// through the direction attribute (start == left under ltr, == right under rtl;
// end the opposite).
void canvas2d_set_text_align(struct canvas2d_context *__single cv, enum canvas2d_text_align align);
// direction: the paragraph direction (default ltr).  It resolves textAlign
// start/end, and it is the base direction text is shaped under -- a
// mixed-direction string lays its runs out in the visual order that base
// implies, and neutrals (spaces, punctuation) resolve against it -- so the same
// string can draw differently under ltr and rtl.  Part of the drawing state:
// save/restore brackets it, reset/resize restore the default.
void canvas2d_set_direction(struct canvas2d_context *__single cv, enum canvas2d_direction dir);
// textBaseline: vertical placement of the baseline relative to (x, y).
// alphabetic (default) draws the baseline at y; top/hanging/middle/ideographic/
// bottom shift it by the font's ascent/descent.
void canvas2d_set_text_baseline(struct canvas2d_context *__single cv, enum canvas2d_text_baseline baseline);
float canvas2d_measure_text(struct canvas2d_context *__single cv, char const *__null_terminated text);

// Full measureText() TextMetrics, all in user px, relative to the text's origin
// and the alphabetic baseline (the defaults: textAlign start, textBaseline
// alphabetic).  Sign conventions match the web TextMetrics: *_left and *_ascent
// are positive measured leftward / upward from the origin, *_right and *_descent
// positive rightward / downward.  `width` equals canvas2d_measure_text.
typedef struct {
    float width;
    float actual_bounding_box_left, actual_bounding_box_right;
    float actual_bounding_box_ascent, actual_bounding_box_descent;
    float font_bounding_box_ascent, font_bounding_box_descent;
    float em_height_ascent, em_height_descent;
    float alphabetic_baseline, hanging_baseline, ideographic_baseline;
} canvas2d_text_metrics;

canvas2d_text_metrics canvas2d_measure_text_full(struct canvas2d_context *__single cv,
                                             char const *__null_terminated text);

// Shaped-line selection/caret queries: a non-spec extension (the canvas 2D spec
// has no selection API) for building text editing and selection on top of the
// canvas.  They shape `text` exactly as canvas2d_measure_text does -- honouring the
// current font size, direction, and letterSpacing/wordSpacing -- and report
// visual x positions in user px from the text's START, the same origin
// canvas2d_measure_text measures from: BEFORE textAlign, textBaseline, and the
// transform are applied (the caller maps between canvas coordinates and this
// text-line space).  Indices are logical UTF-16 offsets into the source.  Like
// measure_text / is_point_in_* / get_image_data / read_rgba, these are pure
// read-only queries: they move no pixels and record nothing.
//
// A visual x range in user px from the text's start.
typedef struct { float x0, x1; } canvas2d_text_span;
// Map a visual x (user px from the text's start) to the logical UTF-16 index it
// falls on, or -1.
int canvas2d_text_index_at_x(struct canvas2d_context *__single cv, char const *__null_terminated text,
                           float x);
// The caret x (user px from the text's start) for a logical UTF-16 index.  An
// index inside a cluster snaps to that cluster's leading edge; an index at or
// past the source's end is the line's advance width (== canvas2d_measure_text).
float canvas2d_text_x_at_index(struct canvas2d_context *__single cv, char const *__null_terminated text,
                             int index);
// Visual x-spans covering the logical range [lo, hi); a bidi range maps to
// non-contiguous visual positions and splits into several spans.  Writes up to
// `max` spans into `out` and returns the count (0 when max <= 0 or out is NULL).
int canvas2d_text_selection(struct canvas2d_context *__single cv, char const *__null_terminated text,
                          int lo, int hi, canvas2d_text_span *__counted_by(max) out, int max);

void canvas2d_fill_text(struct canvas2d_context *__single cv, char const *__null_terminated text,
                      float x, float y);
void canvas2d_stroke_text(struct canvas2d_context *__single cv, char const *__null_terminated text,
                        float x, float y);
// Length-counted variants: `text` is `len` bytes of UTF-8 and need not be
// NUL-terminated, so a caller can pass a slice of a larger buffer directly.  The
// NUL-terminated fill_text/stroke_text above are conveniences over these.
void canvas2d_fill_text_n(struct canvas2d_context *__single cv, char const *__counted_by(len) text,
                        int len, float x, float y);
void canvas2d_stroke_text_n(struct canvas2d_context *__single cv, char const *__counted_by(len) text,
                          int len, float x, float y);
// fillText/strokeText with a maxWidth: when the text's advance exceeds a finite,
// positive `max_width`, it is condensed horizontally (scaled in x about the
// alignment anchor) to fit.  A non-positive or non-finite `max_width` imposes no
// limit, rendering exactly like canvas2d_fill_text/canvas2d_stroke_text.
void canvas2d_fill_text_max(struct canvas2d_context *__single cv, char const *__null_terminated text,
                          float x, float y, float max_width);
void canvas2d_stroke_text_max(struct canvas2d_context *__single cv, char const *__null_terminated text,
                            float x, float y, float max_width);
// Length-counted variants of the maxWidth path (the slice forms replay hands
// its text tail to, and the NUL-terminated conveniences delegate to).
void canvas2d_fill_text_max_n(struct canvas2d_context *__single cv, char const *__counted_by(len) text,
                            int len, float x, float y, float max_width);
void canvas2d_stroke_text_max_n(struct canvas2d_context *__single cv,
                              char const *__counted_by(len) text,
                              int len, float x, float y, float max_width);

// Tightly packed RGBA8, top row first; len must be width*height*4.  `space` names
// the OUTPUT colour space (see canvas2d_color_space): CANVAS2D_CS_SRGB emits encoded
// sRGB bytes, CANVAS2D_CS_LINEAR_SRGB linear-sRGB bytes, and CANVAS2D_CS_OKLAB Oklab
// (L,a,b) bytes.
void canvas2d_read_rgba(struct canvas2d_context *__single cv, enum canvas2d_color_space space,
                      uint8_t *__counted_by(len) out, int len);
// canvas2d_write_png emits a BT.2100 PNG: 16-bit, Rec.2020 primaries, PQ (ST 2084)
// transfer, cICP-signalled, regardless of the working space (the surface is
// transformed into that encoding on the way out -- wide-gamut and HDR values
// from a linear canvas carry through, an sRGB canvas's content lands in the same
// container).  There is no decoder; PNG is output only.
bool canvas2d_write_png(struct canvas2d_context *__single cv, char const *__null_terminated path);

// canvas2d_write_png's in-memory sibling: the same BT.2100 PNG bytes in a malloc'd
// buffer (caller frees), *outlen its length.  NULL on OOM.
uint8_t *__counted_by_or_null(*outlen)
canvas2d_encode_png(struct canvas2d_context *__single cv, int *__single outlen);

// Pixel I/O for a w*h sub-image (tightly packed RGBA8, len must be w*h*4).
// get: pixels outside the canvas read back transparent black.
// put: overwrites (no blending), clipped to the canvas.
// `space` (see canvas2d_color_space) is the space the bytes are in: for get, the
// OUTPUT space (routed through read_rgba); for put, the space the INCOMING bytes
// are interpreted in.  The three colour spaces are peers.
void canvas2d_get_image_data(struct canvas2d_context *__single cv,
                           enum canvas2d_color_space space,
                           int x, int y, int w, int h,
                           uint8_t *__counted_by(len) out, int len);
void canvas2d_put_image_data(struct canvas2d_context *__single cv,
                           enum canvas2d_color_space space,
                           uint8_t const *__counted_by(len) data, int len,
                           int w, int h, int dx, int dy);
// putImageData with a dirty rectangle: only the source sub-rect
// [dirtyX, dirtyX+dirtyWidth) x [dirtyY, dirtyY+dirtyHeight) (ImageData
// coordinates) is written, still placed with the image origin at (dx, dy).  The
// dirty rect is normalised like the spec -- negative extents flip, then it is
// clamped to the source; an empty result is a no-op.  `space` is the incoming
// bytes' space, as for canvas2d_put_image_data.
void canvas2d_put_image_data_dirty(struct canvas2d_context *__single cv,
                                 enum canvas2d_color_space space,
                                 uint8_t const *__counted_by(len) data, int len,
                                 int w, int h, int dx, int dy,
                                 int dirty_x, int dirty_y,
                                 int dirty_w, int dirty_h);

// The rgba-float16 ImageData twins (the spec's `rgba-float16` storage format):
// straight (unpremultiplied) _Float16 RGBA instead of u8, `len` the ELEMENT
// count (w*h*4) rather than a byte count.  Unlike the u8 path these preserve
// EXTENDED range -- HDR (>1) and wide-gamut (negative) colour a CANVAS2D_CS_LINEAR_SRGB
// canvas stores are neither clamped to [0,1] nor quantized to 8-bit.  `space`
// (see canvas2d_color_space) is the f16 values' colour space, with the same
// meaning as the u8 versions: for get the OUTPUT space, for put the space the
// incoming values are interpreted in.  On a linear canvas extended values
// survive end to end; on an sRGB canvas they bound through the sRGB encode/blend.
void canvas2d_get_image_data_f16(struct canvas2d_context *__single cv,
                               enum canvas2d_color_space space,
                               int x, int y, int w, int h,
                               _Float16 *__counted_by(len) out, int len);
void canvas2d_put_image_data_f16(struct canvas2d_context *__single cv,
                               enum canvas2d_color_space space,
                               _Float16 const *__counted_by(len) data, int len,
                               int w, int h, int dx, int dy);
void canvas2d_put_image_data_dirty_f16(struct canvas2d_context *__single cv,
                                     enum canvas2d_color_space space,
                                     _Float16 const *__counted_by(len) data, int len,
                                     int w, int h, int dx, int dy,
                                     int dirty_x, int dirty_y,
                                     int dirty_w, int dirty_h);

// Read a text canvas-program from `path` and replay it onto cv via the calls
// above.  One command per line (UTF-8): each command is exactly the canvas2d_*
// function name *without* the `canvas2d_` prefix, followed by its whitespace-
// separated arguments, e.g.:
//     set_fill_rgba 0.8 0.2 0.2 1 srgb
//     move_to 10 20
//     set_global_composite_operation multiply
//     fill_text 12 30 Hello
// Enums (set_global_composite_operation / set_line_join / set_line_cap /
// set_text_align / set_text_baseline / set_direction / set_font_style /
// set_font_kerning / set_text_rendering / set_font_variant_caps /
// set_font_stretch /
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
// add_fill_color_stop, add_stroke_color_stop, add_filter_drop_shadow -- carry a
// REQUIRED trailing colour-space token (srgb|linear|oklab) after their floats,
// the space the (r,g,b[,a]) are given in (see canvas2d_color_space).  The three
// spaces are peers, so the token is always written and always read; a missing
// or unknown token is a malformed file:
//     set_fill_rgba 0.8 0.2 0.2 1 srgb
//     set_fill_rgba 0.8 0.2 0.2 1 oklab
// put_image_data / put_image_data_dirty carry their input's space the same way,
// but on the image BLOCK's required <space> tag (see below), not the op line,
// since their pixels ride a block.
//
// Text-block lines make a recorded program self-contained (no fonts needed to
// replay): the recorder writes, ahead of each text op that first needs them,
//     font <id> <ascent> <descent> <weight> <style 0|1> <name...>
//     glyph <font-id> <gid> <units-per-em> <x0 y0 x1 y1> <m/l/q/c/z curves...>
//     bitmap <font-id> <gid> <w> <h> <x0 y0 x1 y1> <zlen> <nlines>
//     bits <base64...>                            (exactly nlines of these)
//     shaping <size_px> <rtl 0|1> <ls> <ws> <weight> <style 0|1> <kerning 0-2> <rendering 0-3> <variant-caps 0-2> <stretch 0-8> <lang-len> <lang...> <fam-len> <fam...> <utf16-len> <nruns> <byte-len> <text...>
//     run <font-id|-1> <rtl 0|1> <color 0|1> <nglyphs> (gid adv cluster)*
// font declares a file-local font id: its name (rest of the line, spaces
// allowed), vertical metrics at size 1.0, and the weight/style (CSS 100..900;
// 0 == upright, 1 == italic) that distinguish a SYNTHESIZED bold/italic from
// the regular face -- the family name alone aliases them (synthesis reports the
// regular face's name), so the id (and thus the glyph key) keys on all three.
// glyph carries one glyph's
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
// shaped line (the lang tag is lang-len raw bytes after one space, then the
// family is fam-len raw bytes after one space, then the text is
// exactly byte-len raw bytes after one space; the
// rtl token is the paragraph direction the line was shaped under, ls/ws the
// letterSpacing/wordSpacing, weight/style the font weight/style, and
// kerning/rendering/variant-caps/stretch/lang the shaping toggles -- all parts
// of the cache key, since the same bytes shape and space differently under each.
// ls/ws, weight/style, and kerning/rendering/variant-caps/stretch/lang are
// always written, even
// when default; the spacing is already baked into the run advances, so they only
// key the rebuilt line.  The canvas's family/weight/style/spacing/toggle STATE --
// the values a fill_text keys its lookup by -- ride set_font_family/
// set_font_weight/set_font_style/set_letter_spacing/set_word_spacing/
// set_font_kerning/set_text_rendering/set_font_variant_caps/set_font_stretch/
// set_lang ordinary state ops,
// recorded like set_font_size/set_direction).  Replaying the blocks pre-populates
// the canvas's text cache, so the text ops
// that follow never call the platform text system -- a recorded program,
// emoji included, replays byte-identically on machines without the recording
// machine's fonts.
//
// Image-block lines carry the RGBA8 sources the image ops need, the capture
// machinery reused wholesale:
//     image <id> <unorm8|f16> <unpremul|premul> <w> <h> <zlen> <nlines> <space>
//     bits <base64...>                            (exactly nlines of these)
// declares file-local numbered image <id>, its colour and alpha types named
// on the line like every other enum in the format -- the four combinations
// are peers, none the unmarked default (a recorded canvas2d_snapshot is
// `f16 premul`, bit-lossless in the file too) -- w x h x 4 channels,
// deflated and base64-chunked exactly like a capture, content-deduplicated
// by the recorder so one buffer used many ways costs one block.  The trailing
// <space> (srgb|linear|oklab) is the source's colour-space tag -- REQUIRED, like
// the colour-op token, since the spaces are peers; a missing token is malformed.
// (The tag is honoured on replay: the source is sampled in that space and the
// resolved sample converts to the working space on deposit.)  An optional
//     image_mips <id>
// line marks the block's draws as carrying mip-chain semantics: the bitmap
// entry points (per-draw chain rebuild) emit it as soon as their block is
// declared, and an image-object draw emits it only once
// canvas2d_image_build_mips has run -- so a mip-less image's bilinear-fallback
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
// Parsing is strict: an unknown command, a bad argument, a missing or unknown
// trailing colour-space token on a colour op or image block, an over-long line,
// or a malformed block (an undeclared font id, a bad verb token, a cluster index
// past the text length, a non-finite number where the recorder writes finite
// ones, a bitmap whose bits lines are miscounted, mis-padded, or decode to
// anything but exactly zlen bytes, or whose declared stream is no zlib at all
// or inflates to anything but exactly w*h*4 bytes) stops replay and returns
// false (commands before the faulty line have already been applied; the canvas
// remains valid and drawable).  Numbers are
// written with %.9g and reparse to the identical float32.  The pure queries
// (measure_text, is_point_in_*, text_index_at_x/x_at_index/selection,
// get_image_data, read_rgba, write_png) move no pixels and are not part of the
// text format.
bool canvas2d_replay_from(struct canvas2d_context *__single cv, char const *__null_terminated path);

// Begin recording subsequent drawing calls to `path` as a text canvas-program in
// exactly the format canvas2d_replay_from reads -- the write-side inverse of replay.
// Each recordable call is appended as one line (the canvas2d_* name minus the
// canvas2d_ prefix, then its arguments); replaying the file onto a fresh canvas of
// the same size reproduces the same image.  Recording continues until the canvas
// is destroyed (or record_to is called again, which starts a new file); pass the
// same `path` again only after the first is closed.  Returns false (recording
// nothing) if the file cannot be opened.
//
// Text ops round-trip self-contained: before each fill_text/stroke_text the
// recorder emits the font/glyph/bitmap/shaping blocks (see canvas2d_replay_from)
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
// nothing either).  See canvas2d_replay_from for the caps past which an image
// or path degrades un-recorded.
//
// EVERY pixel-affecting public op records -- the same set replay_from
// understands; there is no excluded op.  The compound path helpers
// (arc/round_rect/round_rect_radii/arc_to) are written as themselves, not
// their expansion; a setter that ignores its call per spec (a non-finite
// shadow offset or filter amount, an invalid resize) records nothing for it;
// and the pure queries (measure_text, is_point_in_*,
// text_index_at_x/x_at_index/selection, get_image_data, read_rgba, write_png,
// get_transform, get_line_dash) move no pixels and are not part of the format.
bool canvas2d_record_to(struct canvas2d_context *__single cv, char const *__null_terminated path);

// createImageData: allocate a blank (transparent black) RGBA8 image of sw*sh
// pixels -- the layout get/put_image_data use.  Returns a freshly malloc'd,
// zeroed buffer of sw*sh*4 bytes (free it with free()) and stores that byte
// length in *len, ready to hand to put_image_data.  On non-positive dimensions,
// a size that would overflow, or out of memory, returns NULL and stores 0.
// The image is independent of any canvas, so none is taken.
uint8_t *__counted_by_or_null(*len)
canvas2d_create_image_data(int sw, int sh, int *__single len);

// createImageData's rgba-float16 twin: a blank (transparent black) _Float16
// RGBA image of sw*sh pixels -- the layout get/put_image_data_f16 use.  Returns
// a freshly calloc'd buffer of sw*sh*4 _Float16 elements (free it with free())
// and stores that ELEMENT count in *len, ready to hand to put_image_data_f16.
// On non-positive dimensions, a size that would overflow, or out of memory,
// returns NULL and stores 0.
_Float16 *__counted_by_or_null(*len)
canvas2d_create_image_data_f16(int sw, int sh, int *__single len);
