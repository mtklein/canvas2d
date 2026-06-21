#pragma once

// The canvas's private object model: struct canvas2d_context and struct canvas2d_state and
// their paint helpers.  canvas2d.c owns the drawing logic; this header exposes the
// layout so the output stages carved into their own translation units
// (canvas2d_encode.c, canvas2d_imagedata.c) can read the surface and scratch buffers
// directly, without duplicating the struct.  Not a public boundary -- it carries
// the working-space, target, and tile internals canvas2d.h deliberately hides.

#include "canvas2d.h"

#include "canvas2d_color.h"     // canvas2d_unpremul, canvas2d_premul
#include "canvas2d_cover.h"     // struct canvas2d_cover
#include "canvas2d_filter.h"    // canvas2d_filter
#include "canvas2d_geom.h"      // canvas2d_matrix, canvas2d_vec2, struct canvas2d_verts
#include "canvas2d_gradient.h"  // struct canvas2d_gradient
#include "canvas2d_path.h"      // struct canvas2d_path
#include "canvas2d_stroke.h"    // enum canvas2d_line_join, enum canvas2d_line_cap
#include "canvas2d_text.h"      // struct canvas2d_font, struct canvas2d_text_cache

#include <ptrcheck.h>
#include <stdbool.h>
#include <stdint.h>

#define CANVAS2D_DASH_MAX 16

// Fixed capacity for the font-family name held in the drawing state (bytes; a
// longer name passed to canvas2d_set_font_family is truncated to fit).  The state
// carries the name by value so save()/restore() snapshot it like any other
// field; a fixed buffer keeps that copy allocation-free.
#define CANVAS2D_FONT_FAMILY_MAX 128

// Fixed capacity for the BCP-47 lang tag held in the drawing state (bytes; a
// longer tag passed to canvas2d_set_lang is truncated to fit).  Held by value like
// the family name so save()/restore() snapshot it; tags are short ("en",
// "zh-Hant", "az-Cyrl-AZ"), so a small buffer is ample.
#define CANVAS2D_LANG_MAX 32

// Cap canvas dimensions so width*height and width*height*4 stay well within a
// positive int -- the whole pipeline's RGBA8 size math is `int`.  Mirrors the
// canvas2d_png.c clamp.
#define CANVAS2D_DIM_MAX 16384

struct canvas2d_recorder;

// One RGBA8 buffer adopted from a replayed `image` block
// (canvas2d_canvas_own_image): a singly linked list the canvas frees only at
// canvas2d_free.  Patterns borrow their source, so a replayed program's
// images must outlive replay -- and survive reset(), which restores drawing
// state but does not invalidate the program's blocks mid-replay.
struct canvas2d_owned_image {
    struct canvas2d_owned_image *__single next;
    uint8_t *__counted_by(len) data;
    int len;
};

// Which paint a fill/stroke uses.  SOLID reads the `fill`/`stroke` colour,
// GRADIENT the `*_grad`, PATTERN the `*_pattern`.
enum canvas2d_paint_kind {
    CANVAS2D_PAINT_SOLID, CANVAS2D_PAINT_GRADIENT, CANVAS2D_PAINT_PATTERN
};

// An image pattern paint.  The source is borrowed (the caller owns it); `len`
// (== w*h*4) bounds it for -fbounds-safety.  `to_pattern` maps a device point to
// pattern-image space (the inverse of the CTM captured when the pattern was set),
// so the pattern is pinned in device space like the gradients.
struct canvas2d_pattern {
    uint8_t const *__counted_by(len) data;
    int len;
    int w, h;
    enum canvas2d_pattern_repeat repeat;
    enum canvas2d_color_space space;  // the source pixels' space; sampled in it, converted on deposit
    canvas2d_matrix to_pattern;
};

struct canvas2d_state {
    canvas2d_matrix ctm;
    canvas2d_unpremul fill;
    enum canvas2d_paint_kind fill_kind;
    struct canvas2d_gradient fill_grad;
    struct canvas2d_pattern fill_pattern;
    canvas2d_unpremul stroke;
    enum canvas2d_paint_kind stroke_kind;
    struct canvas2d_gradient stroke_grad;
    struct canvas2d_pattern stroke_pattern;
    float global_alpha;
    enum canvas2d_composite_op composite;  // globalCompositeOperation
    float line_width;
    enum canvas2d_line_join line_join;
    enum canvas2d_line_cap line_cap;
    float miter_limit;
    float dash[CANVAS2D_DASH_MAX];
    int dash_count;
    float dash_offset;
    float font_size;  // text size in user px (Canvas default 10px)
    // The typeface for fill_text/stroke_text/measureText, held by value (the
    // sized model: length + bytes, no NUL).  Default "Libian TC"; an unavailable
    // family falls back through Core Text and records as the resolved font.
    char font_family[CANVAS2D_FONT_FAMILY_MAX];
    int font_family_len;
    int font_weight;  // CSS 100..900 (default 400; clamped on set); with style,
                      // part of the glyph-cache identity so a synthesized bold
                      // never aliases the regular face
    enum canvas2d_font_style font_style;  // upright/italic (default NORMAL)
    // Shaping-attribute toggles (default AUTO / AUTO / ""): inputs to Core Text
    // shaping that affect the runs' advances/glyphs (not the glyph outlines), so
    // they ride the shaped-line cache key, not the glyph/font identity.
    enum canvas2d_font_kerning font_kerning;      // none disables kerning
    enum canvas2d_text_rendering text_rendering;  // optimizeSpeed disables kerning+ligatures
    enum canvas2d_font_variant_caps font_variant_caps;  // small_caps/all_small_caps -> smcp[/c2sc]
    enum canvas2d_font_stretch font_stretch;            // width axis (default NORMAL, the centre)
    char lang[CANVAS2D_LANG_MAX];  // BCP-47 tag held by value (sized model: len+bytes,
    int lang_len;              // no NUL); empty (len 0) = no language
    float letter_spacing;  // extra advance after each cluster, user px (default 0)
    float word_spacing;    // extra advance at each U+0020 SPACE, user px (default 0)
    enum canvas2d_text_align text_align;
    enum canvas2d_text_baseline text_baseline;
    enum canvas2d_direction direction;  // paragraph direction: resolves start/end and
                                 // is the base direction text shapes under
    bool image_smoothing_enabled;
    enum canvas2d_image_smoothing_quality image_smoothing_quality;
    canvas2d_unpremul shadow_color;  // shadow off when its alpha is 0
    float shadow_blur;           // device px (a Gaussian radius; CTM does not apply)
    float shadow_offset_x, shadow_offset_y;  // device px (CTM does not apply)
    // CSS filter list (canvas2d_add_filter_*): compiled colour-filter functions,
    // applied in call order to every painted op's tile before it composites.
    // A dynamic per-state array like the clip mask below: held by value, so
    // save() deep-copies it and restore()/reset() free it; NULL/0 = no filter.
    canvas2d_filter *__counted_by(filter_count) filters;
    int filter_count;
    // Clip coverage, one byte per canvas pixel (NULL = open).  Held by value in
    // the state, so save() snapshots it and restore() brings it back; clip()
    // intersects the current path's coverage into it.
    uint8_t *__counted_by(clip_len) clip_mask;
    int clip_len;
};

struct canvas2d_context {
    int width;
    int height;
    // The working colour space (canvas2d.h): CANVAS2D_CS_SRGB composites directly on
    // the encoded bytes (no transfer ever runs); CANVAS2D_CS_LINEAR_SRGB
    // composites in extended linear sRGB.  Chosen at creation, immutable -- it is
    // NOT in struct canvas2d_state, so save/restore never touch it, and
    // reset/resize leave it as the constructor set it.
    enum canvas2d_color_space space;
    canvas2d_premul *__counted_by(target_len) target;  // premultiplied; all-zero == transparent
    int target_len;                                // == width * height
    struct canvas2d_state cur;
    struct canvas2d_state *__counted_by(stack_cap) stack;
    int nsaved;
    int stack_cap;
    struct canvas2d_path path;
    // The same path in USER space (points untransformed, curves flattened in user
    // units).  Built alongside `path` so a perspective fill/stroke/clip can w-clip
    // in homogeneous space and project per docs/decisions/perspective.md; an
    // affine CTM ignores it and rasterizes the device `path` bit-identically.
    struct canvas2d_path upath;
    struct canvas2d_path pclip;      // scratch: w-clipped + projected device path (perspective)
    struct canvas2d_path text_path;  // scratch glyph outlines (fill_text/stroke_text)
    struct canvas2d_font *__single font;  // cached for cur.font_size/family/weight/style;
                                      // rebuilt when any of them changes
    float font_built_size;
    char font_built_family[CANVAS2D_FONT_FAMILY_MAX];  // the family cv->font was built for
    int font_built_family_len;
    int font_built_weight;                  // the weight cv->font was built for
    enum canvas2d_font_style font_built_style;  // the style cv->font was built for
    enum canvas2d_font_stretch font_built_stretch;  // the stretch cv->font was built for
    struct canvas2d_text_cache text_cache;  // params->derived-data memo of Core Text results:
                                 // shaped lines + canonical glyph curves, checked
                                 // before the boundary is called (canvas2d_text.h)
    canvas2d_vec2 cur_user;  // current point in user space (path.cur is device space)
    struct canvas2d_verts scratch_verts;  // stroke triangle output, fed to the coverage rasterizer
    struct canvas2d_cover cover;
    uint8_t *__counted_by(cov_cap) cov;     // per-pixel coverage for the current op's bbox
    int cov_cap;
    canvas2d_premul *__counted_by(tile_cap) tile;  // premultiplied tile for the current op's bbox
    int tile_cap;
    float *__counted_by(trow_cap) trow;    // one row of gradient parameters (vectorized solve)
    int trow_cap;
    canvas2d_unpremul *__counted_by(crow_cap) crow;  // one row of gradient colours (vectorized stop lerp)
    int crow_cap;
    struct canvas2d_recorder *__single rec;  // NULL unless canvas2d_record_to is active
    struct canvas2d_owned_image *__single owned_images;  // replayed `image` blocks
    // Shadow scratch: a single-channel mask blurred in place (src/dst ping-pong
    // for the separable box passes), sized to the shadow's device region.  Each
    // gets its own cap so the (pointer, count) pairs update independently under
    // -fbounds-safety (two pointers can't share one count).
    uint8_t *__counted_by(shadow_src_cap) shadow_src;
    int shadow_src_cap;
    uint8_t *__counted_by(shadow_dst_cap) shadow_dst;
    int shadow_dst_cap;
    // filter blur() scratch: the second buffer of the separable passes' src/dst
    // ping-pong over the op's tile (h pass tile -> scratch, v pass scratch ->
    // tile), sized like the tile.  A peer of the shadow masks above, but
    // RGBA16F -- blur() filters the painted pixels, not a coverage silhouette.
    // drop-shadow() grows it to two tiles: the shadow builds in the first half
    // and ping-pongs against the second, leaving the op's own tile untouched.
    canvas2d_premul *__counted_by(blur_tmp_cap) blur_tmp;
    int blur_tmp_cap;
    // drawImage minification scratch: the source's premultiplied mip chain
    // (level 0 = the source premultiplied, then ceil-halves), rebuilt per
    // minifying draw at medium/high smoothing quality.  A borrowed source
    // buffer has no identity to cache a pyramid against; a first-class image
    // type will own this cost via an explicit build call.
    uint8_t *__counted_by(mips_cap) mips;
    int mips_cap;
};

// A caller-supplied image rectangle (get/put_image_data region, drawImage
// source) is honoured only if its RGBA8 byte size fits a positive int: that is
// what makes the `w * h * 4` size arithmetic at the call sites overflow-free.
// (Canvas dims are already bounded by CANVAS2D_DIM_MAX; these come straight from
// the caller and are otherwise unbounded.)
bool canvas2d_rgba8_dims_ok(int w, int h);

// Grow the canvas's coverage and tile scratch to hold npix pixels (one coverage
// byte and one premultiplied pixel per cell); false on OOM.
bool canvas2d_ensure_tile(struct canvas2d_context *__single cv, int npix);
