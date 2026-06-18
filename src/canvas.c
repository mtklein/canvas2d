#include "canvas.h"

#include "blur.h"
#include "cnvs_blend.h"
#include "cnvs_color.h"
#include "cnvs_cover.h"
#include "cnvs_filter.h"
#include "cnvs_geom.h"
#include "cnvs_gradient.h"
#include "cnvs_image.h"
#include "cnvs_math.h"
#include "cnvs_mem.h"
#include "cnvs_path.h"
#include "cnvs_path2d.h"
#include "cnvs_planar.h"
#include "cnvs_png.h"
#include "cnvs_record.h"
#include "cnvs_replay.h"
#include "cnvs_stroke.h"
#include "cnvs_text.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Maximum chord deviation (device pixels) when flattening curves.
#define CANVAS_FLATTEN_TOL 0.25f
#define CANVAS_DASH_MAX 16

// Cap canvas dimensions so width*height and width*height*4 stay well within a
// positive int -- the whole pipeline's RGBA8 size math is `int`.  Mirrors the
// cnvs_png.c clamp.
#define CANVAS_DIM_MAX 16384

// A caller-supplied image rectangle (get/put_image_data region, drawImage
// source) is honoured only if its RGBA8 byte size fits a positive int: that is
// what makes the `w * h * 4` size arithmetic at the call sites overflow-free.
// (Canvas dims are already bounded by CANVAS_DIM_MAX; these come straight from
// the caller and are otherwise unbounded.)
static bool rgba8_dims_ok(int w, int h) {
    return w > 0 && h > 0 && (int64_t)w * (int64_t)h <= (int64_t)INT_MAX / 4;
}

// One RGBA8 buffer adopted from a replayed `image` block
// (cnvs_canvas_own_image): a singly linked list the canvas frees only at
// canvas_free.  Patterns borrow their source, so a replayed program's
// images must outlive replay -- and survive reset(), which restores drawing
// state but does not invalidate the program's blocks mid-replay.
struct cnvs_owned_image {
    struct cnvs_owned_image *__single next;
    uint8_t *__counted_by(len) data;
    int len;
};

// Which paint a fill/stroke uses.  SOLID reads the `fill`/`stroke` colour,
// GRADIENT the `*_grad`, PATTERN the `*_pattern`.
enum cnvs_paint_kind {
    CNVS_PAINT_SOLID, CNVS_PAINT_GRADIENT, CNVS_PAINT_PATTERN
};

// An image pattern paint.  The source is borrowed (the caller owns it); `len`
// (== w*h*4) bounds it for -fbounds-safety.  `to_pattern` maps a device point to
// pattern-image space (the inverse of the CTM captured when the pattern was set),
// so the pattern is pinned in device space like the gradients.
struct cnvs_pattern {
    uint8_t const *__counted_by(len) data;
    int len;
    int w, h;
    enum canvas_pattern_repeat repeat;
    cnvs_mat to_pattern;
};

struct canvas_state {
    cnvs_mat ctm;
    cnvs_unpremul fill;
    enum cnvs_paint_kind fill_kind;
    struct cnvs_gradient fill_grad;
    struct cnvs_pattern fill_pattern;
    cnvs_unpremul stroke;
    enum cnvs_paint_kind stroke_kind;
    struct cnvs_gradient stroke_grad;
    struct cnvs_pattern stroke_pattern;
    float global_alpha;
    enum canvas_composite_op composite;  // globalCompositeOperation
    float line_width;
    enum cnvs_line_join line_join;
    enum cnvs_line_cap line_cap;
    float miter_limit;
    float dash[CANVAS_DASH_MAX];
    int dash_count;
    float dash_offset;
    float font_size;  // text size in user px (Canvas default 10px)
    enum canvas_text_align text_align;
    enum canvas_text_baseline text_baseline;
    enum canvas_direction direction;  // paragraph direction: resolves start/end and
                                 // is the base direction text shapes under
    bool image_smoothing_enabled;
    enum canvas_image_smoothing_quality image_smoothing_quality;
    cnvs_unpremul shadow_color;  // shadow off when its alpha is 0
    float shadow_blur;           // device px (a Gaussian radius; CTM does not apply)
    float shadow_offset_x, shadow_offset_y;  // device px (CTM does not apply)
    // CSS filter list (canvas_add_filter_*): compiled colour-filter functions,
    // applied in call order to every painted op's tile before it composites.
    // A dynamic per-state array like the clip mask below: held by value, so
    // save() deep-copies it and restore()/reset() free it; NULL/0 = no filter.
    cnvs_filter *__counted_by(filter_count) filters;
    int filter_count;
    // Clip coverage, one byte per canvas pixel (NULL = open).  Held by value in
    // the state, so save() snapshots it and restore() brings it back; clip()
    // intersects the current path's coverage into it.
    uint8_t *__counted_by(clip_len) clip_mask;
    int clip_len;
};

struct canvas {
    int width;
    int height;
    // The working colour space (canvas.h): CANVAS_WS_SRGB is the legacy bypass
    // (no transfer ever runs, byte-identical to before this field existed);
    // CANVAS_WS_LINEAR composites in extended linear sRGB.  Chosen at creation,
    // immutable -- it is NOT in struct canvas_state, so save/restore never touch
    // it, and reset/resize leave it as the constructor set it.
    enum canvas_working_space space;
    cnvs_premul *__counted_by(target_len) target;  // premultiplied; all-zero == transparent
    int target_len;                                // == width * height
    struct canvas_state cur;
    struct canvas_state *__counted_by(stack_cap) stack;
    int nsaved;
    int stack_cap;
    struct cnvs_path path;
    struct cnvs_path text_path;  // scratch glyph outlines (fill_text/stroke_text)
    struct cnvs_font *__single font;  // cached for cur.font_size; rebuilt when it changes
    float font_built_size;
    struct cnvs_text_cache text_cache;  // params->derived-data memo of Core Text results:
                                 // shaped lines + canonical glyph curves, checked
                                 // before the boundary is called (cnvs_text.h)
    cnvs_vec2 cur_user;  // current point in user space (path.cur is device space)
    struct cnvs_verts scratch_verts;  // stroke triangle output, fed to the coverage rasterizer
    struct cnvs_cover cover;
    uint8_t *__counted_by(cov_cap) cov;     // per-pixel coverage for the current op's bbox
    int cov_cap;
    cnvs_premul *__counted_by(tile_cap) tile;  // premultiplied tile for the current op's bbox
    int tile_cap;
    float *__counted_by(trow_cap) trow;    // one row of gradient parameters (vectorized solve)
    int trow_cap;
    cnvs_unpremul *__counted_by(crow_cap) crow;  // one row of gradient colours (vectorized stop lerp)
    int crow_cap;
    struct cnvs_recorder *__single rec;  // NULL unless canvas_record_to is active
    struct cnvs_owned_image *__single owned_images;  // replayed `image` blocks
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
    cnvs_premul *__counted_by(blur_tmp_cap) blur_tmp;
    int blur_tmp_cap;
    // drawImage minification scratch: the source's premultiplied mip chain
    // (level 0 = the source premultiplied, then ceil-halves), rebuilt per
    // minifying draw at medium/high smoothing quality.  A borrowed source
    // buffer has no identity to cache a pyramid against; a first-class image
    // type will own this cost via an explicit build call.
    uint8_t *__counted_by(mips_cap) mips;
    int mips_cap;
};

static cnvs_vec2 xf(struct canvas *__single cv, float x, float y);
static bool ensure_tile(struct canvas *__single cv, int npix);
static int sigma_box_radius(float sigma);
static void shadow_offset_split(float v, int *__single whole, int *__single k256);
struct canvas_image;  // reified drawImage source; defined with its API below
static void draw_image_quad(struct canvas *__single cv,
                            uint8_t const *__counted_by(slen) src, int slen,
                            int sw, int sh, float sx, float sy,
                            float sww, float shh, float dx, float dy,
                            float dw, float dh, bool premul_src,
                            enum canvas_color_type src_ct,
                            bool quality_tiers, bool chain_on_demand,
                            struct canvas_image const *__single img,
                            cnvs_mip hi, cnvs_mip lo, float lt);

// Reset a pattern to empty (no source).  Counts first: a NULL pointer must never
// be paired with a positive count under -fbounds-safety.  An empty pattern stays
// consistent across the state copies that save/restore make.
static void pattern_reset(struct cnvs_pattern *p) {
    p->len = 0;
    p->data = NULL;
    p->w = 0;
    p->h = 0;
    p->repeat = CANVAS_NO_REPEAT;
    p->to_pattern = cnvs_mat_identity();
}

// The initial drawing state (Canvas defaults): identity transform, opaque black
// fill/stroke, source-over, 1px miter strokes, no dash, 10px text, open clip.
// Shared by canvas and canvas_reset so the two can't drift.  Assigned
// field by field (not an init list): a compound literal of side-effecting calls
// has indeterminate evaluation order, which -fbounds-safety flags for a struct
// carrying a __counted_by member.  Clearing the gradient scratch isn't needed
// (read only when the kind is GRADIENT), but the patterns must be cleared so the
// borrowed-buffer (data, len) pair stays consistent when the state is copied.
static void state_defaults(struct canvas_state *s) {
    s->ctm = cnvs_mat_identity();
    s->fill = cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 1.0f);
    s->fill_kind = CNVS_PAINT_SOLID;
    pattern_reset(&s->fill_pattern);
    s->stroke = cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 1.0f);
    s->stroke_kind = CNVS_PAINT_SOLID;
    pattern_reset(&s->stroke_pattern);
    s->global_alpha = 1.0f;
    s->composite = CANVAS_OP_SOURCE_OVER;
    s->line_width = 1.0f;
    s->line_join = CNVS_JOIN_MITER;
    s->line_cap = CNVS_CAP_BUTT;
    s->miter_limit = 10.0f;
    s->dash_count = 0;
    s->dash_offset = 0.0f;
    s->font_size = 10.0f;
    s->text_align = CANVAS_ALIGN_START;
    s->text_baseline = CANVAS_BASELINE_ALPHABETIC;
    s->direction = CANVAS_DIRECTION_LTR;
    s->image_smoothing_enabled = true;
    s->image_smoothing_quality = CANVAS_SMOOTHING_LOW;
    s->shadow_color = cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 0.0f);  // transparent: off
    s->shadow_blur = 0.0f;
    s->shadow_offset_x = 0.0f;
    s->shadow_offset_y = 0.0f;
    // Drop the filter list and the clip: zero each count before NULLing its
    // pointer so a __counted_by invariant never sees NULL with a positive count.
    s->filter_count = 0;
    s->filters = NULL;
    s->clip_len = 0;
    s->clip_mask = NULL;
}

struct canvas *__single canvas(int width, int height) {
    return canvas_in_space(width, height, CANVAS_WS_SRGB);
}

struct canvas *__single canvas_in_space(int width, int height,
                                        enum canvas_working_space space) {
    if (width <= 0 || height <= 0 ||
        width > CANVAS_DIM_MAX || height > CANVAS_DIM_MAX) {
        return NULL;
    }
    if (space != CANVAS_WS_SRGB && space != CANVAS_WS_LINEAR) {
        return NULL;  // an out-of-range space is a caller error, not a default
    }
    int const n = width * height;
    cnvs_premul *__counted_by_or_null(n) target = calloc((size_t)n, sizeof *target);
    if (!target) {
        return NULL;
    }
    struct canvas *__single cv = calloc(1, sizeof *cv);
    if (!cv) {
        free(target);
        return NULL;
    }
    cv->width = width;
    cv->height = height;
    cv->space = space;  // immutable; reset/resize never touch it
    cv->target_len = n;
    cv->target = target;  // count before pointer
    state_defaults(&cv->cur);
    cv->stack = NULL;
    cv->nsaved = 0;
    cv->stack_cap = 0;
    cnvs_path_init(&cv->path);
    cnvs_path_init(&cv->text_path);
    cv->font = NULL;
    cv->font_built_size = 0.0f;
    cnvs_text_cache_init(&cv->text_cache);
    cv->rec = NULL;
    cv->owned_images = NULL;
    return cv;
}

void canvas_free(struct canvas *__single cv) {
    if (!cv) {
        return;
    }
    cnvs_recorder_end(cv->rec);  // flush and close any active recording
    free(cv->target);
    for (int i = 0; i < cv->nsaved; i++) {
        free(cv->stack[i].filters);
        free(cv->stack[i].clip_mask);
    }
    free(cv->stack);
    free(cv->cur.filters);
    free(cv->cur.clip_mask);
    cnvs_font_free(cv->font);
    cnvs_text_cache_reset(&cv->text_cache);  // owned shaped lines + glyph curves
    cnvs_path_free(&cv->path);
    cnvs_path_free(&cv->text_path);
    cnvs_verts_free(&cv->scratch_verts);
    cnvs_cover_free(&cv->cover);
    free(cv->cov);
    free(cv->tile);
    free(cv->trow);
    free(cv->crow);
    free(cv->shadow_src);
    free(cv->shadow_dst);
    free(cv->blur_tmp);
    free(cv->mips);
    for (struct cnvs_owned_image *__single n = cv->owned_images; n;) {
        struct cnvs_owned_image *__single next = n->next;
        free(n->data);
        free(n);
        n = next;
    }
    free(cv);
}

bool cnvs_canvas_set_working_space(struct canvas *__single cv,
                                   enum canvas_working_space space) {
    // The replay seam for a leading `working_space` line: the canvas is fresh
    // and transparent (the parser only reaches here as the first command), so
    // re-stamping the immutable space is creation-time, not a mid-stream flip.
    if (space == CANVAS_WS_SRGB || space == CANVAS_WS_LINEAR) {
        cv->space = space;
        // Replaying a linear program onto a recording canvas re-emits the line,
        // so the round trip stays byte-idempotent (the no-op-for-sRGB emitter
        // keeps sRGB programs from gaining a line they never had).
        cnvs_rec_working_space(cv->rec, space);
    }
    return true;  // an out-of-range value leaves sRGB; the parser already
}                 // validated the name, so this only guards a direct caller

bool cnvs_canvas_own_image(struct canvas *__single cv,
                           uint8_t *__counted_by(len) px, int len) {
    struct cnvs_owned_image *__single node = calloc(1, sizeof *node);
    if (!node) {
        return false;
    }
    node->data = px;
    node->len = len;
    node->next = cv->owned_images;
    cv->owned_images = node;
    return true;
}

bool canvas_record_to(struct canvas *__single cv, char const *__null_terminated path) {
    cnvs_recorder_end(cv->rec);  // stop any prior recording first
    cv->rec = cnvs_recorder_begin(path);
    // A new file holds no blocks yet: forget what any prior recording emitted,
    // so warm cache entries serialize afresh into this one.
    cnvs_text_cache_unmark(&cv->text_cache);
    // The working space rides the very first line, before any draw -- but only
    // when it is non-sRGB, so every sRGB program stays byte-identical (the
    // emitter is a no-op for CANVAS_WS_SRGB).  Replay applies it to the fresh
    // canvas before the first colour interns.
    cnvs_rec_working_space(cv->rec, cv->space);
    return cv->rec != NULL;
}

bool canvas_is_context_lost(struct canvas *__single cv) {
    (void)cv;
    return false;  // a headless renderer owns its backing store; never lost.
}

static bool stack_reserve(struct canvas *__single cv, int need) {
    if (need <= cv->stack_cap) {
        return true;
    }
    int const newcap = cnvs_grow_cap(cv->stack_cap, need);
    struct canvas_state *ns =
        realloc(cv->stack, (size_t)newcap * sizeof *ns);
    if (!ns) {
        return false;
    }
    cv->stack = ns;
    cv->stack_cap = newcap;
    return true;
}

void canvas_save(struct canvas *__single cv) {
    if (cv->rec) { cnvs_rec_op(cv->rec, "save"); }
    if (!stack_reserve(cv, cv->nsaved + 1)) {
        return;
    }
    cv->stack[cv->nsaved] = cv->cur;
    // Give the saved entry its own copy of the clip mask so clip() can mutate
    // cur's independently.
    if (cv->cur.clip_mask) {
        int n = cv->cur.clip_len;
        uint8_t *copy = malloc((size_t)n);
        if (copy) {
            memcpy(copy, cv->cur.clip_mask, (size_t)n);
            cv->stack[cv->nsaved].clip_mask = copy;
            cv->stack[cv->nsaved].clip_len = n;
        } else {
            cv->stack[cv->nsaved].clip_mask = NULL;
            cv->stack[cv->nsaved].clip_len = 0;
        }
    }
    // Likewise the filter list, so add_filter/set_filter_none can mutate cur's
    // independently.  On allocation failure the saved entry keeps no filters
    // (best-effort, matching the clip's degraded copy above).
    if (cv->cur.filters) {
        int const n = cv->cur.filter_count;
        cnvs_filter *copy = malloc((size_t)n * sizeof *copy);
        if (copy) {
            memcpy(copy, cv->cur.filters, (size_t)n * sizeof *copy);
            cv->stack[cv->nsaved].filters = copy;
            cv->stack[cv->nsaved].filter_count = n;
        } else {
            cv->stack[cv->nsaved].filters = NULL;
            cv->stack[cv->nsaved].filter_count = 0;
        }
    }
    cv->nsaved += 1;
}

void canvas_restore(struct canvas *__single cv) {
    if (cv->rec) { cnvs_rec_op(cv->rec, "restore"); }
    if (cv->nsaved > 0) {
        cv->nsaved -= 1;
        free(cv->cur.filters);
        free(cv->cur.clip_mask);
        cv->cur = cv->stack[cv->nsaved];  // adopts the saved clip mask + filters
    }
}

void canvas_reset(struct canvas *__single cv) {
    // Recording continues across a reset: the cleared text cache means the
    // file's font-id space restarts with it (later text re-interns from 0 and
    // re-emits its blocks), which replay mirrors when it executes this line.
    if (cv->rec) { cnvs_rec_op(cv->rec, "reset"); }
    // Empty the saved-state stack (each entry may own clip-mask and filter-list
    // copies); keep the backing allocation for reuse.
    for (int i = 0; i < cv->nsaved; i++) {
        free(cv->stack[i].filters);
        free(cv->stack[i].clip_mask);
    }
    cv->nsaved = 0;
    // Drop the current filter list and clip mask, and restore every state field
    // to its default.
    free(cv->cur.filters);
    free(cv->cur.clip_mask);
    state_defaults(&cv->cur);
    // Discard the current path.
    cnvs_path_reset(&cv->path);
    cv->cur_user = (cnvs_vec2){ .x = 0.0f, .y = 0.0f };
    // Drop the text caches too.  Keeping them would also be correct -- the cache
    // is a pure memo of boundary results, invisible to rendering -- but reset()'s
    // contract is "as if freshly created", and reset is the natural point for a
    // long-lived canvas to shed accumulated memory, so warm entries go (and
    // resize(), which resets, starts its new canvas cold like create() does).
    cnvs_text_cache_reset(&cv->text_cache);
    // Clear the whole bitmap to transparent black: a destination-out of a
    // unit-alpha splat leaves dst*(1 - 1) = 0 everywhere, with the clip open
    // (state_defaults just dropped the mask; no tile, so a reset can't fail
    // on allocation).
    cnvs_blend_solid(cv, 0, 0, cv->width, cv->height,
                     (cnvs_premul){ .r = 0, .g = 0, .b = 0,
                                    .a = (_Float16)1.0f },
                     NULL, NULL, 0, CANVAS_OP_DESTINATION_OUT);
}

bool canvas_resize(struct canvas *__single cv, int width, int height) {
    if (width <= 0 || height <= 0 ||
        width > CANVAS_DIM_MAX || height > CANVAS_DIM_MAX) {
        return false;
    }
    // Build the new-sized target first; on failure leave the canvas intact.
    int const n = width * height;
    cnvs_premul *__counted_by_or_null(n) nt = calloc((size_t)n, sizeof *nt);
    if (!nt) {
        return false;
    }
    free(cv->target);
    cv->target_len = n;
    cv->target = nt;  // count before pointer
    cv->width = width;
    cv->height = height;
    // Record `resize` only once it has succeeded (a failed resize changes
    // nothing), then swallow the reset it expands to -- the file keeps the op
    // the caller issued, and replay's resize implies the same reset.
    if (cv->rec) { cnvs_rec_ints(cv->rec, "resize", (int[]){ width, height }, 2); }
    cnvs_rec_enter(cv->rec);
    // reset() drops the (now wrong-sized) clip masks and saved stack, restores the
    // default state, and clears the fresh bitmap to transparent black.
    canvas_reset(cv);
    cnvs_rec_leave(cv->rec);
    return true;
}

void canvas_translate(struct canvas *__single cv, float tx, float ty) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "translate", (float[]){ tx, ty }, 2); }
    cv->cur.ctm = cnvs_mat_mul(cv->cur.ctm, cnvs_mat_translate(tx, ty));
}

void canvas_scale(struct canvas *__single cv, float sx, float sy) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "scale", (float[]){ sx, sy }, 2); }
    cv->cur.ctm = cnvs_mat_mul(cv->cur.ctm, cnvs_mat_scale(sx, sy));
}

void canvas_rotate(struct canvas *__single cv, float radians) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "rotate", (float[]){ radians }, 1); }
    cv->cur.ctm = cnvs_mat_mul(cv->cur.ctm, cnvs_mat_rotate(radians));
}

void canvas_transform(struct canvas *__single cv,
                      float a, float b, float c, float d, float e, float f) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "transform", (float[]){ a, b, c, d, e, f }, 6); }
    cnvs_mat const m = { .a = a, .b = b, .c = c, .d = d, .e = e, .f = f };
    cv->cur.ctm = cnvs_mat_mul(cv->cur.ctm, m);
}

void canvas_set_transform(struct canvas *__single cv,
                          float a, float b, float c, float d, float e, float f) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_transform", (float[]){ a, b, c, d, e, f }, 6); }
    cv->cur.ctm = (cnvs_mat){ .a = a, .b = b, .c = c, .d = d, .e = e, .f = f };
}

void canvas_reset_transform(struct canvas *__single cv) {
    if (cv->rec) { cnvs_rec_op(cv->rec, "reset_transform"); }
    cv->cur.ctm = cnvs_mat_identity();
}

canvas_matrix canvas_get_transform(struct canvas *__single cv) {
    cnvs_mat const m = cv->cur.ctm;
    return (canvas_matrix){ .a = m.a, .b = m.b, .c = m.c,
                            .d = m.d, .e = m.e, .f = m.f };
}

// Intern one untagged (encoded sRGB) colour into the canvas's working space --
// the single entry-side transfer gate every colour the API takes flows through
// (fill/stroke/shadow/drop-shadow colours and gradient stops; image and pattern
// pixels intern at their own seams).  On an sRGB canvas this is a literal bypass
// of the legacy code: clamp01 each channel exactly as before, no transfer.  On a
// linear canvas the RGB channels decode sRGB->linear (cnvs_srgb_to_linear, the
// odd extension, total over R) with NO [0,1] clamp -- an extended (out-of-gamut)
// input must propagate, and the clamp would crush it -- while alpha still clamps
// to [0,1] (alpha is a coverage coordinate, never a colour, and the premultiply
// and readback both assume it in range).  The recorder logs the RAW input floats
// upstream of this, so a recorded program is space-agnostic at the colour lines;
// replay re-interns through the same gate on its own canvas.
static cnvs_unpremul intern_color(struct canvas *__single cv,
                                  float r, float g, float b, float a) {
    if (cv->space == CANVAS_WS_LINEAR) {
        return cnvs_unpremul_of(cnvs_srgb_to_linear(r), cnvs_srgb_to_linear(g),
                                cnvs_srgb_to_linear(b), cnvs_clamp01(a));
    }
    return cnvs_unpremul_of(cnvs_clamp01(r), cnvs_clamp01(g),
                            cnvs_clamp01(b), cnvs_clamp01(a));
}

void canvas_set_fill_rgba(struct canvas *__single cv, float r, float g, float b, float a) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_fill_rgba", (float[]){ r, g, b, a }, 4); }
    cv->cur.fill = intern_color(cv, r, g, b, a);
    cv->cur.fill_kind = CNVS_PAINT_SOLID;
}

// Average CTM scale, used to bake user-space radii into device space.
static float ctm_scale(cnvs_mat m) {
    float const det = m.a * m.d - m.b * m.c;
    return sqrtf(fabsf(det));
}

// Initialise a gradient struct in device space (the CTM is baked in now); the
// caller sets the matching paint kind to GRADIENT.
// Every grad_set_* stamps the canvas's (immutable) working space onto the
// gradient so its Oklab path can take stops working<->linear at eval time, and
// resets the interpolation space to the sRGB default -- a freshly set gradient
// interpolates in sRGB until canvas_set_*_gradient_interpolation says otherwise,
// matching the Canvas object's per-gradient default and keeping legacy
// behaviour byte-identical.
static void grad_set_linear(struct canvas *__single cv, struct cnvs_gradient *gr,
                            float x0, float y0, float x1, float y1) {
    gr->kind = CNVS_GRAD_LINEAR;
    gr->p0 = xf(cv, x0, y0);
    gr->p1 = xf(cv, x1, y1);
    gr->r0 = 0.0f;
    gr->r1 = 0.0f;
    gr->angle = 0.0f;
    gr->stop_count = 0;
    gr->interp = CNVS_INTERP_SRGB;
    gr->space = cv->space;
}

static void grad_set_radial(struct canvas *__single cv, struct cnvs_gradient *gr, float x0,
                            float y0, float r0, float x1, float y1, float r1) {
    float const s = ctm_scale(cv->cur.ctm);
    gr->kind = CNVS_GRAD_RADIAL;
    gr->p0 = xf(cv, x0, y0);
    gr->p1 = xf(cv, x1, y1);
    gr->r0 = r0 * s;
    gr->r1 = r1 * s;
    gr->angle = 0.0f;
    gr->stop_count = 0;
    gr->interp = CNVS_INTERP_SRGB;
    gr->space = cv->space;
}

// Rotation angle (radians) of the CTM's x-axis basis, for baking a conic
// gradient's start angle into device space.  Exact for similarity transforms;
// skew / non-uniform scale distort the angles (as they do the radial circles).
static float ctm_rotation(cnvs_mat m) {
    return atan2f(m.b, m.a);
}

static void grad_set_conic(struct canvas *__single cv, struct cnvs_gradient *gr,
                           float start_angle, float cx, float cy) {
    gr->kind = CNVS_GRAD_CONIC;
    gr->p0 = xf(cv, cx, cy);  // centre in device space
    gr->p1 = gr->p0;
    gr->r0 = 0.0f;
    gr->r1 = 0.0f;
    gr->angle = start_angle + ctm_rotation(cv->cur.ctm);
    gr->stop_count = 0;
    gr->interp = CNVS_INTERP_SRGB;
    gr->space = cv->space;
}

// Configure `p` to tile `src` (borrowed) with `repeat`, pinned in device space
// via the inverse of the current CTM.  The (data, len) pair is set together so
// -fbounds-safety can verify the __counted_by(len) invariant: src is itself
// __counted_by(w*h*4), exactly the new len.
static void pattern_set(struct canvas *__single cv, struct cnvs_pattern *p,
                        uint8_t const *__counted_by(w * h * 4) src, int w, int h,
                        enum canvas_pattern_repeat repeat) {
    p->data = src;
    p->len = w * h * 4;
    p->w = w;
    p->h = h;
    p->repeat = repeat;
    p->to_pattern = cnvs_mat_invert(cv->cur.ctm);  // device -> pattern image space
}

void canvas_set_fill_linear_gradient(struct canvas *__single cv,
                                     float x0, float y0, float x1, float y1) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_fill_linear_gradient", (float[]){ x0, y0, x1, y1 }, 4); }
    grad_set_linear(cv, &cv->cur.fill_grad, x0, y0, x1, y1);
    cv->cur.fill_kind = CNVS_PAINT_GRADIENT;
}

void canvas_set_fill_radial_gradient(struct canvas *__single cv, float x0, float y0,
                                     float r0, float x1, float y1, float r1) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_fill_radial_gradient", (float[]){ x0, y0, r0, x1, y1, r1 }, 6); }
    grad_set_radial(cv, &cv->cur.fill_grad, x0, y0, r0, x1, y1, r1);
    cv->cur.fill_kind = CNVS_PAINT_GRADIENT;
}

void canvas_set_fill_conic_gradient(struct canvas *__single cv, float start_angle,
                                    float x, float y) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_fill_conic_gradient", (float[]){ start_angle, x, y }, 3); }
    grad_set_conic(cv, &cv->cur.fill_grad, start_angle, x, y);
    cv->cur.fill_kind = CNVS_PAINT_GRADIENT;
}

void canvas_add_fill_color_stop(struct canvas *__single cv, float offset,
                                float r, float g, float b, float a) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "add_fill_color_stop", (float[]){ offset, r, g, b, a }, 5); }
    cnvs_gradient_add_stop(&cv->cur.fill_grad, cnvs_clamp01(offset),
                           intern_color(cv, r, g, b, a));
}

void canvas_set_fill_gradient_interpolation(struct canvas *__single cv,
                                            enum cnvs_gradient_interp interp) {
    if (cv->rec) { cnvs_rec_gradient_interp(cv->rec, "set_fill_gradient_interpolation", interp); }
    cv->cur.fill_grad.interp = interp;
}

void canvas_set_fill_pattern(struct canvas *__single cv,
                             uint8_t const *__counted_by(w * h * 4) src,
                             int w, int h, enum canvas_pattern_repeat repeat) {
    if (!rgba8_dims_ok(w, h)) {
        return;  // invalid dimensions: leave the fill paint unchanged
    }
    if (cv->rec) {
        // The pattern pixels ride a content-deduped image block; when the
        // block can't be carried (caps/OOM) the op line is skipped with it.
        int const id = cnvs_rec_image(cv->rec, src, w * h * 4, w, h,
                                      CANVAS_COLOR_UNORM8, CANVAS_ALPHA_UNPREMUL);
        if (id >= 0) { cnvs_rec_pattern(cv->rec, "set_fill_pattern", id, repeat); }
    }
    pattern_set(cv, &cv->cur.fill_pattern, src, w, h, repeat);
    cv->cur.fill_kind = CNVS_PAINT_PATTERN;
}

void canvas_set_stroke_linear_gradient(struct canvas *__single cv,
                                       float x0, float y0, float x1, float y1) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_stroke_linear_gradient", (float[]){ x0, y0, x1, y1 }, 4); }
    grad_set_linear(cv, &cv->cur.stroke_grad, x0, y0, x1, y1);
    cv->cur.stroke_kind = CNVS_PAINT_GRADIENT;
}

void canvas_set_stroke_radial_gradient(struct canvas *__single cv, float x0, float y0,
                                       float r0, float x1, float y1, float r1) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_stroke_radial_gradient", (float[]){ x0, y0, r0, x1, y1, r1 }, 6); }
    grad_set_radial(cv, &cv->cur.stroke_grad, x0, y0, r0, x1, y1, r1);
    cv->cur.stroke_kind = CNVS_PAINT_GRADIENT;
}

void canvas_set_stroke_conic_gradient(struct canvas *__single cv, float start_angle,
                                      float x, float y) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_stroke_conic_gradient", (float[]){ start_angle, x, y }, 3); }
    grad_set_conic(cv, &cv->cur.stroke_grad, start_angle, x, y);
    cv->cur.stroke_kind = CNVS_PAINT_GRADIENT;
}

void canvas_add_stroke_color_stop(struct canvas *__single cv, float offset,
                                  float r, float g, float b, float a) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "add_stroke_color_stop", (float[]){ offset, r, g, b, a }, 5); }
    cnvs_gradient_add_stop(&cv->cur.stroke_grad, cnvs_clamp01(offset),
                           intern_color(cv, r, g, b, a));
}

void canvas_set_stroke_gradient_interpolation(struct canvas *__single cv,
                                              enum cnvs_gradient_interp interp) {
    if (cv->rec) { cnvs_rec_gradient_interp(cv->rec, "set_stroke_gradient_interpolation", interp); }
    cv->cur.stroke_grad.interp = interp;
}

void canvas_set_stroke_pattern(struct canvas *__single cv,
                               uint8_t const *__counted_by(w * h * 4) src,
                               int w, int h, enum canvas_pattern_repeat repeat) {
    if (!rgba8_dims_ok(w, h)) {
        return;
    }
    if (cv->rec) {
        int const id = cnvs_rec_image(cv->rec, src, w * h * 4, w, h,
                                      CANVAS_COLOR_UNORM8, CANVAS_ALPHA_UNPREMUL);
        if (id >= 0) { cnvs_rec_pattern(cv->rec, "set_stroke_pattern", id, repeat); }
    }
    pattern_set(cv, &cv->cur.stroke_pattern, src, w, h, repeat);
    cv->cur.stroke_kind = CNVS_PAINT_PATTERN;
}

void canvas_set_global_alpha(struct canvas *__single cv, float alpha) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_global_alpha", (float[]){ alpha }, 1); }
    cv->cur.global_alpha = cnvs_clamp01(alpha);
}

void canvas_set_global_composite_operation(struct canvas *__single cv,
                                           enum canvas_composite_op op) {
    if ((int)op < 0 || (int)op >= CNVS_BLEND_MODE_COUNT) {
        return;
    }
    if (cv->rec) { cnvs_rec_composite(cv->rec, op); }
    cv->cur.composite = op;
}

void canvas_set_shadow_color_rgba(struct canvas *__single cv,
                                  float r, float g, float b, float a) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_shadow_color_rgba", (float[]){ r, g, b, a }, 4); }
    cv->cur.shadow_color = intern_color(cv, r, g, b, a);
}

void canvas_set_shadow_blur(struct canvas *__single cv, float blur) {
    if (isfinite(blur) && blur >= 0.0f) {  // spec: ignore negative / non-finite
        // The hook sits inside the guard: an ignored call records nothing
        // (and %.9g of a non-finite would not reparse anyway).
        if (cv->rec) { cnvs_rec_floats(cv->rec, "set_shadow_blur", (float[]){ blur }, 1); }
        cv->cur.shadow_blur = blur;
    }
}

void canvas_set_shadow_offset_x(struct canvas *__single cv, float offset) {
    if (isfinite(offset)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "set_shadow_offset_x", (float[]){ offset }, 1); }
        cv->cur.shadow_offset_x = offset;
    }
}

void canvas_set_shadow_offset_y(struct canvas *__single cv, float offset) {
    if (isfinite(offset)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "set_shadow_offset_y", (float[]){ offset }, 1); }
        cv->cur.shadow_offset_y = offset;
    }
}

void canvas_set_filter_none(struct canvas *__single cv) {
    if (cv->rec) { cnvs_rec_op(cv->rec, "set_filter_none"); }
    free(cv->cur.filters);
    cv->cur.filter_count = 0;
    cv->cur.filters = NULL;
}

// Append one compiled function to the state's filter list.  The list grows by
// exactly one (filter lists stay a few entries long); on allocation failure the
// call is dropped, leaving the list as it was (best-effort, like the path
// builders).  Non-finite amounts never reach here -- each canvas_add_filter_*
// ignores those calls outright, per spec.
static void filter_append(struct canvas *__single cv, cnvs_filter f) {
    int const n = cv->cur.filter_count;
    cnvs_filter *nf = realloc(cv->cur.filters, ((size_t)n + 1) * sizeof *nf);
    if (!nf) {
        return;
    }
    nf[n] = f;
    cv->cur.filters = nf;
    cv->cur.filter_count = n + 1;
}

// The unbounded-above amounts (brightness/contrast/saturate) clamp only below;
// the clamp guarantee still holds (D1): NaN lands on the 0 bound, not through.
static float clamp_lo(float v) {
    return v > 0.0f ? v : 0.0f;   // <= 0, or NaN -> 0
}

void canvas_add_filter_blur(struct canvas *__single cv, float px) {
    if (!isfinite(px) || px < 0.0f) {
        return;  // spec: ignore an unparseable (or negative) length
    }
    // filter blur(px) IS the Gaussian's stdDev (where shadowBlur is twice it);
    // px == 0 maps to radius 0 -- an identity blur, so nothing is appended.
    // The recorder hooks sit inside each add_filter_*'s accept guard: an
    // ignored call records nothing, and the raw amount rides the line (replay
    // re-clamps and re-compiles it through this same code).
    int const r = sigma_box_radius(px);
    if (r > 0) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "add_filter_blur", (float[]){ px }, 1); }
        filter_append(cv, cnvs_filter_blur(r));
    }
}

void canvas_add_filter_brightness(struct canvas *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "add_filter_brightness", (float[]){ amount }, 1); }
        filter_append(cv, cnvs_filter_brightness(clamp_lo(amount)));
    }
}

void canvas_add_filter_contrast(struct canvas *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "add_filter_contrast", (float[]){ amount }, 1); }
        filter_append(cv, cnvs_filter_contrast(clamp_lo(amount)));
    }
}

void canvas_add_filter_drop_shadow(struct canvas *__single cv, float dx, float dy,
                                   float blur, float r, float g, float b,
                                   float a) {
    if (!isfinite(dx) || !isfinite(dy) || !isfinite(blur) || blur < 0.0f) {
        return;  // spec: ignore an unparseable (or negative-blur) drop-shadow
    }
    if (!(cnvs_clamp01(a) > 0.0f)) {
        return;  // a fully transparent shadow composites as nothing
    }
    if (cv->rec) {
        // dx/dy/blur are guarded finite and ride raw; the colour rides
        // clamped (identity for in-range values, and a NaN channel would
        // otherwise print as unreparseable "nan").
        cnvs_rec_floats(cv->rec, "add_filter_drop_shadow",
                        (float[]){ dx, dy, blur, cnvs_clamp01(r), cnvs_clamp01(g),
                                   cnvs_clamp01(b), cnvs_clamp01(a) }, 7);
    }
    // The offsets split onto the 1/256th-px grid (shadow_offset_split, as for
    // shadowOffset{X,Y}); blur IS the Gaussian's stdDev, like blur() -- but
    // unlike blur(), radius 0 is a real entry (a sharp shadow, not identity).
    int dxw, dxk, dyw, dyk;
    shadow_offset_split(dx, &dxw, &dxk);
    shadow_offset_split(dy, &dyw, &dyk);
    // The shadow tint composites into the tile in the working space, so it
    // interns through the same gate as every other untagged colour: bypass on
    // sRGB (the clamped values land identically), decode sRGB->linear on a
    // linear canvas.  The recorder above logged the raw input.
    cnvs_unpremul const tint = intern_color(cv, r, g, b, a);
    filter_append(cv, cnvs_filter_drop_shadow(
                          dxw, dxk, dyw, dyk,
                          sigma_box_radius(blur), (float)tint.r, (float)tint.g,
                          (float)tint.b, (float)tint.a));
}

void canvas_add_filter_grayscale(struct canvas *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "add_filter_grayscale", (float[]){ amount }, 1); }
        filter_append(cv, cnvs_filter_grayscale(cnvs_clamp01(amount)));
    }
}

void canvas_add_filter_hue_rotate(struct canvas *__single cv, float radians) {
    if (isfinite(radians)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "add_filter_hue_rotate", (float[]){ radians }, 1); }
        filter_append(cv, cnvs_filter_hue_rotate(radians));
    }
}

void canvas_add_filter_invert(struct canvas *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "add_filter_invert", (float[]){ amount }, 1); }
        filter_append(cv, cnvs_filter_invert(cnvs_clamp01(amount)));
    }
}

void canvas_add_filter_opacity(struct canvas *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "add_filter_opacity", (float[]){ amount }, 1); }
        filter_append(cv, cnvs_filter_opacity(cnvs_clamp01(amount)));
    }
}

void canvas_add_filter_saturate(struct canvas *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "add_filter_saturate", (float[]){ amount }, 1); }
        filter_append(cv, cnvs_filter_saturate(clamp_lo(amount)));
    }
}

void canvas_add_filter_sepia(struct canvas *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "add_filter_sepia", (float[]){ amount }, 1); }
        filter_append(cv, cnvs_filter_sepia(cnvs_clamp01(amount)));
    }
}

static cnvs_vec2 xf(struct canvas *__single cv, float x, float y) {
    return cnvs_mat_apply(cv->cur.ctm, (cnvs_vec2){ .x = x, .y = y });
}

// Integer device-space bounding box of a point set, padded by `margin` device
// pixels on every side, clamped to the canvas.  The margin is applied *before*
// the canvas clamp so a shape just off-canvas still gets a box for the part of
// its margin (e.g. a blur's soft skirt) that reaches on-canvas.
typedef struct {
    int x, y, w, h;
} cbbox;

// Defined with the shadow machinery below; called from the paint tails above
// it (blend_tile, paint_tile_solid).
static void emit_shadow(struct canvas *__single cv, cbbox b,
                        bool from_tile, bool with_cov, _Float16 base);

static cbbox points_bbox(struct canvas *__single cv,
                         cnvs_vec2 const *__counted_by(n) pts, int n, int margin) {
    if (n <= 0) {
        return (cbbox){ .x = 0, .y = 0, .w = 0, .h = 0 };
    }
    float minx = pts[0].x, maxx = pts[0].x, miny = pts[0].y, maxy = pts[0].y;
    for (int i = 1; i < n; i++) {
        cnvs_vec2 const p = pts[i];
        minx = p.x < minx ? p.x : minx;
        maxx = p.x > maxx ? p.x : maxx;
        miny = p.y < miny ? p.y : miny;
        maxy = p.y > maxy ? p.y : maxy;
    }
    float const m = (float)margin;
    float fx0 = floorf(minx) - m, fy0 = floorf(miny) - m;
    float fx1 = ceilf(maxx) + m, fy1 = ceilf(maxy) + m;
    int x0 = cnvs_f2i(fx0), y0 = cnvs_f2i(fy0), x1 = cnvs_f2i(fx1), y1 = cnvs_f2i(fy1);
    if (x0 < 0)          { x0 = 0; }
    if (y0 < 0)          { y0 = 0; }
    if (x1 > cv->width)  { x1 = cv->width; }
    if (y1 > cv->height) { y1 = cv->height; }
    cbbox b = { .x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0 };
    if (b.w < 0) { b.w = 0; }
    if (b.h < 0) { b.h = 0; }
    return b;
}

static bool ensure_tile(struct canvas *__single cv, int npix) {
    if (npix > cv->cov_cap) {
        uint8_t *nc = realloc(cv->cov, (size_t)npix);
        if (!nc) {
            return false;
        }
        cv->cov = nc;
        cv->cov_cap = npix;
    }
    if (npix > cv->tile_cap) {  // one premultiplied pixel per cell
        cnvs_premul *nt = realloc(cv->tile, (size_t)npix * sizeof *nt);
        if (!nt) {
            return false;
        }
        cv->tile = nt;
        cv->tile_cap = npix;
    }
    return true;
}

// Row buffers for the vectorized gradient fill, one entry per column of the
// current bbox: trow holds the solved parameters, crow the colours evaluated
// from them.  Two caps so the (pointer, count) pairs update independently
// under -fbounds-safety, like the shadow scratch pair.
static bool ensure_grad_rows(struct canvas *__single cv, int w) {
    if (w > cv->trow_cap) {
        float *nr = realloc(cv->trow, (size_t)w * sizeof *nr);
        if (!nr) {
            return false;
        }
        cv->trow = nr;
        cv->trow_cap = w;
    }
    if (w > cv->crow_cap) {
        cnvs_unpremul *nc = realloc(cv->crow, (size_t)w * sizeof *nc);
        if (!nc) {
            return false;
        }
        cv->crow = nc;
        cv->crow_cap = w;
    }
    return true;
}

// Grow the filter-blur scratch tile to at least npix pixels.
static bool ensure_blur_tmp(struct canvas *__single cv, int npix) {
    if (npix > cv->blur_tmp_cap) {
        cnvs_premul *nt = realloc(cv->blur_tmp, (size_t)npix * sizeof *nt);
        if (!nt) {
            return false;
        }
        cv->blur_tmp = nt;
        cv->blur_tmp_cap = npix;
    }
    return true;
}

// Total spread of the state's filter chain, in device pixels: each blur()
// entry's three box passes reach 3*r beyond the shape (emit_shadow's margin
// reasoning), a drop-shadow() entry additionally pushes its blurred shadow
// |dx|,|dy| further, and successive entries each add their own spread.  Every
// paint site widens its bbox by this *before* rasterizing coverage, so the
// soft edge has tile to land on; colour entries are 1:1 per pixel and add
// nothing.  points_bbox takes one margin for all four sides, so a
// drop-shadow's offset contributes max(|dx|, |dy|) symmetrically rather than
// per side -- at most a somewhat larger tile, never a clipped shadow.  Capped
// at a canvas dimension's worth -- a wider spread can't paint anything the
// clamp to the canvas wouldn't cut anyway.
static int filter_margin(struct canvas const *__single cv) {
    int m = 0;
    for (int i = 0; i < cv->cur.filter_count; i++) {
        cnvs_filter const f = cv->cur.filters[i];
        m += 3 * f.blur;
        if (f.shadow) {
            // A fractional offset spills one pixel past its floor.
            int const ax = (f.dx < 0 ? -f.dx : f.dx) + (f.kx ? 1 : 0);
            int const ay = (f.dy < 0 ? -f.dy : f.dy) + (f.ky ? 1 : 0);
            m += ax > ay ? ax : ay;
        }
        if (m > CANVAS_DIM_MAX) {
            return CANVAS_DIM_MAX;
        }
    }
    return m;
}

// One drop-shadow() entry over the w*h tile in cv->tile: composite the tile
// source-over ON TOP of a blurred, offset, tinted copy of its own alpha
// silhouette -- the Filter Effects drop-shadow, whose output (shadow under
// drawing, one image) flows on to any later entries in the list.  The shadow
// builds in the blur scratch, grown to two tiles so the box passes can
// ping-pong between its halves while the op's own pixels stay untouched for
// the final under-composite.  The spec blurs the alpha and tints after; blur
// is linear, so tinting first lands the same pixels, and the premultiplied
// shadow pixel is just the premultiplied tint scaled by the source alpha.
// The offset is folded into the build (read the alpha at (x-dx, y-dy),
// out-of-tile reads transparent) -- the paint site already widened the bbox
// by filter_margin, so the shifted, blurred shadow has tile to land on.  A
// fractional offset reads bilinearly: the four taps around the source point,
// weighted by the 1/256th fractions (kx == ky == 0 keeps the exact one-tap
// gather, the common integer path).  If the scratch can't grow the entry is
// skipped (the op paints shadowless), like the other best-effort OOM paths.
static void apply_drop_shadow(struct canvas *__single cv, cnvs_filter f, int w, int h) {
    int const npix = w * h;
    if (!ensure_blur_tmp(cv, 2 * npix)) {
        return;
    }
    cnvs_premul const tint = cnvs_premultiply(
        cnvs_unpremul_of(f.color[0], f.color[1], f.color[2], f.color[3]));
    if (f.kx || f.ky) {
        // Bilinear gather: the source point x - (dx + kx/256) sits between
        // columns xa = x-dx-1 (weight tx) and xb = x-dx (weight 1-tx), rows
        // likewise.  Out-of-tile taps are transparent.
        float const tx = (float)f.kx * (1.0f / 256.0f);
        float const ty = (float)f.ky * (1.0f / 256.0f);
        for (int y = 0; y < h; y++) {
            int const yb = y - f.dy, ya = yb - 1;
            float const wya = ty, wyb = 1.0f - ty;
            for (int x = 0; x < w; x++) {
                int const xb = x - f.dx, xa = xb - 1;
                float a = 0.0f;
                if (ya >= 0 && ya < h) {
                    if (xa >= 0 && xa < w) { a += tx * wya * (float)cv->tile[ya * w + xa].a; }
                    if (xb >= 0 && xb < w) { a += (1.0f - tx) * wya * (float)cv->tile[ya * w + xb].a; }
                }
                if (yb >= 0 && yb < h) {
                    if (xa >= 0 && xa < w) { a += tx * wyb * (float)cv->tile[yb * w + xa].a; }
                    if (xb >= 0 && xb < w) { a += (1.0f - tx) * wyb * (float)cv->tile[yb * w + xb].a; }
                }
                cv->blur_tmp[y * w + x] = (cnvs_premul){
                    .r = (_Float16)((float)tint.r * a),
                    .g = (_Float16)((float)tint.g * a),
                    .b = (_Float16)((float)tint.b * a),
                    .a = (_Float16)((float)tint.a * a),
                };
            }
        }
    } else {
        for (int y = 0; y < h; y++) {
            int const sy = y - f.dy;
            for (int x = 0; x < w; x++) {
                int const sx = x - f.dx;
                float a = sx >= 0 && sx < w && sy >= 0 && sy < h
                              ? (float)cv->tile[sy * w + sx].a
                              : 0.0f;
                cv->blur_tmp[y * w + x] = (cnvs_premul){
                    .r = (_Float16)((float)tint.r * a),
                    .g = (_Float16)((float)tint.g * a),
                    .b = (_Float16)((float)tint.b * a),
                    .a = (_Float16)((float)tint.a * a),
                };
            }
        }
    }
    if (f.blur > 0) {  // three box passes ~ a Gaussian, between the two halves
        for (int pass = 0; pass < 3; pass++) {
            blur_box_h_f16(cv->blur_tmp + npix, cv->blur_tmp, w, h, f.blur);
            blur_box_v_f16(cv->blur_tmp, cv->blur_tmp + npix, w, h, f.blur);
        }
    }
    for (int i = 0; i < npix; i++) {  // premultiplied source-over: tile OVER shadow
        cnvs_premul t = cv->tile[i], s = cv->blur_tmp[i];
        float const k = 1.0f - (float)t.a;
        cv->tile[i] = (cnvs_premul){
            .r = (_Float16)((float)t.r + (float)s.r * k),
            .g = (_Float16)((float)t.g + (float)s.g * k),
            .b = (_Float16)((float)t.b + (float)s.b * k),
            .a = (_Float16)((float)t.a + (float)s.a * k),
        };
    }
}

// Run the state's filter list (if any) over the freshly painted w*h tile in
// place, just before it composites.  Colour entries are 1:1 per pixel (maximal
// runs of them go through cnvs_filter_apply together); a blur() entry runs the
// separable box passes over the whole tile, and a drop-shadow() entry runs
// apply_drop_shadow -- both over the bbox the paint site already widened by
// filter_margin.  Because the list applies per entry, in order, a drop-shadow
// after a colour function shadows the recoloured drawing, and a colour
// function after a drop-shadow recolours the shadow too.  If the blur scratch
// can't be grown the spatial entry is skipped -- the op paints unblurred (or
// shadowless), inset in its widened (transparent) tile, like the other
// best-effort OOM paths.  Spec order is filter-then-shadow, and blend_tile
// runs exactly that: the shadowColor shadow is cast from the tile's alpha
// AFTER this filter pass (emit_shadow), so a blur()'s soft skirt and a
// drop-shadow()'s offset copy both shape the canvas shadow too.
static void apply_filters(struct canvas *__single cv, int w, int h) {
    int const count = cv->cur.filter_count;
    int const npix = w * h;
    int i = 0;
    while (i < count) {
        cnvs_filter const f = cv->cur.filters[i];
        if (f.shadow) {
            apply_drop_shadow(cv, f, w, h);
            i += 1;
        } else if (f.blur > 0) {
            if (ensure_blur_tmp(cv, npix)) {
                for (int pass = 0; pass < 3; pass++) {
                    blur_box_h_f16(cv->blur_tmp, cv->tile, w, h, f.blur);
                    blur_box_v_f16(cv->tile, cv->blur_tmp, w, h, f.blur);
                }
            }
            i += 1;
        } else {
            int j = i + 1;
            while (j < count && cv->cur.filters[j].blur == 0 &&
                   !cv->cur.filters[j].shadow) {
                j += 1;
            }
            cnvs_filter_apply(cv->cur.filters + i, j - i, cv->tile, npix);
            i = j;
        }
    }
}

// Add a path edge to the coverage rasterizer, translated into the tile's frame.
static void cover_edge(struct canvas *__single cv, cbbox b, cnvs_vec2 p0, cnvs_vec2 p1) {
    cnvs_cover_add_edge(&cv->cover, b.w, b.h, p0.x - (float)b.x, p0.y - (float)b.y,
                        p1.x - (float)b.x, p1.y - (float)b.y);
}

static void cover_path_edges(struct canvas *__single cv, cbbox b, struct cnvs_path const *p) {
    for (int s = 0; s < p->nsubs; s++) {
        cnvs_subpath const sp = p->subs[s];
        if (sp.count < 2) {
            continue;
        }
        for (int k = 0; k < sp.count; k++) {
            cnvs_vec2 const a = p->pts[sp.start + k];
            cnvs_vec2 const c = p->pts[sp.start + (k + 1) % sp.count];
            cover_edge(cv, b, a, c);
        }
    }
}

// --- the blend stage ----------------------------------------------------------
//
// W3C composite + blend math, premultiplied throughout, in _Float16 arithmetic
// end to end (docs/decisions/color-axis.md: f16 is the compute type, not just
// the storage type).  Blend modes: co = s*(1-da) + d*(1-sa) + T,
// ao = sa + da*(1-sa), with premultiplied term T = sa*da*B(Cb,Cs); the
// polynomial modes have divide-free T, the non-linear ones (dodge/burn/
// soft-light, HSL) un-premultiply once.  Porter-Duff: co = Fa*s + Fb*d.
//
// Everything below runs eight pixels at a time over channel planes
// (cnvs_planar.h); per-pixel branches are bitwise lane selects (half8_if_then_else)
// that compute both arms and discard a guarded divide's inf/NaN lanes
// exactly.
//
// Coverage (the op's AA plane x the clip mask) applies per the ruling in
// docs/rasterization.md: out = lerp(dst, blend(src, dst), cov) -- the
// uncovered fraction of a pixel keeps its destination.  Folding coverage
// into source alpha instead is identical math exactly when, in
// co = Fa*s + Fb*d, Fa is free of sa and Fb is affine in sa with Fb(0) = 1
// (coverage_folds): the over-family folds, every other mode blends at full
// strength and lerps.

static_assert(CANVAS_OP_COPY == 10 && CANVAS_OP_MULTIPLY == 11 &&
              CANVAS_OP_EXCLUSION == 21 && CANVAS_OP_HUE == 22 &&
              CNVS_BLEND_MODE_COUNT == 26,
              "blend8 dispatches on these mode bands");

// Coverage semantics (docs/rasterization.md): partial
// coverage applies in principle as a lerp between the destination and the
// full-strength blend, out = lerp(dst, blend(src, dst), cov) -- a pixel the
// shape doesn't cover keeps its destination.  Folding coverage into source
// alpha instead (src *= cov, premultiplied) is identical math only where the
// Porter-Duff form co = Fa*s + Fb*d has Fa free of sa and Fb affine in sa
// with Fb(0) = 1, AND the result never trips the output clamp: the modes
// below.  Those fold; every other mode takes the lerp in cnvs_blend.
// 'lighter' passes the Fa/Fb criterion (Fa = Fb = 1) but its co = s + d
// exceeds 1 exactly where it saturates, and clamp(c*s + d) != lerp(d,
// clamp(s + d), c) there -- the supersampled truth clamps per subsample, so
// lighter lerps (test_coverage_lerp measures the difference).
static bool coverage_folds(enum canvas_composite_op m) {
    switch ((int)m) {
        case CANVAS_OP_SOURCE_OVER:      // Fa = 1,      Fb = 1 - sa
        case CANVAS_OP_SOURCE_ATOP:      // Fa = da,     Fb = 1 - sa
        case CANVAS_OP_DESTINATION_OVER: // Fa = 1 - da, Fb = 1
        case CANVAS_OP_DESTINATION_OUT:  // Fa = 0,      Fb = 1 - sa
        case CANVAS_OP_XOR:              // Fa = 1 - da, Fb = 1 - sa
            return true;
        default:  // copy, the in/out family, dst-atop, lighter, blends
            return false;
    }
}

// Separable blend B(cb, cs), unpremultiplied; only the non-linear modes need it.
static half8 blend_sep8(enum canvas_composite_op mode, half8 cb, half8 cs) {
    half8 const zero = (half8)(_Float16)0.0f, one = (half8)(_Float16)1.0f;
    switch ((int)mode) {
        case CANVAS_OP_COLOR_DODGE:
            return half8_if_then_else(cb <= zero, zero,
                   half8_if_then_else(cs >= one,  one,
                       __builtin_elementwise_min(one, cb / (one - cs))));
        case CANVAS_OP_COLOR_BURN:
            return half8_if_then_else(cb >= one,  one,
                   half8_if_then_else(cs <= zero, zero,
                       one - __builtin_elementwise_min(one, (one - cb) / cs)));
        case CANVAS_OP_SOFT_LIGHT: {
            half8 dd = half8_if_then_else(cb <= (half8)(_Float16)0.25f,
                (((_Float16)16.0f * cb - (_Float16)12.0f) * cb + (_Float16)4.0f) * cb,
                __builtin_elementwise_sqrt(cb));
            return half8_if_then_else(cs <= (half8)(_Float16)0.5f,
                cb - (one - (_Float16)2.0f * cs) * cb * (one - cb),
                cb + ((_Float16)2.0f * cs - one) * (dd - cb));
        }
        default:
            return cs;
    }
}

// Premultiplied separable term T = sa*da*B per channel plane (s, d premultiplied).
static half8 blend_term8(enum canvas_composite_op mode,
                           half8 s, half8 d, half8 sa, half8 da) {
    switch ((int)mode) {
        case CANVAS_OP_MULTIPLY:    return s * d;
        case CANVAS_OP_SCREEN:      return sa * d + da * s - s * d;
        case CANVAS_OP_OVERLAY:
            return half8_if_then_else((_Float16)2.0f * d <= da,
                                      (_Float16)2.0f * s * d,
                                      sa * da - (_Float16)2.0f * (da - d) * (sa - s));
        case CANVAS_OP_DARKEN:      return __builtin_elementwise_min(s * da, d * sa);
        case CANVAS_OP_LIGHTEN:     return __builtin_elementwise_max(s * da, d * sa);
        case CANVAS_OP_HARD_LIGHT:
            return half8_if_then_else((_Float16)2.0f * s <= sa,
                                      (_Float16)2.0f * s * d,
                                      sa * da - (_Float16)2.0f * (da - d) * (sa - s));
        case CANVAS_OP_DIFFERENCE:
            return __builtin_elementwise_abs(s * da - d * sa);
        case CANVAS_OP_EXCLUSION:   return sa * d + da * s - (_Float16)2.0f * s * d;
        default: {  // color-dodge / color-burn / soft-light
            half8 const zero = (half8)(_Float16)0.0f;
            half8 const cs = half8_if_then_else(sa > zero, s / sa, zero);
            half8 const cb = half8_if_then_else(da > zero, d / da, zero);
            return sa * da * blend_sep8(mode, cb, cs);
        }
    }
}

// --- the linear working-space seam -------------------------------------------
//
// On a LINEAR canvas (cv->space == CANVAS_WS_LINEAR) the target holds extended
// linear sRGB.  Two families of blend mode:
//
//   * range-preserving (the over/Porter-Duff family, multiply, screen,
//     difference, exclusion) -- the blend expression is a valid operation in
//     any space, so it runs DIRECTLY on the linear premul slabs, finished with
//     the no-upper-clamp variant (cnvs_px8_clamp_premul_lin) so extended
//     colours survive to the output boundary;
//   * spec-defined-in-encoded-[0,1] (lighter, overlay, darken, lighten,
//     color-dodge, color-burn, hard-light, soft-light, and the non-separable
//     hue/saturation/color/luminosity) -- clip_color / the HSL math / the
//     dodge-burn-soft-light divides all need an encoded [0,1] precondition, so
//     these run through an encode->sRGB / blend / decode->linear wrapper.
//
// linear_direct() splits the two.  An sRGB canvas never reaches any of this --
// every linear-only branch is gated on cv->space upstream.
static bool linear_direct(enum canvas_composite_op mode) {
    switch ((int)mode) {
        case CANVAS_OP_SOURCE_OVER:      case CANVAS_OP_SOURCE_IN:
        case CANVAS_OP_SOURCE_OUT:       case CANVAS_OP_SOURCE_ATOP:
        case CANVAS_OP_DESTINATION_OVER: case CANVAS_OP_DESTINATION_IN:
        case CANVAS_OP_DESTINATION_OUT:  case CANVAS_OP_DESTINATION_ATOP:
        case CANVAS_OP_XOR:              case CANVAS_OP_COPY:
        case CANVAS_OP_MULTIPLY:         case CANVAS_OP_SCREEN:
        case CANVAS_OP_DIFFERENCE:       case CANVAS_OP_EXCLUSION:
            return true;
        default:  // lighter, overlay, darken, lighten, dodge/burn, hard/soft
            return false;  // light, and the non-separable HSL quartet
    }
}

// Per-lane sRGB transfer over a premultiplied slab.  The transfer is nonlinear,
// so it must act on STRAIGHT (unpremultiplied) colour: unpremultiply (guarded;
// a == 0 stays all-zero), transfer each RGB lane through the f32 scalar kernel
// (cnvs_color.c -- precision-sensitive pow runs in f32, narrowing once), then
// repremultiply.  Alpha rides through untouched -- it is never a colour.  Scalar
// per lane (libm has no half8 pow; the deferral note in cnvs_color.c applies):
// these wrap only the spec-in-sRGB blend modes, which are not the profiled hot
// path -- the over-family fast path never enters here.
typedef float (*chan_xfer)(float);
static cnvs_px8 px8_transfer(cnvs_px8 p, chan_xfer xf_chan) {
    cnvs_px8 o = p;
    for (int i = 0; i < 8; i++) {
        float const a = (float)p.a[i];
        if (a > 0.0f) {
            float const inv = 1.0f / a;
            o.r[i] = (_Float16)(xf_chan((float)p.r[i] * inv) * a);
            o.g[i] = (_Float16)(xf_chan((float)p.g[i] * inv) * a);
            o.b[i] = (_Float16)(xf_chan((float)p.b[i] * inv) * a);
        }
    }
    return o;
}

// Source-over for one planar slab: co = s + (1-sa)*d, ao = sa + (1-sa)*da --
// the same fold over every channel plane, alpha included.  `lin` picks the
// linear no-upper-clamp finish; an sRGB canvas keeps the legacy [0,ao] clamp,
// byte for byte.  Source-over is range-preserving, so on a linear canvas the
// fold runs directly on the linear slabs -- no encode wrapper.
static cnvs_px8 src_over8(cnvs_px8 s, cnvs_px8 d, bool lin) {
    half8 const fb = (half8)(_Float16)1.0f - s.a;
    cnvs_px8 co = { s.r + fb * d.r, s.g + fb * d.g,
                    s.b + fb * d.b, s.a + fb * d.a };
    return lin ? cnvs_px8_clamp_premul_lin(co) : cnvs_px8_clamp_premul(co);
}

// Eight pixels' unpremultiplied colour as three channel planes.
typedef struct {
    half8 r, g, b;
} rgb8;

static half8 lum8(rgb8 c) {
    return (_Float16)0.3f * c.r + (_Float16)0.59f * c.g + (_Float16)0.11f * c.b;
}

static rgb8 clip_color8(rgb8 c) {
    half8 const zero = (half8)(_Float16)0.0f, one = (half8)(_Float16)1.0f;
    half8 const l = lum8(c);
    half8 const n = __builtin_elementwise_min(c.r, __builtin_elementwise_min(c.g, c.b));
    half8 const x = __builtin_elementwise_max(c.r, __builtin_elementwise_max(c.g, c.b));
    short8 const lo = n < zero;  // lanes with a channel below 0: scale about l
    half8 const kn = l / (l - n);
    c.r = half8_if_then_else(lo, l + (c.r - l) * kn, c.r);
    c.g = half8_if_then_else(lo, l + (c.g - l) * kn, c.g);
    c.b = half8_if_then_else(lo, l + (c.b - l) * kn, c.b);
    // The W3C ClipColor computes n and x ONCE, before either fix: the x > 1
    // test and the kx denominator both read the pre-fix maximum even though
    // the channels they rescale may have just been pulled toward l.
    short8 const hi = x > one;   // lanes with a channel above 1
    half8 const kx = (one - l) / (x - l);
    c.r = half8_if_then_else(hi, l + (c.r - l) * kx, c.r);
    c.g = half8_if_then_else(hi, l + (c.g - l) * kx, c.g);
    c.b = half8_if_then_else(hi, l + (c.b - l) * kx, c.b);
    return c;
}

static rgb8 set_lum8(rgb8 c, half8 l) {
    half8 const dl = l - lum8(c);
    c.r += dl;
    c.g += dl;
    c.b += dl;
    return clip_color8(c);
}

static half8 saturation8(rgb8 c) {
    return __builtin_elementwise_max(c.r, __builtin_elementwise_max(c.g, c.b))
         - __builtin_elementwise_min(c.r, __builtin_elementwise_min(c.g, c.b));
}

// Set saturation: max channel -> s, min -> 0, mid proportional; an all-equal
// lane (mx <= mn) has no saturation axis and goes to black.
static rgb8 set_saturation8(rgb8 c, half8 s) {
    half8 const zero = (half8)(_Float16)0.0f;
    half8 const mn = __builtin_elementwise_min(c.r, __builtin_elementwise_min(c.g, c.b));
    half8 const mx = __builtin_elementwise_max(c.r, __builtin_elementwise_max(c.g, c.b));
    short8 const flat = mx <= mn;
    half8 const k = s / (mx - mn);
    return (rgb8){ .r = half8_if_then_else(flat, zero, (c.r - mn) * k),
                   .g = half8_if_then_else(flat, zero, (c.g - mn) * k),
                   .b = half8_if_then_else(flat, zero, (c.b - mn) * k) };
}

static rgb8 blend_nonsep8(enum canvas_composite_op mode, rgb8 cb, rgb8 cs) {
    switch ((int)mode) {
        case CANVAS_OP_HUE:        return set_lum8(set_saturation8(cs, saturation8(cb)), lum8(cb));
        case CANVAS_OP_SATURATION: return set_lum8(set_saturation8(cb, saturation8(cs)), lum8(cb));
        case CANVAS_OP_COLOR:      return set_lum8(cs, lum8(cb));
        default:                   return set_lum8(cb, lum8(cs));  // luminosity
    }
}

// `s` is one planar slab of premultiplied source pixels, already
// clip-attenuated.  The composite fold runs the same expression over every
// plane: in the Porter-Duff arm the
// alpha plane of Fa*s + Fb*d is Fa*sa + Fb*da = ao, and in the blend arm the
// alpha plane of s*(1-da) + d*(1-sa) + T is sa + da*(1-sa) = ao because T's
// alpha plane is pinned to sa*da.
//
// `lin` is the canvas's linear working space.  When false the kernel is exactly
// today's code, byte for byte (the sRGB bypass).  When true: a range-preserving
// mode runs the same math, finished with the no-upper-clamp; a spec-in-sRGB mode
// is wrapped -- encode the linear s and d to sRGB, blend in sRGB (a recursive
// lin == false call, so clip_color / HSL / the dodge-burn divides run in the
// encoded space they are defined in), then decode the result back to linear.
// For in-gamut colour the encode lands operands in [0,1], the precondition the
// HSL/divide math wants.  An EXTENDED (out-of-[0,1]) operand -- an out-of-gamut
// fill colour, or a destination the no-upper-clamp linear-direct path pushed
// past 1 -- encodes to sRGB > 1 and feeds these modes values outside their
// defined domain, exactly as 'lighter' accumulation can on an sRGB canvas
// today; the final clamp bounds the stored pixel either way.  Wrapping in the
// encoded space is the spec's own frame for these modes; a fully gamut-correct
// extended treatment would be its own design.
static cnvs_px8 blend8(cnvs_px8 s, cnvs_px8 d, enum canvas_composite_op mode,
                       bool lin) {
    if (lin && !linear_direct(mode)) {
        cnvs_px8 const se = px8_transfer(s, cnvs_linear_to_srgb);
        cnvs_px8 const de = px8_transfer(d, cnvs_linear_to_srgb);
        return px8_transfer(blend8(se, de, mode, false), cnvs_srgb_to_linear);
    }
    if (mode == CANVAS_OP_SOURCE_OVER) {
        // Delegate to the fast path's kernel: the Porter-Duff arm below would
        // spell source-over as fa*s + fb*d with fa = 1, and the contraction
        // shape differs -- fa*s rounds fb*d's product separately where
        // src_over8's s + fb*d fuses it -- so the explicit kernel keeps every
        // source-over bit-identical no matter which loop reached it.
        return src_over8(s, d, lin);
    }
    half8 const zero = (half8)(_Float16)0.0f, one = (half8)(_Float16)1.0f;
    half8 sa = s.a, da = d.a;
    cnvs_px8 co;

    if ((int)mode <= CANVAS_OP_COPY) {
        // Porter-Duff: co = Fa*s + Fb*d, ao = Fa*sa + Fb*da.
        half8 fa, fb;
        switch ((int)mode) {
            case CANVAS_OP_SOURCE_IN:        fa = da;       fb = zero;      break;
            case CANVAS_OP_SOURCE_OUT:       fa = one - da; fb = zero;      break;
            case CANVAS_OP_SOURCE_ATOP:      fa = da;       fb = one - sa;  break;
            case CANVAS_OP_DESTINATION_OVER: fa = one - da; fb = one;       break;
            case CANVAS_OP_DESTINATION_IN:   fa = zero;     fb = sa;        break;
            case CANVAS_OP_DESTINATION_OUT:  fa = zero;     fb = one - sa;  break;
            case CANVAS_OP_DESTINATION_ATOP: fa = one - da; fb = sa;        break;
            case CANVAS_OP_XOR:              fa = one - da; fb = one - sa;  break;
            case CANVAS_OP_LIGHTER:          fa = one;      fb = one;       break;
            case CANVAS_OP_COPY:             fa = one;      fb = zero;      break;
            default:                         fa = one;      fb = one - sa;  break;  // source-over
        }
        co.r = fa * s.r + fb * d.r;
        co.g = fa * s.g + fb * d.g;
        co.b = fa * s.b + fb * d.b;
        co.a = fa * sa  + fb * da ;
    } else {
        cnvs_px8 t;
        if ((int)mode >= CANVAS_OP_HUE) {
            short8 sm = sa > zero, dm = da > zero;  // a == 0 un-premultiplies to 0
            rgb8 cs = { half8_if_then_else(sm, s.r / sa, zero),
                        half8_if_then_else(sm, s.g / sa, zero),
                        half8_if_then_else(sm, s.b / sa, zero) };
            rgb8 cb = { half8_if_then_else(dm, d.r / da, zero),
                        half8_if_then_else(dm, d.g / da, zero),
                        half8_if_then_else(dm, d.b / da, zero) };
            rgb8 const bl = blend_nonsep8(mode, cb, cs);
            t.r = sa * da * bl.r;
            t.g = sa * da * bl.g;
            t.b = sa * da * bl.b;
            t.a = sa * da       ;
        } else {
            t.r = blend_term8(mode, s.r, d.r, sa, da);
            t.g = blend_term8(mode, s.g, d.g, sa, da);
            t.b = blend_term8(mode, s.b, d.b, sa, da);
            t.a = sa * da;
        }
        co.r = s.r * (one - da) + d.r * (one - sa) + t.r;
        co.g = s.g * (one - da) + d.g * (one - sa) + t.g;
        co.b = s.b * (one - da) + d.b * (one - sa) + t.b;
        co.a = sa  * (one - da) + da  * (one - sa) + t.a;
    }
    // The linear-direct modes here are multiply/screen/difference/exclusion and
    // the Porter-Duff family (copy, the in/out/atop set, xor) -- range-preserving,
    // so the no-upper clamp lets an extended linear colour survive to the output.
    // additive 'lighter' is NOT linear-direct (it took the encode wrapper above).
    return lin ? cnvs_px8_clamp_premul_lin(co)
               : cnvs_px8_clamp_premul(co);  // additive 'lighter' can exceed 1
}

// The coverage lerp: out = blend*cov + dst*(1-cov) per plane.  Two products,
// not dst + (blend-dst)*cov, so cov == 1 returns the blend bit-exactly and
// cov == 0 returns dst bit-exactly (full coverage must not perturb the blend;
// zero coverage must not perturb the destination).  The clamp restores the
// premultiplied invariant against the one-ULP drift of cov + (1-cov) in f16.
static cnvs_px8 cov_lerp8(cnvs_px8 d, cnvs_px8 co, half8 cov, bool lin) {
    half8 const icov = (half8)(_Float16)1.0f - cov;
    cnvs_px8 o = { co.r * cov + d.r * icov, co.g * cov + d.g * icov,
                   co.b * cov + d.b * icov, co.a * cov + d.a * icov };
    // Coverage is a geometric fraction (antialiasing), so the lerp between two
    // working-space premul colours is range-preserving: the linear no-upper
    // clamp on a linear canvas, the legacy [0,ao] clamp on an sRGB one.
    return lin ? cnvs_px8_clamp_premul_lin(o) : cnvs_px8_clamp_premul(o);
}

// The generic modes, shared by the tile and solid-colour entry points: the
// same planar slab walk as the source-over fast path, with the 26-mode
// kernel in place of the source-over fold.  `tile` may be NULL, in which case
// every slab's source is `splat` -- one solid colour broadcast across the
// lanes.  Effective coverage is the op plane x the clip mask (each absent
// factor is 1); the over-family folds it into the source exactly as the fast
// path does, every other mode blends at full strength and lerps toward the
// destination (cov_lerp8).
static void blend_region(struct canvas *__single cv, int x, int y, int w, int h,
                         cnvs_premul const *__counted_by_or_null(w * h) tile,
                         cnvs_px8 splat,
                         uint8_t const *__counted_by_or_null(w * h) cov,
                         uint8_t const *__counted_by_or_null(clip_len) clip,
                         int clip_len, enum canvas_composite_op mode) {
    (void)clip_len;
    bool const folds = coverage_folds(mode);
    bool const atten = cov || clip;  // any coverage to apply?
    bool const lin = cv->space == CANVAS_WS_LINEAR;  // false -> legacy bypass
    _Float16 const inv255 = (_Float16)(1.0f / 255.0f);
    half8 const one = (half8)(_Float16)1.0f;
    for (int row = 0; row < h; row++) {
        int col = 0;
        for (; col + 8 <= w; col += 8) {
            int ti = row * w + col;
            int di = (y + row) * cv->width + (x + col);
            cnvs_px8 s = tile ? cnvs_px8_load(tile + ti) : splat;
            cnvs_px8 d = cnvs_px8_load(cv->target + di);
            cnvs_px8 o;
            if (!atten) {
                o = blend8(s, d, mode, lin);
            } else if (folds) {
                // Attenuate the source by each factor in turn, exactly as the
                // fast path does (combining the factors first re-rounds).
                if (cov) {
                    s = cnvs_px8_scale(s, half8_from_u8(cov + ti) * inv255);
                }
                if (clip) {
                    s = cnvs_px8_scale(s, half8_from_u8(clip + di) * inv255);
                }
                o = blend8(s, d, mode, lin);
            } else {
                half8 cv8 = one;  // 1*x is exact: a lone factor passes through
                if (cov) {
                    cv8 = cv8 * (half8_from_u8(cov + ti) * inv255);
                }
                if (clip) {
                    cv8 = cv8 * (half8_from_u8(clip + di) * inv255);
                }
                o = cov_lerp8(d, blend8(s, d, mode, lin), cv8, lin);
            }
            cnvs_px8_store(cv->target + di, o);
        }
        if (col < w) {
            int const n = w - col;
            int const ti = row * w + col;
            int const di = (y + row) * cv->width + (x + col);
            cnvs_px8 s = tile ? cnvs_px8_load_k(tile + ti, n) : splat;
            cnvs_px8 const d = cnvs_px8_load_k(cv->target + di, n);
            cnvs_px8 o;
            if (!atten) {
                o = blend8(s, d, mode, lin);
            } else if (folds) {
                if (cov) {
                    s = cnvs_px8_scale(s, half8_from_u8_k(cov + ti, n) * inv255);
                }
                if (clip) {
                    s = cnvs_px8_scale(s, half8_from_u8_k(clip + di, n) * inv255);
                }
                o = blend8(s, d, mode, lin);
            } else {
                half8 cv8 = one;
                if (cov) {
                    cv8 = cv8 * (half8_from_u8_k(cov + ti, n) * inv255);
                }
                if (clip) {
                    cv8 = cv8 * (half8_from_u8_k(clip + di, n) * inv255);
                }
                o = cov_lerp8(d, blend8(s, d, mode, lin), cv8, lin);
            }
            cnvs_px8_store_k(cv->target + di, n, o);
        }
    }
}

void cnvs_blend(struct canvas *__single cv, int x, int y, int w, int h,
                cnvs_premul const *__counted_by(w * h) tile,
                uint8_t const *__counted_by_or_null(w * h) cov,
                uint8_t const *__counted_by_or_null(clip_len) clip, int clip_len,
                enum canvas_composite_op mode) {
    if (!tile || w <= 0 || h <= 0) {
        return;
    }
    if (x < 0 || y < 0 || x + w > cv->width || y + h > cv->height) {
        return;
    }
    if (clip && clip_len < cv->width * cv->height) {
        return;
    }
    if (mode == CANVAS_OP_SOURCE_OVER) {
        // The fast path.  Source-over folds: op coverage (normally already
        // folded by the shade stage, so cov is NULL here) and clip
        // attenuation both scale the premultiplied source, in f16.  Source-over
        // is range-preserving, so `lin` only swaps src_over8's final clamp; an
        // sRGB canvas (lin == false) runs the identical legacy code.
        bool const lin = cv->space == CANVAS_WS_LINEAR;
        _Float16 const inv255 = (_Float16)(1.0f / 255.0f);
        for (int row = 0; row < h; row++) {
            int col = 0;
            for (; col + 8 <= w; col += 8) {
                int ti = row * w + col;
                int di = (y + row) * cv->width + (x + col);
                cnvs_px8 s = cnvs_px8_load(tile + ti);
                if (cov) {
                    s = cnvs_px8_scale(s, half8_from_u8(cov + ti) * inv255);
                }
                if (clip) {
                    s = cnvs_px8_scale(s, half8_from_u8(clip + di) * inv255);
                }
                cnvs_px8 d = cnvs_px8_load(cv->target + di);
                cnvs_px8_store(cv->target + di, src_over8(s, d, lin));
            }
            if (col < w) {
                int const k = w - col;
                int const ti = row * w + col;
                int const di = (y + row) * cv->width + (x + col);
                cnvs_px8 s = cnvs_px8_load_k(tile + ti, k);
                if (cov) {
                    s = cnvs_px8_scale(s, half8_from_u8_k(cov + ti, k) * inv255);
                }
                if (clip) {
                    s = cnvs_px8_scale(s, half8_from_u8_k(clip + di, k) * inv255);
                }
                cnvs_px8 const d = cnvs_px8_load_k(cv->target + di, k);
                cnvs_px8_store_k(cv->target + di, k, src_over8(s, d, lin));
            }
        }
        return;
    }
    // The generic modes: the shared region walk (splat unused, tile present).
    cnvs_px8 const zero = { (half8)(_Float16)0.0f, (half8)(_Float16)0.0f,
                            (half8)(_Float16)0.0f, (half8)(_Float16)0.0f };
    blend_region(cv, x, y, w, h, tile, zero, cov, clip, clip_len, mode);
}

void cnvs_blend_solid(struct canvas *__single cv, int x, int y, int w, int h,
                      cnvs_premul color,
                      uint8_t const *__counted_by_or_null(w * h) cov,
                      uint8_t const *__counted_by_or_null(clip_len) clip, int clip_len,
                      enum canvas_composite_op mode) {
    if (w <= 0 || h <= 0) {
        return;
    }
    if (x < 0 || y < 0 || x + w > cv->width || y + h > cv->height) {
        return;
    }
    if (clip && clip_len < cv->width * cv->height) {
        return;
    }
    // Broadcast the colour across the lanes once: a splat lane equals a
    // stored-and-reloaded f16 exactly, so the region walk's output is
    // byte-identical to materializing the constant tile.  SOURCE_OVER takes
    // the region walk here, not the fast path, and still lands the same
    // bytes: blend8 delegates source-over to src_over8, and the fold arm
    // applies cov/clip as the same two successive scales.
    cnvs_px8 const splat = { (half8)color.r, (half8)color.g,
                             (half8)color.b, (half8)color.a };
    blend_region(cv, x, y, w, h, NULL, splat, cov, clip, clip_len, mode);
}

void cnvs_blend_read(struct canvas *__single cv,
                     cnvs_premul *__counted_by(pixels) out, int pixels) {
    if (!out || pixels < cv->target_len) {
        return;
    }
    memcpy(out, cv->target, (size_t)cv->target_len * sizeof *out);
}

// --- the planar shade stage --------------------------------------------------
//
// The coverage -> premultiplied-tile fold, eight pixels per step over channel
// planes (cnvs_planar.h).  The fold runs in _Float16, the pipeline's compute
// type (docs/decisions/color-axis.md): coverage normalizes as one f16
// multiply by RN16(1/255) -- the blend stage's exact idiom, and 255 * that
// rounds back to exactly 1.0, so full coverage passes the paint's alpha
// through untouched -- and each paint's alpha factors fold in the type the
// colour data is born in (f16 for solid and gradient paint, f32 for image
// and pattern samples), with one narrowing convert.

// Eight coverage bytes as an f16 plane in [0, 1]: exact widen (every u8 value
// is exact in _Float16), one multiply by RN16(1/255).
static half8 cover8(uint8_t const *__counted_by(8) cov) {
    return half8_from_u8(cov) * (_Float16)(1.0f / 255.0f);
}

static half8 cover8_k(uint8_t const *__counted_by(k) cov, int k) {
    return half8_from_u8_k(cov, k) * (_Float16)(1.0f / 255.0f);
}

// Premultiply the colour planes under the folded alpha plane.
static cnvs_px8 shade8(half8 r, half8 g, half8 b, half8 alpha) {
    return cnvs_px8_premultiply((cnvs_px8){ r, g, b, alpha });
}

// cnvs_mat_apply for eight pixel centres along one row: only x varies and the
// affine map is elementwise, so the scalar expression runs per lane bit for
// bit.
typedef struct {
    float8 x, y;
} foldv8;

static foldv8 mat_apply8(cnvs_mat m, float8 x, float y) {
    return (foldv8){ .x = m.a * x + m.c * y + m.e,
                     .y = m.b * x + m.d * y + m.f };
}

// Does the shade stage fold the op's coverage into the tile's alpha?  The
// over-family folds exactly (coverage_folds); for every other
// mode the tile carries the source at full strength and the
// coverage plane rides to cnvs_blend separately, which lerps.  Filters
// force the fold regardless: blur()/drop-shadow() consume the op's silhouette
// from the tile's alpha, so coverage must be materialized before they run --
// after a filter the coverage genuinely is source alpha.
static bool shade_folds_coverage(struct canvas const *__single cv) {
    return coverage_folds(cv->cur.composite) ||
           cv->cur.filter_count > 0;
}

// Finish a freshly shaded tile: run the state's filter list over it, cast the
// shadow from the result's alpha, then composite -- the shared tail of every
// tile-building paint loop.  `fold` is the caller's shade_folds_coverage
// answer: a folded tile already carries the op's coverage in its alpha;
// otherwise cv->cov rides to the blend's lerp (and joins the shadow's alpha).
// The order is the spec's drawing model: filter the drawing, render the
// shadow from the filtered result, composite shadow then drawing.
static void blend_tile(struct canvas *__single cv, cbbox b, bool fold) {
    apply_filters(cv, b.w, b.h);
    emit_shadow(cv, b, true, !fold, (_Float16)1.0f);
    cnvs_blend(cv, b.x, b.y, b.w, b.h, cv->tile, fold ? NULL : cv->cov,
               cv->cur.clip_mask, cv->cur.clip_len, cv->cur.composite);
}

// Paint the resolved coverage in cv->cov with a solid colour.  Each pixel's
// alpha is paint_alpha * global_alpha * coverage when the composite mode folds
// coverage (shade_folds_coverage); otherwise paint_alpha * global_alpha, with
// cv->cov handed to the blend's lerp.
static void paint_tile_solid(struct canvas *__single cv, cbbox b, cnvs_unpremul solid) {
    float const ga = cv->cur.global_alpha;
    bool const fold = shade_folds_coverage(cv);
    // The colour planes are splats and every alpha factor but coverage is one
    // constant, so the loop is a coverage widen, two multiplies, and the
    // premultiply -> st4.  Coverage and tile are both dense over the bbox, so
    // one flat loop covers all rows.
    half8 const base = (half8)(_Float16)((float)solid.a * ga);
    half8 const cr = (half8)solid.r, cg = (half8)solid.g,
                cb = (half8)solid.b;
    int const npix = b.w * b.h;
    if (!fold) {
        // Full-strength source: every pixel is the same premultiplied
        // colour (at full coverage the folded form's base * 1.0 is base
        // exactly, so interiors agree bit for bit).  No tile at all --
        // the blend takes the colour as a splat and the op's
        // coverage plane drives its lerp.  (!fold implies no filters:
        // shade_folds_coverage forces the fold whenever filters are
        // active, so skipping apply_filters here drops only a no-op.)
        // The shadow's alpha is the splat's alpha under the coverage --
        // the same product the folded tile would carry.
        emit_shadow(cv, b, false, true, (_Float16)((float)solid.a * ga));
        cnvs_premul px;
        cnvs_px8_store_k(&px, 1, shade8(cr, cg, cb, base));
        cnvs_blend_solid(cv, b.x, b.y, b.w, b.h, px, cv->cov,
                         cv->cur.clip_mask, cv->cur.clip_len,
                         cv->cur.composite);
        return;
    }
    int i = 0;
    for (; i + 8 <= npix; i += 8) {
        cnvs_px8_store(cv->tile + i,
                       shade8(cr, cg, cb, base * cover8(cv->cov + i)));
    }
    if (i < npix) {
        int const k = npix - i;
        cnvs_px8_store_k(cv->tile + i, k,
                         shade8(cr, cg, cb,
                                base * cover8_k(cv->cov + i, k)));
    }
    blend_tile(cv, b, fold);
}

// Paint the resolved coverage with a gradient; the same alpha fold as
// paint_tile_solid, with the colour solved per pixel instead of splatted.
static void paint_tile_gradient(struct canvas *__single cv, cbbox b,
                                struct cnvs_gradient const *gr) {
    float const ga = cv->cur.global_alpha;
    bool const fold = shade_folds_coverage(cv);
    if (ensure_grad_rows(cv, b.w)) {
        // Evaluate the gradient a row at a time, all three stages vectorized:
        // solve the parameters (cnvs_gradient_param_row), lerp the stop
        // colours from them (cnvs_gradient_color_row) -- the exact
        // piecewise-linear colour, no precomputed ramp
        // (docs/decisions/gradient-eval.md) -- then pick the colours back up
        // as planes (ld4 over the row buffer) for the fold.
        half8 const gah = (half8)(_Float16)ga;
        for (int py = 0; py < b.h; py++) {
            cnvs_gradient_param_row(gr, b.x, (float)b.y + (float)py + 0.5f, b.w,
                                    cv->trow);
            cnvs_gradient_color_row(gr, cv->trow, b.w, cv->crow);
            int const row = py * b.w;
            int px = 0;
            for (; px + 8 <= b.w; px += 8) {
                cnvs_px8 const col = cnvs_px8_load_unpremul(cv->crow + px);
                half8 alpha = col.a * gah;
                if (fold) {
                    alpha = alpha * cover8(cv->cov + row + px);
                }
                cnvs_px8_store(cv->tile + row + px,
                               shade8(col.r, col.g, col.b, alpha));
            }
            if (px < b.w) {
                int const k = b.w - px;
                cnvs_px8 const col = cnvs_px8_load_unpremul_k(cv->crow + px, k);
                half8 alpha = col.a * gah;
                if (fold) {
                    alpha = alpha * cover8_k(cv->cov + row + px, k);
                }
                cnvs_px8_store_k(cv->tile + row + px, k,
                                 shade8(col.r, col.g, col.b, alpha));
            }
        }
    } else {
        // OOM fallback: the row buffers couldn't grow, so run the scalar
        // per-pixel parameter solve + stop lerp.
        for (int py = 0; py < b.h; py++) {
            for (int px = 0; px < b.w; px++) {
                int const i = py * b.w + px;
                float t;
                cnvs_vec2 p = { .x = (float)b.x + (float)px + 0.5f,
                                .y = (float)b.y + (float)py + 0.5f };
                cnvs_unpremul col = cnvs_gradient_param(gr, p, &t)
                                        ? cnvs_gradient_color_at(gr, t)
                                        : cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 0.0f);
                // Fold global alpha (and, for folding modes, coverage) into
                // the paint's alpha, then premultiply -- the row kernel's f16
                // fold, one pixel at a time.
                _Float16 alpha = col.a * (_Float16)ga;
                if (fold) {
                    alpha = alpha * ((_Float16)cv->cov[i] * (_Float16)(1.0f / 255.0f));
                }
                cv->tile[i] = cnvs_premultiply((cnvs_unpremul){
                    .r = col.r, .g = col.g, .b = col.b, .a = alpha });
            }
        }
    }
    blend_tile(cv, b, fold);
}

// Map a sample index onto an axis of length n: wrap mod n for a repeating axis
// (handling negatives), else clamp to [0, n-1].
static int wrap_idx(int i, int n, bool repeat) {
    if (repeat) {
        i %= n;
        return i < 0 ? i + n : i;
    }
    return i < 0 ? 0 : (i > n - 1 ? n - 1 : i);
}

// Sample a pattern at pattern-image coordinate (u, v), unpremultiplied into out.
// A point outside the image on a non-repeating axis is transparent; otherwise the
// per-tap indices wrap (repeat) or clamp (no-repeat).  Bilinear when `smooth`.
static void pattern_sample(struct cnvs_pattern const *p, float u, float v, bool smooth,
                           float *__counted_by(4) out) {
    bool const rx = p->repeat == CANVAS_REPEAT || p->repeat == CANVAS_REPEAT_X;
    bool const ry = p->repeat == CANVAS_REPEAT || p->repeat == CANVAS_REPEAT_Y;
    if ((!rx && (u < 0.0f || u >= (float)p->w)) ||
        (!ry && (v < 0.0f || v >= (float)p->h))) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    int w = p->w, h = p->h;
    if (smooth) {
        float gu = u - 0.5f, gv = v - 0.5f;
        float fu = floorf(gu), fv = floorf(gv);
        float tu = gu - fu, tv = gv - fv;
        int u0 = wrap_idx(cnvs_f2i(fu), w, rx), u1 = wrap_idx(cnvs_f2i(fu) + 1, w, rx);
        int v0 = wrap_idx(cnvs_f2i(fv), h, ry), v1 = wrap_idx(cnvs_f2i(fv) + 1, h, ry);
        for (int k = 0; k < 4; k++) {
            float const c00 = (float)p->data[(v0 * w + u0) * 4 + k];
            float const c10 = (float)p->data[(v0 * w + u1) * 4 + k];
            float const c01 = (float)p->data[(v1 * w + u0) * 4 + k];
            float const c11 = (float)p->data[(v1 * w + u1) * 4 + k];
            float const top = c00 + (c10 - c00) * tu;
            float const bot = c01 + (c11 - c01) * tu;
            out[k] = (top + (bot - top) * tv) / 255.0f;
        }
    } else {
        int const iu = wrap_idx(cnvs_f2i(floorf(u)), w, rx);
        int const iv = wrap_idx(cnvs_f2i(floorf(v)), h, ry);
        int const o = (iv * w + iu) * 4;
        for (int k = 0; k < 4; k++) {
            out[k] = (float)p->data[o + k] / 255.0f;
        }
    }
}

// Paint the resolved coverage (cv->cov over b) with an image pattern: each device
// pixel maps through the pattern's device->image transform, samples (bilinear or
// nearest per image smoothing), and folds in global alpha and coverage.
//
// Blocks of eight pixels: the SAMPLING stays scalar per lane -- each sample is
// data-dependent addressing (up to four taps at arbitrary source coords), with
// no batch shape -- but everything around it is planar.  A zero-coverage lane
// skips its sample and stays transparent black (the all-zero lanes fold to the
// same {0,0,0,0} bits the skip leaves).  The samples land in f32 planes (they
// are born f32 at the taps); sample alpha x global alpha folds there, narrows
// once, and the coverage fold finishes in f16 like every shade path.
static void paint_tile_pattern(struct canvas *__single cv, cbbox b, struct cnvs_pattern const *p) {
    float const ga = cv->cur.global_alpha;
    bool const fold = shade_folds_coverage(cv);
    bool const smooth = cv->cur.image_smoothing_enabled;
    float8 const lane = { 0, 1, 2, 3, 4, 5, 6, 7 };
    for (int py = 0; py < b.h; py++) {
        float const dy = (float)b.y + (float)py + 0.5f;
        for (int px = 0; px < b.w; px += 8) {
            int const i = py * b.w + px;
            int const k = b.w - px < 8 ? b.w - px : 8;
            // Pixel-centre x per lane: integer-exact f32 sums, so the grouping
            // can't differ from the scalar (float)b.x + (float)(px+l) + 0.5f.
            float8 const xs = (float)b.x + ((float)px + lane) + 0.5f;
            foldv8 const uv = mat_apply8(p->to_pattern, xs, dy);
            float8 sr = (float8)0.0f, sg = (float8)0.0f, sb = (float8)0.0f,
                   sa = (float8)0.0f;
            for (int l = 0; l < k; l++) {
                if (cv->cov[i + l] == 0) {  // a zero-coverage lane skips its taps
                    continue;
                }
                float s[4];
                pattern_sample(p, uv.x[l], uv.y[l], smooth, s);
                sr[l] = s[0];
                sg[l] = s[1];
                sb[l] = s[2];
                sa[l] = s[3];
            }
            half8 alpha = __builtin_convertvector(sa * ga, half8);
            if (fold) {
                alpha = alpha * (k < 8 ? cover8_k(cv->cov + i, k)
                                       : cover8(cv->cov + i));
            }
            cnvs_px8 const out = shade8(__builtin_convertvector(sr, half8),
                                        __builtin_convertvector(sg, half8),
                                        __builtin_convertvector(sb, half8),
                                        alpha);
            if (k < 8) {
                cnvs_px8_store_k(cv->tile + i, k, out);
            } else {
                cnvs_px8_store(cv->tile + i, out);
            }
        }
    }
    blend_tile(cv, b, fold);
}

// Grow the two shadow ping-pong masks to at least n bytes each.
static bool ensure_shadow(struct canvas *__single cv, int n) {
    if (n > cv->shadow_src_cap) {
        uint8_t *na = realloc(cv->shadow_src, (size_t)n);
        if (!na) {
            return false;
        }
        cv->shadow_src = na;
        cv->shadow_src_cap = n;
    }
    if (n > cv->shadow_dst_cap) {
        uint8_t *nb = realloc(cv->shadow_dst, (size_t)n);
        if (!nb) {
            return false;
        }
        cv->shadow_dst = nb;
        cv->shadow_dst_cap = n;
    }
    return true;
}

// A shadow is cast when its colour is non-transparent and there is some blur or
// offset (a zero-blur, zero-offset shadow would sit exactly under the shape).
static bool shadow_active(struct canvas const *__single cv) {
    return (float)cv->cur.shadow_color.a > 0.0f &&
           (cv->cur.shadow_blur > 0.0f || cv->cur.shadow_offset_x != 0.0f ||
            cv->cur.shadow_offset_y != 0.0f);
}

// Box radius approximating a Gaussian of standard deviation `sigma` (device
// px): three box passes of radius r have variance r^2 + r, so r ~= sigma.
// Any positive sigma blurs at least radius 1; clamped above so a huge blur
// stays bounded; <= 0 means no blur.  Shared by the two Gaussian-ish blurs,
// whose parameters map differently: shadowBlur is *twice* the stdDev
// (shadow_radius), while filter blur(px) *is* the stdDev
// (canvas_add_filter_blur).
static int sigma_box_radius(float sigma) {
    if (!(sigma > 0.0f)) {
        return 0;
    }
    int const r = (int)(sigma + 0.5f);
    return r < 1 ? 1 : (r > 1024 ? 1024 : r);
}

// The spec's shadow Gaussian has stdDev = shadowBlur / 2.
static int shadow_radius(float blur) {
    return sigma_box_radius(blur * 0.5f);
}

// Split a shadow offset into its whole-pixel floor and the [0, 256) numerator
// of its 1/256th-pixel remainder -- the subpixel grid the 2-tap shift passes
// lerp on (blur_shift_h/v); finer than 1/256 px is invisible.  Clamped to a
// sane range first (a larger offset just pushes the shadow off-canvas).  The
// fraction is always shifted rightward/downward of the floor, so a negative
// offset splits the same way: -2.5 is whole -3 plus 128/256.
static void shadow_offset_split(float v, int *__single whole, int *__single k256) {
    float const m = (float)(2 * CANVAS_DIM_MAX);
    v = v > m ? m : (v < -m ? -m : v);
    int const t = cnvs_f2i(floorf(v * 256.0f + 0.5f));  // round to the 1/256 grid
    int q = t / 256, r = t - q * 256;
    if (r < 0) { q -= 1; r += 256; }  // floor division: remainder in [0, 256)
    *whole = q;
    *k256 = r;
}

// Cast the current op's shadow from its source alpha over bbox b: build a
// single-channel mask of the op's alpha -- the spec's "render the shadow from
// image A", so paint alpha, coverage, global alpha, and any filters all shape
// it -- blur it (~Gaussian), and composite the shadow colour through the
// blurred mask, offset, under the shape (which the caller blends next).  The
// alpha source is per fold strategy: a folded tile already carries the op's
// full alpha in its alpha plane (from_tile, !with_cov); an unfolded tile
// carries paint x ga at full strength with coverage riding separately
// (from_tile, with_cov); and the tile-less solid splat path is coverage
// scaled by the splat's alpha (!from_tile, with_cov, base).  Blur and offset
// are device-space and unaffected by the CTM, per spec.
static void emit_shadow(struct canvas *__single cv, cbbox b,
                        bool from_tile, bool with_cov, _Float16 base) {
    if (!shadow_active(cv) || b.w <= 0 || b.h <= 0) {
        return;
    }
    int const radius = shadow_radius(cv->cur.shadow_blur);
    int offx, kx, offy, ky;
    shadow_offset_split(cv->cur.shadow_offset_x, &offx, &kx);
    shadow_offset_split(cv->cur.shadow_offset_y, &offy, &ky);
    // Three box passes each spread the blur by the radius, so the falloff
    // reaches ~0 only at 3x the radius beyond the shape -- the mask region must
    // include that whole spread or the soft edge gets clipped to a rectangle.
    // A fractional offset spills one more pixel rightward/downward.
    int const margin = 3 * radius + (kx || ky ? 1 : 0);
    int sx0 = b.x       + offx - margin, sy0 = b.y       + offy - margin;
    int sx1 = b.x + b.w + offx + margin, sy1 = b.y + b.h + offy + margin;
    if (sx0 < 0)          { sx0 = 0; }
    if (sy0 < 0)          { sy0 = 0; }
    if (sx1 > cv->width)  { sx1 = cv->width; }
    if (sy1 > cv->height) { sy1 = cv->height; }
    int sw = sx1 - sx0, sh = sy1 - sy0;
    if (sw <= 0 || sh <= 0 || !ensure_shadow(cv, sw * sh)) {
        return;
    }
    int const n = sw * sh;
    memset(cv->shadow_src, 0, (size_t)n);
    // Stamp the op alpha into the mask at its offset position (clipped to the
    // mask, which may be tighter than the offset alpha near the canvas edge).
    // The 255-quantize is the readback idiom (x255 + 0.5, truncating store):
    // every exact 8-bit alpha -- coverage included -- stamps its byte back
    // unchanged, so an opaque solid's mask is its coverage bit for bit.
    _Float16 const inv255 = (_Float16)(1.0f / 255.0f);
    int mx0 = b.x + offx - sx0, my0 = b.y + offy - sy0;
    for (int cy = 0; cy < b.h; cy++) {
        int const my = my0 + cy;
        if (my < 0 || my >= sh) {
            continue;
        }
        for (int cx = 0; cx < b.w; cx++) {
            int const mx = mx0 + cx;
            if (mx >= 0 && mx < sw) {
                int const i = cy * b.w + cx;
                _Float16 a = base;
                if (from_tile) { a = a * cv->tile[i].a; }
                if (with_cov)  { a = a * ((_Float16)cv->cov[i] * inv255); }
                float const af = (float)a;
                cv->shadow_src[my * sw + mx] =
                    (uint8_t)((af < 0.0f ? 0.0f : af > 1.0f ? 1.0f : af)
                              * 255.0f + 0.5f);
            }
        }
    }
    if (radius > 0) {  // three separable box passes ~ a Gaussian (ping-pong)
        for (int pass = 0; pass < 3; pass++) {
            blur_box_h(cv->shadow_dst, cv->shadow_src, sw, sh, radius);
            blur_box_v(cv->shadow_src, cv->shadow_dst, sw, sh, radius);
        }
    }
    if (kx || ky) {  // the offsets' subpixel fractions: one 2-tap lerp per
                     // axis (blur_shift_h/v), commuting with the box passes
                     // above, so the whole-pixel stamp plus these IS the
                     // fractional translate.  A zero fraction passes through
                     // as an exact copy, keeping the src/dst ping-pong even.
        blur_shift_h(cv->shadow_dst, cv->shadow_src, sw, sh, kx);
        blur_shift_v(cv->shadow_src, cv->shadow_dst, sw, sh, ky);
    }
    // Composite the shadow colour through the blurred mask: one splat, the
    // mask as the blend's coverage plane -- blend_region's fold arm scales
    // the splat by the mask for folding modes, its lerp arm bounds the blend
    // by it for the rest, so one call serves both strategies.  Global alpha
    // is NOT folded here: it already rides in the mask (the op alpha the
    // stamp loop quantized), per the spec's B = shadow(A) x globalAlpha.
    // The tile stays untouched -- it holds the op this shadow lands under.
    cnvs_unpremul const sc = cv->cur.shadow_color;
    cnvs_premul px;
    cnvs_px8_store_k(&px, 1, shade8((half8)sc.r, (half8)sc.g, (half8)sc.b,
                                    (half8)sc.a));
    cnvs_blend_solid(cv, sx0, sy0, sw, sh, px, cv->shadow_src,
                     cv->cur.clip_mask, cv->cur.clip_len,
                     cv->cur.composite);
}

// Paint the resolved coverage with the current fill / stroke paint, dispatching
// on its kind.  Each paint path casts the shadow itself, from the painted
// alpha, just before its blend -- so it lands under the shape.
static void paint_fill(struct canvas *__single cv, cbbox b) {
    if (cv->cur.fill_kind == CNVS_PAINT_GRADIENT &&
        cnvs_gradient_paints_nothing(&cv->cur.fill_grad)) {
        return;
    }
    switch (cv->cur.fill_kind) {
        case CNVS_PAINT_SOLID:    paint_tile_solid(cv, b, cv->cur.fill);            break;
        case CNVS_PAINT_GRADIENT: paint_tile_gradient(cv, b, &cv->cur.fill_grad);   break;
        case CNVS_PAINT_PATTERN:  paint_tile_pattern(cv, b, &cv->cur.fill_pattern); break;
    }
}

static void paint_stroke(struct canvas *__single cv, cbbox b) {
    if (cv->cur.stroke_kind == CNVS_PAINT_GRADIENT &&
        cnvs_gradient_paints_nothing(&cv->cur.stroke_grad)) {
        return;
    }
    switch (cv->cur.stroke_kind) {
        case CNVS_PAINT_SOLID:    paint_tile_solid(cv, b, cv->cur.stroke);            break;
        case CNVS_PAINT_GRADIENT: paint_tile_gradient(cv, b, &cv->cur.stroke_grad);   break;
        case CNVS_PAINT_PATTERN:  paint_tile_pattern(cv, b, &cv->cur.stroke_pattern); break;
    }
}

void canvas_clear_rect(struct canvas *__single cv, float x, float y, float w, float h) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "clear_rect", (float[]){ x, y, w, h }, 4); }
    cnvs_vec2 q[4] = { xf(cv, x, y), xf(cv, x + w, y),
                       xf(cv, x + w, y + h), xf(cv, x, y + h) };
    cbbox b = points_bbox(cv, q, 4, 0);  // clear_rect bypasses filters: no margin
    if (b.w <= 0 || b.h <= 0) {
        return;
    }
    // Erase = destination-out of a unit-alpha solid: out = dst*(1 - alpha), and
    // the clip attenuates alpha to the coverage, so a clip leaves dst*(1 - clip).
    // The unit-alpha source is one splat colour -- no tile (and no allocation).
    cnvs_blend_solid(cv, b.x, b.y, b.w, b.h,
                     (cnvs_premul){ .r = 0, .g = 0, .b = 0,
                                    .a = (_Float16)1.0f },
                     NULL, cv->cur.clip_mask, cv->cur.clip_len,
                     CANVAS_OP_DESTINATION_OUT);
}

void canvas_fill_rect(struct canvas *__single cv, float x, float y, float w, float h) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "fill_rect", (float[]){ x, y, w, h }, 4); }
    cnvs_vec2 q[4] = { xf(cv, x, y), xf(cv, x + w, y),
                       xf(cv, x + w, y + h), xf(cv, x, y + h) };
    cbbox const b = points_bbox(cv, q, 4, filter_margin(cv));
    if (b.w <= 0 || b.h <= 0 || !ensure_tile(cv, b.w * b.h) ||
        !cnvs_cover_reset(&cv->cover, b.w, b.h)) {
        return;
    }
    cover_edge(cv, b, q[0], q[1]);
    cover_edge(cv, b, q[1], q[2]);
    cover_edge(cv, b, q[2], q[3]);
    cover_edge(cv, b, q[3], q[0]);
    cnvs_cover_resolve(&cv->cover, b.w, b.h, CNVS_NONZERO, cv->cov);
    paint_fill(cv, b);
}

void canvas_begin_path(struct canvas *__single cv) {
    if (cv->rec) { cnvs_rec_op(cv->rec, "begin_path"); }
    cnvs_path_reset(&cv->path);
}

void canvas_move_to(struct canvas *__single cv, float x, float y) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "move_to", (float[]){ x, y }, 2); }
    cnvs_path_move_to(&cv->path, xf(cv, x, y));
    cv->cur_user = (cnvs_vec2){ .x = x, .y = y };
}

void canvas_line_to(struct canvas *__single cv, float x, float y) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "line_to", (float[]){ x, y }, 2); }
    cnvs_path_line_to(&cv->path, xf(cv, x, y));
    cv->cur_user = (cnvs_vec2){ .x = x, .y = y };
}

void canvas_rect(struct canvas *__single cv, float x, float y, float w, float h) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "rect", (float[]){ x, y, w, h }, 4); }
    cnvs_path_rect(&cv->path, xf(cv, x, y), xf(cv, x + w, y),
                   xf(cv, x + w, y + h), xf(cv, x, y + h));
    cv->cur_user = (cnvs_vec2){ .x = x, .y = y };
}

void canvas_quadratic_curve_to(struct canvas *__single cv,
                               float cpx, float cpy, float x, float y) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "quadratic_curve_to", (float[]){ cpx, cpy, x, y }, 4); }
    cnvs_path_quad_to(&cv->path, xf(cv, cpx, cpy), xf(cv, x, y),
                      CANVAS_FLATTEN_TOL);
    cv->cur_user = (cnvs_vec2){ .x = x, .y = y };
}

void canvas_bezier_curve_to(struct canvas *__single cv, float c1x, float c1y,
                            float c2x, float c2y, float x, float y) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "bezier_curve_to", (float[]){ c1x, c1y, c2x, c2y, x, y }, 6); }
    cnvs_path_cubic_to(&cv->path, xf(cv, c1x, c1y), xf(cv, c2x, c2y),
                       xf(cv, x, y), CANVAS_FLATTEN_TOL);
    cv->cur_user = (cnvs_vec2){ .x = x, .y = y };
}

void canvas_ellipse(struct canvas *__single cv, float x, float y, float rx, float ry,
                    float rotation, float start_angle, float end_angle,
                    bool anticlockwise) {
    if (cv->rec) {
        cnvs_rec_floats_bool(cv->rec, "ellipse",
                             (float[]){ x, y, rx, ry, rotation, start_angle, end_angle },
                             7, anticlockwise);
    }
    float const two_pi = 2.0f * (float)M_PI;
    float sweep = end_angle - start_angle;
    if (!isfinite(sweep)) {
        return;  // non-finite angle: no arc (Canvas spec ignores non-finite args)
    }
    // Bring `sweep` to the sign the winding direction needs.  A repeated +/-2pi
    // loop never terminates for huge magnitudes (the step falls below the float
    // ULP, and +/-inf never crosses zero), so fold it in one step.
    if (!anticlockwise && sweep < 0.0f) {
        sweep -= two_pi * floorf(sweep / two_pi);   // -> [0, 2pi)
    } else if (anticlockwise && sweep > 0.0f) {
        sweep -= two_pi * ceilf(sweep / two_pi);     // -> (-2pi, 0]
    }
    float const arx = rx < 0.0f ? -rx : rx;
    float const ary = ry < 0.0f ? -ry : ry;
    float const rmax = arx > ary ? arx : ary;
    float const rr = rmax > CANVAS_FLATTEN_TOL ? rmax : CANVAS_FLATTEN_TOL;
    float dstep = 2.0f * acosf(fmaxf(-1.0f, 1.0f - CANVAS_FLATTEN_TOL / rr));
    if (!(dstep > 1e-4f)) {
        dstep = 1e-4f;  // guard against tiny/NaN step
    }
    float const fsegs = ceilf(fabsf(sweep) / dstep);
    int segs = cnvs_f2i(fsegs);
    if (segs < 2) {
        segs = 2;
    }
    if (segs > 4096) {
        segs = 4096;
    }
    float const cosr = cosf(rotation);
    float const sinr = sinf(rotation);
    for (int i = 0; i <= segs; i++) {
        float const t = start_angle + sweep * ((float)i / (float)segs);
        float const ex = rx * cosf(t);
        float const ey = ry * sinf(t);
        cnvs_vec2 const p = xf(cv, x + ex * cosr - ey * sinr, y + ex * sinr + ey * cosr);
        if (i == 0 && !cv->path.has_cur) {
            cnvs_path_move_to(&cv->path, p);
        } else {
            cnvs_path_line_to(&cv->path, p);
        }
    }
    float const te = start_angle + sweep;
    cv->cur_user = (cnvs_vec2){
        .x = x + rx * cosf(te) * cosr - ry * sinf(te) * sinr,
        .y = y + rx * cosf(te) * sinr + ry * sinf(te) * cosr,
    };
}

void canvas_arc(struct canvas *__single cv, float x, float y, float radius,
                float start_angle, float end_angle, bool anticlockwise) {
    // Record `arc` as itself, then swallow the canvas_ellipse it expands to.
    if (cv->rec) {
        cnvs_rec_floats_bool(cv->rec, "arc",
                             (float[]){ x, y, radius, start_angle, end_angle },
                             5, anticlockwise);
        cnvs_rec_enter(cv->rec);
    }
    canvas_ellipse(cv, x, y, radius, radius, 0.0f, start_angle, end_angle,
                   anticlockwise);
    cnvs_rec_leave(cv->rec);
}

void canvas_round_rect(struct canvas *__single cv, float x, float y, float w, float h,
                       float radius) {
    // Record `round_rect` as itself, then swallow the move_to/arc/close_path it
    // expands to (no early returns between enter and leave).
    if (cv->rec) {
        cnvs_rec_floats(cv->rec, "round_rect", (float[]){ x, y, w, h, radius }, 5);
        cnvs_rec_enter(cv->rec);
    }
    float r = radius < 0.0f ? 0.0f : radius;
    float rmax = (w < h ? w : h) * 0.5f;
    if (rmax < 0.0f) {
        rmax = 0.0f;
    }
    if (r > rmax) {
        r = rmax;
    }
    float const q = (float)M_PI * 0.5f;
    float const pi = (float)M_PI;
    canvas_move_to(cv, x + r, y);
    canvas_arc(cv, x + w - r, y + r,     r,   -q, 0.0f,   false);  // top-right
    canvas_arc(cv, x + w - r, y + h - r, r, 0.0f,    q,   false);  // bottom-right
    canvas_arc(cv, x + r,     y + h - r, r,    q,   pi,   false);  // bottom-left
    canvas_arc(cv, x + r,     y + r,     r,   pi, pi + q, false);  // top-left
    canvas_close_path(cv);
    cnvs_rec_leave(cv->rec);
}

// CSS border-radius overlap rule: reduce the scale factor `f` so that two radii
// summing to `sum` fit within an edge of length `len`.  `sum` 0 (no radii on the
// edge) divides to inf or NaN, which never passes g < f.
static float radii_fit(float f, float len, float sum) {
    float const g = len / sum;
    return g < f ? g : f;
}

static void round_rect_radii_impl(struct canvas *__single cv, float x, float y,
                                  float w, float h,
                                  float tl_x, float tl_y, float tr_x, float tr_y,
                                  float br_x, float br_y, float bl_x, float bl_y) {
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h)) {
        return;  // Canvas spec: non-finite geometry paints nothing.
    }
    // Normalise negative extents so the corners keep CSS order (TL, TR, BR, BL).
    if (w < 0.0f) { x += w; w = -w; }
    if (h < 0.0f) { y += h; h = -h; }
    // r[2i], r[2i+1] are corner i's (x,y) radii, i = TL, TR, BR, BL.  Each clamps
    // to a finite, non-negative value (negative / NaN / inf -> 0), matching the
    // scalar canvas_round_rect's convention and keeping the scaling math finite.
    float r[8] = { tl_x, tl_y, tr_x, tr_y, br_x, br_y, bl_x, bl_y };
    for (int i = 0; i < 8; i++) {
        r[i] = (isfinite(r[i]) && r[i] > 0.0f) ? r[i] : 0.0f;
    }
    // Scale every radius down by the tightest edge constraint (CSS rule).
    float f = 1.0f;
    f = radii_fit(f, w, r[0] + r[2]);  // top:    TL.x + TR.x
    f = radii_fit(f, w, r[6] + r[4]);  // bottom: BL.x + BR.x
    f = radii_fit(f, h, r[1] + r[7]);  // left:   TL.y + BL.y
    f = radii_fit(f, h, r[3] + r[5]);  // right:  TR.y + BR.y
    if (f < 1.0f) {
        for (int i = 0; i < 8; i++) {
            r[i] *= f;
        }
    }
    // Trace the outline clockwise from the top edge, each corner an elliptical
    // arc (a zero-radius corner degenerates to the sharp rectangle corner).
    float const q = (float)M_PI * 0.5f;
    float const pi = (float)M_PI;
    canvas_move_to(cv, x + r[0], y);
    canvas_ellipse(cv, x + w - r[2], y + r[3],     r[2], r[3], 0.0f,   -q, 0.0f,   false);
    canvas_ellipse(cv, x + w - r[4], y + h - r[5], r[4], r[5], 0.0f, 0.0f,    q,   false);
    canvas_ellipse(cv, x + r[6],     y + h - r[7], r[6], r[7], 0.0f,    q,   pi,   false);
    canvas_ellipse(cv, x + r[0],     y + r[1],     r[0], r[1], 0.0f,   pi, pi + q, false);
    canvas_close_path(cv);
}

void canvas_round_rect_radii(struct canvas *__single cv, float x, float y,
                             float w, float h,
                             float tl_x, float tl_y, float tr_x, float tr_y,
                             float br_x, float br_y, float bl_x, float bl_y) {
    // Record as itself, then swallow the move_to/ellipse/close_path the impl
    // expands to (the impl's early return keeps this wrapper single-exit, the
    // arc_to pattern).
    if (cv->rec) {
        cnvs_rec_floats(cv->rec, "round_rect_radii",
                        (float[]){ x, y, w, h, tl_x, tl_y, tr_x, tr_y,
                                   br_x, br_y, bl_x, bl_y }, 12);
        cnvs_rec_enter(cv->rec);
    }
    round_rect_radii_impl(cv, x, y, w, h, tl_x, tl_y, tr_x, tr_y,
                          br_x, br_y, bl_x, bl_y);
    cnvs_rec_leave(cv->rec);
}

static void arc_to_impl(struct canvas *__single cv, float x1, float y1, float x2, float y2,
                        float radius) {
    if (!cv->path.has_cur) {
        canvas_move_to(cv, x1, y1);
        return;
    }
    // Work in user space using the user-space current point.
    float u0x = cv->cur_user.x - x1;
    float u0y = cv->cur_user.y - y1;
    float u2x = x2 - x1;
    float u2y = y2 - y1;
    float const l0 = sqrtf(u0x * u0x + u0y * u0y);
    float const l2 = sqrtf(u2x * u2x + u2y * u2y);
    if (l0 < 1e-6f || l2 < 1e-6f || radius <= 0.0f) {
        canvas_line_to(cv, x1, y1);
        return;
    }
    u0x /= l0;
    u0y /= l0;
    u2x /= l2;
    u2y /= l2;
    float cosang = u0x * u2x + u0y * u2y;
    if (cosang > 1.0f) {
        cosang = 1.0f;
    }
    if (cosang < -1.0f) {
        cosang = -1.0f;
    }
    float const ang = acosf(cosang);
    if (ang < 1e-3f || (float)M_PI - ang < 1e-3f) {
        canvas_line_to(cv, x1, y1);  // collinear: no arc
        return;
    }
    float const td = radius / tanf(ang * 0.5f);   // P1 -> tangent point distance
    float const bx = u0x + u2x;
    float const by = u0y + u2y;
    float const bl = sqrtf(bx * bx + by * by);
    float const cdist = radius / sinf(ang * 0.5f);  // P1 -> arc centre distance
    float const cx = x1 + bx / bl * cdist;
    float const cy = y1 + by / bl * cdist;
    float const t1x = x1 + u0x * td;
    float const t1y = y1 + u0y * td;
    float const t2x = x1 + u2x * td;
    float const t2y = y1 + u2y * td;
    float const sa = atan2f(t1y - cy, t1x - cx);
    float const ea = atan2f(t2y - cy, t2x - cx);
    bool const ccw = (u0x * u2y - u0y * u2x) > 0.0f;
    canvas_line_to(cv, t1x, t1y);
    canvas_arc(cv, cx, cy, radius, sa, ea, ccw);
    cv->cur_user = (cnvs_vec2){ .x = t2x, .y = t2y };
}

void canvas_arc_to(struct canvas *__single cv, float x1, float y1, float x2, float y2,
                   float radius) {
    // Record `arc_to` as itself, then swallow the line_to/arc its impl issues.
    // The wrapper is single-exit, so leave always balances enter even though the
    // impl has several early returns.
    if (cv->rec) {
        cnvs_rec_floats(cv->rec, "arc_to", (float[]){ x1, y1, x2, y2, radius }, 5);
        cnvs_rec_enter(cv->rec);
    }
    arc_to_impl(cv, x1, y1, x2, y2, radius);
    cnvs_rec_leave(cv->rec);
}

void canvas_close_path(struct canvas *__single cv) {
    if (cv->rec) { cnvs_rec_op(cv->rec, "close_path"); }
    cnvs_path_close(&cv->path);
}

// Rasterize a device-space path under `rule` and paint it with the fill paint
// over its clamped bbox.
static void fill_device_path(struct canvas *__single cv, struct cnvs_path const *p,
                             enum cnvs_fill_rule rule) {
    cbbox const b = points_bbox(cv, p->pts, p->npts, filter_margin(cv));
    if (b.w <= 0 || b.h <= 0 || !ensure_tile(cv, b.w * b.h) ||
        !cnvs_cover_reset(&cv->cover, b.w, b.h)) {
        return;
    }
    cover_path_edges(cv, b, p);
    cnvs_cover_resolve(&cv->cover, b.w, b.h, rule, cv->cov);
    paint_fill(cv, b);
}

void canvas_fill(struct canvas *__single cv, enum canvas_fill_rule rule) {
    if (cv->rec) { cnvs_rec_rule(cv->rec, "fill", rule); }
    fill_device_path(cv, &cv->path,
                     rule == CANVAS_EVENODD ? CNVS_EVENODD : CNVS_NONZERO);
}

// Point-in-path for hit testing.  Each subpath is treated as implicitly closed
// (as the fill rasterizer does).  Casts a ray in +x from `q` and counts edge
// crossings: the signed count is the winding number (nonzero rule) and the raw
// count is the crossing number (even-odd rule).  The half-open vertical test
// (a.y <= q.y < b.y for an upward edge, and the reverse for downward) counts each
// shared vertex exactly once.
static bool path_contains(struct cnvs_path const *p, cnvs_vec2 q, enum cnvs_fill_rule rule) {
    int wn = 0;   // winding number  (nonzero rule)
    int cn = 0;   // crossing number (even-odd rule)
    for (int s = 0; s < p->nsubs; s++) {
        cnvs_subpath const sp = p->subs[s];
        if (sp.count < 2) {
            continue;
        }
        for (int k = 0; k < sp.count; k++) {
            cnvs_vec2 const a = p->pts[sp.start + k];
            cnvs_vec2 const b = p->pts[sp.start + (k + 1) % sp.count];
            bool const up = a.y <= q.y && b.y > q.y;
            bool const down = a.y > q.y && b.y <= q.y;
            if (!up && !down) {
                continue;  // edge doesn't straddle the ray's row
            }
            // isLeft > 0 means q is left of the directed edge a->b, i.e. the +x
            // ray from q crosses it.  An upward edge then winds +1, a downward -1.
            float const is_left = (b.x - a.x) * (q.y - a.y) - (q.x - a.x) * (b.y - a.y);
            if (up && is_left > 0.0f) {
                wn += 1;
                cn += 1;
            } else if (down && is_left < 0.0f) {
                wn -= 1;
                cn += 1;
            }
        }
    }
    return rule == CNVS_EVENODD ? (cn & 1) != 0 : wn != 0;
}

bool canvas_is_point_in_path(struct canvas *__single cv, float x, float y,
                             enum canvas_fill_rule rule) {
    if (!isfinite(x) || !isfinite(y)) {
        return false;
    }
    enum cnvs_fill_rule const r = rule == CANVAS_EVENODD ? CNVS_EVENODD : CNVS_NONZERO;
    return path_contains(&cv->path, xf(cv, x, y), r);
}

void canvas_clip(struct canvas *__single cv, enum canvas_fill_rule rule) {
    if (cv->rec) { cnvs_rec_rule(cv->rec, "clip", rule); }
    int const n = cv->width * cv->height;
    uint8_t *nm = malloc((size_t)n);
    if (!nm) {
        return;
    }
    // Rasterize the path's coverage into cv->cov over its (clamped) bbox.
    cbbox b = points_bbox(cv, cv->path.pts, cv->path.npts, 0);  // the clip is unfiltered
    if (b.w > 0 && b.h > 0 && ensure_tile(cv, b.w * b.h) &&
        cnvs_cover_reset(&cv->cover, b.w, b.h)) {
        cover_path_edges(cv, b, &cv->path);
        cnvs_cover_resolve(&cv->cover, b.w, b.h,
                           rule == CANVAS_EVENODD ? CNVS_EVENODD : CNVS_NONZERO,
                           cv->cov);
    } else {
        b.w = 0;
        b.h = 0;  // empty path: clip to nothing
    }
    // new_clip = old_clip * path_coverage, zero outside the path's bbox.
    for (int yy = 0; yy < cv->height; yy++) {
        for (int xx = 0; xx < cv->width; xx++) {
            int const i = yy * cv->width + xx;
            int pc = 0;
            if (xx >= b.x && xx < b.x + b.w && yy >= b.y && yy < b.y + b.h) {
                pc = cv->cov[(yy - b.y) * b.w + (xx - b.x)];
            }
            int const old = cv->cur.clip_mask ? cv->cur.clip_mask[i] : 255;
            nm[i] = (uint8_t)(old * pc / 255);
        }
    }
    free(cv->cur.clip_mask);
    cv->cur.clip_mask = nm;
    cv->cur.clip_len = n;
}

void canvas_set_stroke_rgba(struct canvas *__single cv, float r, float g, float b, float a) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_stroke_rgba", (float[]){ r, g, b, a }, 4); }
    cv->cur.stroke = intern_color(cv, r, g, b, a);
    cv->cur.stroke_kind = CNVS_PAINT_SOLID;
}

void canvas_set_line_width(struct canvas *__single cv, float width) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_line_width", (float[]){ width }, 1); }
    cv->cur.line_width = width;
}

void canvas_set_line_join(struct canvas *__single cv, enum canvas_line_join join) {
    if (cv->rec) { cnvs_rec_line_join(cv->rec, join); }
    switch (join) {
        case CANVAS_JOIN_MITER: cv->cur.line_join = CNVS_JOIN_MITER; break;
        case CANVAS_JOIN_ROUND: cv->cur.line_join = CNVS_JOIN_ROUND; break;
        case CANVAS_JOIN_BEVEL: cv->cur.line_join = CNVS_JOIN_BEVEL; break;
    }
}

void canvas_set_line_cap(struct canvas *__single cv, enum canvas_line_cap cap) {
    if (cv->rec) { cnvs_rec_line_cap(cv->rec, cap); }
    switch (cap) {
        case CANVAS_CAP_BUTT:   cv->cur.line_cap = CNVS_CAP_BUTT;   break;
        case CANVAS_CAP_ROUND:  cv->cur.line_cap = CNVS_CAP_ROUND;  break;
        case CANVAS_CAP_SQUARE: cv->cur.line_cap = CNVS_CAP_SQUARE; break;
    }
}

void canvas_set_miter_limit(struct canvas *__single cv, float limit) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_miter_limit", (float[]){ limit }, 1); }
    cv->cur.miter_limit = limit;
}

void canvas_set_line_dash(struct canvas *__single cv,
                          float const *__counted_by(count) pattern, int count) {
    // Clamp into a separate variable: mutating `count` would desync the
    // __counted_by(count) bound on `pattern`.
    int m = count < 0 ? 0 : count;
    if (m > CANVAS_DASH_MAX) {
        m = CANVAS_DASH_MAX;
    }
    for (int i = 0; i < m; i++) {
        cv->cur.dash[i] = pattern[i];
    }
    cv->cur.dash_count = m;
    // Record the effective (clamped) pattern, so the line never exceeds the
    // parser's per-line dash cap and re-clamps to the same state on replay.
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_line_dash", cv->cur.dash, m); }
}

int canvas_get_line_dash(struct canvas *__single cv,
                         float *__counted_by(cap) out, int cap) {
    int const n = cv->cur.dash_count;
    // Copy at most `cap` entries; never write past the caller's buffer, and never
    // mutate `cap` itself (it bounds `out`).  A negative cap copies nothing.
    int const m = cap < n ? cap : n;
    for (int i = 0; i < m; i++) {
        out[i] = cv->cur.dash[i];
    }
    return n;
}

void canvas_set_line_dash_offset(struct canvas *__single cv, float offset) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_line_dash_offset", (float[]){ offset }, 1); }
    cv->cur.dash_offset = offset;
}

// Build the stroke triangles for `p` into cv->scratch_verts under the current
// line styles (width/join/cap/dash, CTM scale baked in).  False on alloc failure.
static bool build_stroke_verts(struct canvas *__single cv, struct cnvs_path const *p) {
    cnvs_verts_reset(&cv->scratch_verts);
    // Line width and dash lengths are in user units; bake the CTM scale in.
    float const scale = ctm_scale(cv->cur.ctm);
    float const half_width = cv->cur.line_width * 0.5f * scale;

    bool const dashed = cv->cur.dash_count > 0;
    float sdash[CANVAS_DASH_MAX];
    for (int i = 0; i < cv->cur.dash_count; i++) {
        sdash[i] = cv->cur.dash[i] * scale;
    }
    float const soff = cv->cur.dash_offset * scale;

    for (int s = 0; s < p->nsubs; s++) {
        cnvs_subpath const sp = p->subs[s];
        if (sp.count < 2) {
            continue;
        }
        cnvs_vec2 *poly = p->pts + sp.start;
        bool ok = dashed
                      ? cnvs_stroke_dashed(poly, sp.count, sp.closed, half_width, sdash,
                                           cv->cur.dash_count, soff,
                                           &cv->scratch_verts)
                      : cnvs_stroke_polyline(poly, sp.count, sp.closed, half_width,
                                             cv->cur.line_join, cv->cur.line_cap,
                                             cv->cur.miter_limit,
                                             &cv->scratch_verts);
        if (!ok) {
            return false;
        }
    }
    return true;
}

static void stroke_device_path(struct canvas *__single cv, struct cnvs_path const *p) {
    if (!build_stroke_verts(cv, p) || cv->scratch_verts.nverts < 3) {
        return;
    }
    cbbox b = points_bbox(cv, cv->scratch_verts.data, cv->scratch_verts.nverts,
                           filter_margin(cv));
    if (b.w <= 0 || b.h <= 0 || !ensure_tile(cv, b.w * b.h) ||
        !cnvs_cover_reset(&cv->cover, b.w, b.h)) {
        return;
    }
    // Feed each stroke triangle as edges, forced to a consistent winding so the
    // overlapping join/cap triangles union (nonzero) instead of cancelling.
    for (int i = 0; i + 2 < cv->scratch_verts.nverts; i += 3) {
        cnvs_vec2 const p0 = cv->scratch_verts.data[i];
        cnvs_vec2 p1 = cv->scratch_verts.data[i + 1];
        cnvs_vec2 p2 = cv->scratch_verts.data[i + 2];
        float const area = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
        if (area < 0.0f) {
            cnvs_vec2 const t = p1;
            p1 = p2;
            p2 = t;
        }
        cover_edge(cv, b, p0, p1);
        cover_edge(cv, b, p1, p2);
        cover_edge(cv, b, p2, p0);
    }
    cnvs_cover_resolve(&cv->cover, b.w, b.h, CNVS_NONZERO, cv->cov);
    paint_stroke(cv, b);
}

void canvas_stroke(struct canvas *__single cv) {
    if (cv->rec) { cnvs_rec_op(cv->rec, "stroke"); }
    stroke_device_path(cv, &cv->path);
}

// Twice the signed area of triangle (a,b,c); its sign is the winding, zero means
// the three points are collinear (a degenerate triangle).
static float orient(cnvs_vec2 a, cnvs_vec2 b, cnvs_vec2 c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Whether q lies in triangle (a,b,c) -- on the same side of all three edges,
// winding-agnostic, boundary counts as inside.  A degenerate triangle has no
// interior, so it never reports a hit (guards against the stroker's zero-area
// triangles swallowing every query).
static bool point_in_tri(cnvs_vec2 q, cnvs_vec2 a, cnvs_vec2 b, cnvs_vec2 c) {
    if (orient(a, b, c) == 0.0f) {
        return false;
    }
    float d1 = orient(a, b, q), d2 = orient(b, c, q), d3 = orient(c, a, q);
    bool const neg = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
    bool const pos = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
    return !(neg && pos);
}

// Whether q lies in the stroke triangles currently in cv->scratch_verts (their
// union -- inside any triangle counts).
static bool stroke_verts_contain(struct canvas *__single cv, cnvs_vec2 q) {
    for (int i = 0; i + 2 < cv->scratch_verts.nverts; i += 3) {
        if (point_in_tri(q, cv->scratch_verts.data[i], cv->scratch_verts.data[i + 1],
                         cv->scratch_verts.data[i + 2])) {
            return true;
        }
    }
    return false;
}

bool canvas_is_point_in_stroke(struct canvas *__single cv, float x, float y) {
    if (!isfinite(x) || !isfinite(y)) {
        return false;
    }
    // Build the same stroke triangles canvas_stroke would paint, then test the
    // (transformed) query point against their union.
    if (!build_stroke_verts(cv, &cv->path)) {
        return false;
    }
    return stroke_verts_contain(cv, xf(cv, x, y));
}

void canvas_stroke_rect(struct canvas *__single cv, float x, float y, float w, float h) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "stroke_rect", (float[]){ x, y, w, h }, 4); }
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h)) {
        return;  // Canvas spec: non-finite args paint nothing.
    }
    // strokeRect builds and strokes its own rectangle without touching the
    // current path; the corners go through the CTM exactly as fill_rect's quad.
    struct cnvs_path rp;
    cnvs_path_init(&rp);
    if (w == 0.0f && h == 0.0f) {
        // Spec: a single-point subpath.  Our stroker emits nothing for a
        // zero-length subpath (no caps on a bare point) -- the lone deviation.
        cnvs_path_move_to(&rp, xf(cv, x, y));
    } else if (w == 0.0f || h == 0.0f) {
        // A degenerate rect is a hairline: an open two-point subpath, so caps
        // (not joins) bracket it.  The far corner coincides for both axes.
        cnvs_path_move_to(&rp, xf(cv, x, y));
        cnvs_path_line_to(&rp, xf(cv, x + w, y + h));
    } else {
        cnvs_path_rect(&rp, xf(cv, x, y), xf(cv, x + w, y),
                       xf(cv, x + w, y + h), xf(cv, x, y + h));
    }
    stroke_device_path(cv, &rp);
    cnvs_path_free(&rp);
}

// The pinned font family: shaping, the primary font handle, and the text
// cache's vmetrics record all key on this one name.
static char const k_font_family[] = "Libian TC";

// Rebuild the cached font when the requested size changes; NULL on failure.
static struct cnvs_font *__single ensure_font(struct canvas *__single cv) {
    if (!cv->font || fabsf(cv->font_built_size - cv->cur.font_size) > 1e-6f) {
        cnvs_font_free(cv->font);
        cv->font = cnvs_font(k_font_family, (int)sizeof k_font_family - 1,
                                    cv->cur.font_size);
        cv->font_built_size = cv->cur.font_size;
    }
    return cv->font;
}

// The primary font's vertical metrics at the current size, in user px, through
// the text cache's per-name record (vmetrics normalized at size 1.0): a
// replayed program reads the recorded values and never needs the real font,
// and the live path scales through the same normalized floats so recording
// and replay place baselines bit-identically.  Populated from a live font
// handle on first use.  False only when no record exists and the font can't
// be built (then there are no metrics to give).
static bool canvas_vmetrics(struct canvas *__single cv, float *__single ascent,
                            float *__single descent) {
    float const size = cv->cur.font_size;
    int fid = cnvs_text_cache_intern(&cv->text_cache, k_font_family,
                                     (int)sizeof k_font_family - 1);
    float a1 = 0.0f, d1 = 0.0f;
    if (cnvs_text_cache_get_vmetrics(&cv->text_cache, fid, &a1, &d1)) {
        *ascent = a1 * size;
        *descent = d1 * size;
        return true;
    }
    struct cnvs_font *__single f = ensure_font(cv);
    if (!f) {
        return false;
    }
    float a = 0.0f, d = 0.0f;
    cnvs_font_vmetrics(f, &a, &d);
    if (fid >= 0 && size > 0.0f) {
        cnvs_text_cache_set_vmetrics(&cv->text_cache, fid, a / size, d / size);
        if (cnvs_text_cache_get_vmetrics(&cv->text_cache, fid, &a1, &d1)) {
            *ascent = a1 * size;  // re-derive through the stored record, so the
            *descent = d1 * size; // live values match a future replay's exactly
            return true;
        }
    }
    *ascent = a;
    *descent = d;
    return true;
}

void canvas_set_font_size(struct canvas *__single cv, float px) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_font_size", (float[]){ px }, 1); }
    cv->cur.font_size = px > 0.0f ? px : 0.0f;
}

void canvas_set_text_align(struct canvas *__single cv, enum canvas_text_align align) {
    switch (align) {
        case CANVAS_ALIGN_START:
        case CANVAS_ALIGN_END:
        case CANVAS_ALIGN_LEFT:
        case CANVAS_ALIGN_RIGHT:
        case CANVAS_ALIGN_CENTER:
            if (cv->rec) { cnvs_rec_text_align(cv->rec, align); }
            cv->cur.text_align = align;
            break;
    }
}

void canvas_set_direction(struct canvas *__single cv, enum canvas_direction dir) {
    switch (dir) {
        case CANVAS_DIRECTION_LTR:
        case CANVAS_DIRECTION_RTL:
            if (cv->rec) { cnvs_rec_direction(cv->rec, dir); }
            cv->cur.direction = dir;
            break;
    }
}

void canvas_set_text_baseline(struct canvas *__single cv, enum canvas_text_baseline baseline) {
    switch (baseline) {
        case CANVAS_BASELINE_ALPHABETIC:
        case CANVAS_BASELINE_TOP:
        case CANVAS_BASELINE_HANGING:
        case CANVAS_BASELINE_MIDDLE:
        case CANVAS_BASELINE_IDEOGRAPHIC:
        case CANVAS_BASELINE_BOTTOM:
            if (cv->rec) { cnvs_rec_text_baseline(cv->rec, baseline); }
            cv->cur.text_baseline = baseline;
            break;
    }
}

// Fraction of the advance the textAlign anchor sits from the text's left edge:
// left 0, center 0.5, right 1.  start/end resolve through the direction
// attribute -- start is the edge the text flows from (left under ltr, right
// under rtl), end the edge it flows toward.
static float text_align_frac(enum canvas_text_align a, enum canvas_direction dir) {
    bool const rtl = dir == CANVAS_DIRECTION_RTL;
    switch (a) {
        case CANVAS_ALIGN_START:  return rtl ? 1.0f : 0.0f;
        case CANVAS_ALIGN_END:    return rtl ? 0.0f : 1.0f;
        case CANVAS_ALIGN_LEFT:   return 0.0f;
        case CANVAS_ALIGN_CENTER: return 0.5f;
        case CANVAS_ALIGN_RIGHT:  return 1.0f;
    }
    return 0.0f;  // unreachable for a valid enum
}

// Offset added to the pen y to place the requested textBaseline at y, derived
// from the font's ascent/descent (no BASE table: hanging ~ top, ideographic ~
// bottom).  The metrics come through the cached vmetrics record, so a replayed
// program needs no font handle to place a baseline.
static float text_baseline_offset(struct canvas *__single cv) {
    if (cv->cur.text_baseline == CANVAS_BASELINE_ALPHABETIC) {
        return 0.0f;
    }
    float a = 0.0f, d = 0.0f;
    if (!canvas_vmetrics(cv, &a, &d)) {
        return 0.0f;
    }
    switch (cv->cur.text_baseline) {
        case CANVAS_BASELINE_ALPHABETIC:  return 0.0f;
        case CANVAS_BASELINE_TOP:         return a;
        case CANVAS_BASELINE_HANGING:     return a;
        case CANVAS_BASELINE_MIDDLE:      return (a - d) * 0.5f;
        case CANVAS_BASELINE_IDEOGRAPHIC: return -d;
        case CANVAS_BASELINE_BOTTOM:      return -d;
    }
    return 0.0f;  // unreachable for a valid enum
}

// Shape `text` (UTF-8, `len` bytes) with the current font/size under the
// current paragraph direction, through the canvas's text cache: a repeated
// (size, direction, text) triple -- a frame's static labels, or measureText
// before fillText -- reuses the cached line instead of re-shaping at the
// boundary.  Draw and measure both come through here, so a string measures the
// way it draws under either direction.  NULL on failure.  The result is
// BORROWED from the cache (do not free); it stays valid until the next shape
// lookup, and every caller is done with it before making another.
static struct cnvs_shaped const *__single shape_text(struct canvas *__single cv,
                                              char const *__counted_by(len) text,
                                              int len) {
    return cnvs_text_cache_shaping(&cv->text_cache, k_font_family,
                                 (int)sizeof k_font_family - 1,
                                 cv->cur.font_size,
                                 cv->cur.direction == CANVAS_DIRECTION_RTL,
                                 text, len);
}

// Render one color (emoji) glyph from its canonical capture: pick the mip
// level pair around the glyph quad's device footprint and sample it through
// the same transform-aware trilinear path drawImage minification takes -- so
// the emoji takes the transform, clip, global alpha, and shadow like any
// other image, and no boundary call (indeed, no CTFontRef at all) is needed
// once the capture exists.  The capture covers the glyph-space rect
// [ink_x0, ink_x0 + capture_w] x [ink_y0, ink_y0 + capture_h] in capture px (y up,
// baseline-relative); scaling by size_px / CNVS_CAPTURE_EM and pinning to the
// pen maps it to user space.
static void draw_glyph_capture(struct canvas *__single cv, struct cnvs_glyph_slot *__single slot,
                               float pen_x, float baseline_y, float size_px) {
    float const k = size_px / (float)CNVS_CAPTURE_EM;
    float const dw = (float)slot->capture_w * k;
    float const dh = (float)slot->capture_h * k;
    if (!(dw > 0.0f) || !(dh > 0.0f)) {
        return;
    }
    float const dx = pen_x + slot->ink_x0 * k;
    float const dy = baseline_y - (slot->ink_y0 + (float)slot->capture_h) * k;
    // The mip rule: the quad's device footprint is its longer mapped edge.
    // The finer level of the pair is the smallest one >= that footprint (so
    // its taps never downscale by more than 2x), and the blend toward the
    // ceil-halved level under it tracks the footprint between the two --
    // level-selection popping along a continuous zoom smooths into a fade.
    cnvs_mat const m = cv->cur.ctm;
    float const ex = hypotf(m.a * dw, m.b * dw);
    float const ey = hypotf(m.c * dh, m.d * dh);
    cnvs_mip hi, lo;
    float const lt = cnvs_glyph_mip_pair(slot, ex > ey ? ex : ey, &hi, &lo);
    if (!hi.px) {
        return;
    }
    draw_image_quad(cv, hi.px, hi.len, hi.w, hi.h, 0.0f, 0.0f,
                    (float)hi.w, (float)hi.h, dx, dy, dw, dh, true,
                    CANVAS_COLOR_UNORM8, false, false, NULL, hi, lo, lt);
}

// The degraded path when the capture cache can't serve (full table, OOM) but
// the run still has its boundary handle: ask the boundary for the ink box,
// draw into a checked RGBA8 buffer at device size, unpremultiply, then
// composite through the CTM with canvas_draw_bitmap_subrect.
static void draw_color_glyph(struct canvas *__single cv, void *__single font,
                             uint16_t glyph, float pen_x, float baseline_y) {
    float x0, y0, x1, y1;
    cnvs_glyph_bounds(font, glyph, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0) {
        return;  // blank glyph (e.g. a space in the color font)
    }
    int const margin = 1;
    float const bw = ceilf(x1 - x0);
    float const bh = ceilf(y1 - y0);
    int const gw = (int)bw + 2 * margin;
    int const gh = (int)bh + 2 * margin;
    if (gw < 1 || gh < 1 || gw > 4096 || gh > 4096) {
        return;
    }
    int const glen = gw * gh * 4;
    uint8_t *buf = malloc((size_t)glen);
    if (!buf) {
        return;
    }
    memset(buf, 0, (size_t)glen);
    // Draw with the glyph origin placed so its ink box sits `margin` px inside the
    // buffer (cnvs_glyph_draw is bitmap space: y up from the bottom).
    cnvs_glyph_draw(font, glyph, (float)margin - x0, (float)margin - y0, buf, gw, gh);
    // CGBitmapContext gives premultiplied RGBA, top row first (the orientation
    // canvas_draw_bitmap_subrect wants); just unpremultiply to straight alpha.
    for (int i = 0; i < glen; i += 4) {
        int const a = buf[i + 3];
        if (a > 0 && a < 255) {
            buf[i + 0] = (uint8_t)((buf[i + 0] * 255 + a / 2) / a);
            buf[i + 1] = (uint8_t)((buf[i + 1] * 255 + a / 2) / a);
            buf[i + 2] = (uint8_t)((buf[i + 2] * 255 + a / 2) / a);
        }
    }
    // The buffer maps to a user-space rect: its left edge is `margin` px left of
    // the glyph ink, its top edge `gh - margin + y0` glyph-px above the baseline.
    float const dest_x = pen_x + x0 - (float)margin;
    float const dest_y = baseline_y - ((float)gh - (float)margin + y0);
    canvas_draw_bitmap_subrect(cv, buf, gw, gh, 0.0f, 0.0f, (float)gw, (float)gh,
                              dest_x, dest_y, (float)gw, (float)gh);
    free(buf);
}

// cnvs_shaped_outline's color-glyph callback context: the canvas plus the size
// the line was shaped at (the capture px scale's numerator).
struct color_glyph_ctx {
    struct canvas *__single cv;
    float size_px;
};

// cnvs_shaped_outline's color-glyph callback: the context rides along untyped
// (checked C on both ends of the void* hop, so no forge), and each emoji glyph
// composites immediately at its pen position, interleaved with the outline
// accumulation.  The canonical capture
// draws it (one boundary rasterization per glyph ever, and none at all when it
// came from a replayed bitmap block); only when the cache can't serve does a
// live font handle fall back to the per-draw boundary render, and a handle-less
// run with no capture draws as a blank advance.
static void paint_color_glyph(void *__single ctx, int fid, void *__single font,
                              uint16_t glyph, float pen_x, float baseline_y) {
    struct color_glyph_ctx *__single cc = ctx;
    struct cnvs_glyph_slot *__single slot =
        cnvs_text_cache_color(&cc->cv->text_cache, fid, font, glyph);
    if (slot) {
        if (slot->capture_w > 0) {
            draw_glyph_capture(cc->cv, slot, pen_x, baseline_y, cc->size_px);
        }
        return;  // a cached blank: known to have no ink, nothing to draw
    }
    if (font) {
        draw_color_glyph(cc->cv, font, glyph, pen_x, baseline_y);
    }
}

// Paint a shaped line from pen origin (ox, oy) through `to_device`: accumulate the
// outline glyphs into one path (filled or stroked), and composite color-glyph runs
// (emoji) from their canonical captures.  Core Text font fallback already happened
// during shaping.  One run/pen walk serves outlines and emoji alike:
// cnvs_shaped_outline does the layout and hands color glyphs back through the
// callback above.
static void paint_shaped(struct canvas *__single cv, struct cnvs_shaped const *__single s,
                         float ox, float oy, cnvs_mat to_device, bool stroke) {
    cnvs_path_reset(&cv->text_path);
    struct color_glyph_ctx cc = { .cv = cv, .size_px = s->size_px };
    cnvs_shaped_outline(&cv->text_cache, s, ox, oy, to_device, CANVAS_FLATTEN_TOL,
                        &cv->text_path, paint_color_glyph, &cc);
    if (stroke) {
        stroke_device_path(cv, &cv->text_path);
    } else {
        fill_device_path(cv, &cv->text_path, CNVS_NONZERO);
    }
}

// Shape once, place by textAlign/textBaseline (condensing to max_width if finite and
// positive), and paint.  One shaped line drives both the alignment advance and the
// glyphs, so emoji and fallback runs are measured the same way they are drawn.
static void do_text(struct canvas *__single cv, char const *__counted_by(len) text, int len,
                    float x, float y, float max_width, bool stroke) {
    if (!isfinite(x) || !isfinite(y)) {
        return;  // spec: fillText/strokeText with non-finite coordinates draw
    }            // nothing (and an inf pen would poison every glyph point)
    struct cnvs_shaped const *__single s = shape_text(cv, text, len);
    if (!s) {
        return;
    }
    float const advance = cnvs_shaped_width(s);
    float sx = 1.0f;
    if (isfinite(max_width) && max_width > 0.0f && advance > max_width) {
        sx = max_width / advance;  // condense in x to fit
    }
    float ox = x - text_align_frac(cv->cur.text_align, cv->cur.direction)
                       * advance * sx;
    float const oy = y + text_baseline_offset(cv);
    cnvs_mat td = cv->cur.ctm;
    if (sx != 1.0f) {
        // Scale x by sx about the anchor: X' = sx*X + ox*(1-sx), Y' = Y; then the CTM.
        cnvs_mat cond = { .a = sx, .b = 0.0f, .c = 0.0f, .d = 1.0f,
                          .e = ox * (1.0f - sx), .f = 0.0f };
        td = cnvs_mat_mul(cv->cur.ctm, cond);
    }
    paint_shaped(cv, s, ox, oy, td, stroke);
}

float canvas_measure_text(struct canvas *__single cv, char const *__null_terminated text) {
    int const len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    struct cnvs_shaped const *__single s = shape_text(cv, t, len);
    if (!s) {
        return 0.0f;
    }
    return cnvs_shaped_width(s);
}

canvas_text_metrics canvas_measure_text_full(struct canvas *__single cv,
                                             char const *__null_terminated text) {
    canvas_text_metrics m;
    memset(&m, 0, sizeof m);  // all-zero if the font/shaping can't be built
    float a = 0.0f, d = 0.0f;
    bool const have_vm = canvas_vmetrics(cv, &a, &d);  // cached: no font handle needed
    int const len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    struct cnvs_shaped const *__single s = shape_text(cv, t, len);
    if (have_vm && s) {
        cnvs_text_metrics tm;  // fallback-aware: each glyph in its run's font
        cnvs_shaped_metrics(&cv->text_cache, s, cv->cur.font_size, a, d, &tm);
        m.width                       = tm.width;
        m.actual_bounding_box_left    = tm.actual_left;
        m.actual_bounding_box_right   = tm.actual_right;
        m.actual_bounding_box_ascent  = tm.actual_ascent;
        m.actual_bounding_box_descent = tm.actual_descent;
        m.font_bounding_box_ascent    = tm.font_ascent;
        m.font_bounding_box_descent   = tm.font_descent;
        m.em_height_ascent            = tm.em_ascent;
        m.em_height_descent           = tm.em_descent;
        m.alphabetic_baseline         = tm.alphabetic_baseline;
        m.hanging_baseline            = tm.hanging_baseline;
        m.ideographic_baseline        = tm.ideographic_baseline;
    }
    return m;
}

// The per-canvas text cache, for tests and stats -- declared in cnvs_text.h so
// it stays off the public canvas.h surface (tests include internal headers).
struct cnvs_text_cache *__single cnvs_canvas_text_cache(struct canvas *__single cv) {
    return &cv->text_cache;
}

// Recording a text op: first make sure the cache holds everything the op is
// about to use (the family's vmetrics record and the shaped line -- the same
// lookups the draw takes, so this adds no boundary traffic), then serialize
// the not-yet-emitted font/glyph/shape blocks ahead of the op line.  The
// recorded program is self-contained: replay rebuilds the cache from the
// blocks and draws with no text boundary at all.
static void record_text_blocks(struct canvas *__single cv,
                               char const *__counted_by(len) text, int len) {
    float a = 0.0f, d = 0.0f;
    (void)canvas_vmetrics(cv, &a, &d);  // intern the family + its vmetrics
    (void)shape_text(cv, text, len);    // ensure the line is cached
    cnvs_rec_text_blocks(cv->rec, &cv->text_cache, cv->cur.font_size,
                         cv->cur.direction == CANVAS_DIRECTION_RTL, text, len);
}

void canvas_fill_text_n(struct canvas *__single cv, char const *__counted_by(len) text,
                        int len, float x, float y) {
    if (cv->rec) {
        record_text_blocks(cv, text, len);
        cnvs_rec_text(cv->rec, "fill_text", x, y, text, len);
    }
    do_text(cv, text, len, x, y, -1.0f, false);
}

void canvas_fill_text(struct canvas *__single cv, char const *__null_terminated text,
                      float x, float y) {
    int const len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    canvas_fill_text_n(cv, t, len, x, y);
}

void canvas_fill_text_max_n(struct canvas *__single cv, char const *__counted_by(len) text,
                            int len, float x, float y, float max_width) {
    if (cv->rec) {
        record_text_blocks(cv, text, len);
        cnvs_rec_text_max(cv->rec, "fill_text_max", x, y, max_width, text, len);
    }
    do_text(cv, text, len, x, y, max_width, false);
}

void canvas_fill_text_max(struct canvas *__single cv, char const *__null_terminated text,
                          float x, float y, float max_width) {
    int const len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    canvas_fill_text_max_n(cv, t, len, x, y, max_width);
}

void canvas_stroke_text_n(struct canvas *__single cv, char const *__counted_by(len) text,
                          int len, float x, float y) {
    if (cv->rec) {
        record_text_blocks(cv, text, len);
        cnvs_rec_text(cv->rec, "stroke_text", x, y, text, len);
    }
    do_text(cv, text, len, x, y, -1.0f, true);
}

void canvas_stroke_text(struct canvas *__single cv, char const *__null_terminated text,
                        float x, float y) {
    int const len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    canvas_stroke_text_n(cv, t, len, x, y);
}

void canvas_stroke_text_max_n(struct canvas *__single cv,
                              char const *__counted_by(len) text,
                              int len, float x, float y, float max_width) {
    if (cv->rec) {
        record_text_blocks(cv, text, len);
        cnvs_rec_text_max(cv->rec, "stroke_text_max", x, y, max_width, text, len);
    }
    do_text(cv, text, len, x, y, max_width, true);
}

void canvas_stroke_text_max(struct canvas *__single cv, char const *__null_terminated text,
                            float x, float y, float max_width) {
    int const len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    canvas_stroke_text_max_n(cv, t, len, x, y, max_width);
}

// Bilinear sample of an RGBA8 source at source-pixel (fx, fy), unpremultiplied,
// clamp-to-edge.
static void sample_src(uint8_t const *__counted_by(slen) src, int slen,
                       int sw, int sh, float fx, float fy,
                       float *__counted_by(4) out) {
    (void)slen;
    float gx = fx - 0.5f, gy = fy - 0.5f;
    float fxx = floorf(gx), fyy = floorf(gy);
    int x0 = cnvs_f2i(fxx), y0 = cnvs_f2i(fyy);
    // cnvs_f2i saturates a huge source coordinate (an inf/1e30 subrect maps
    // device pixels far off the source) to INT_MAX, so the +1 must saturate
    // too: the edge clamps below fold both taps to the same column/row either
    // way, and in-range samples are untouched.
    int x1 = x0 < INT_MAX ? x0 + 1 : x0, y1 = y0 < INT_MAX ? y0 + 1 : y0;
    float tx = gx - fxx, ty = gy - fyy;
    if (x0 < 0) { x0 = 0; } else if (x0 > sw - 1) { x0 = sw - 1; }
    if (x1 < 0) { x1 = 0; } else if (x1 > sw - 1) { x1 = sw - 1; }
    if (y0 < 0) { y0 = 0; } else if (y0 > sh - 1) { y0 = sh - 1; }
    if (y1 < 0) { y1 = 0; } else if (y1 > sh - 1) { y1 = sh - 1; }
    for (int k = 0; k < 4; k++) {
        float const c00 = (float)src[(y0 * sw + x0) * 4 + k];
        float const c10 = (float)src[(y0 * sw + x1) * 4 + k];
        float const c01 = (float)src[(y1 * sw + x0) * 4 + k];
        float const c11 = (float)src[(y1 * sw + x1) * 4 + k];
        float const top = c00 + (c10 - c00) * tx;
        float const bot = c01 + (c11 - c01) * tx;
        out[k] = (top + (bot - top) * ty) / 255.0f;
    }
}

// Nearest-neighbour sample of an RGBA8 source at source-pixel (fx, fy),
// unpremultiplied, clamp-to-edge: pick the pixel whose cell contains the point.
static void sample_src_nearest(uint8_t const *__counted_by(slen) src, int slen,
                               int sw, int sh, float fx, float fy,
                               float *__counted_by(4) out) {
    (void)slen;
    int x = cnvs_f2i(floorf(fx));
    int y = cnvs_f2i(floorf(fy));
    if (x < 0) { x = 0; } else if (x > sw - 1) { x = sw - 1; }
    if (y < 0) { y = 0; } else if (y > sh - 1) { y = sh - 1; }
    int const o = (y * sw + x) * 4;
    for (int k = 0; k < 4; k++) {
        out[k] = (float)src[o + k] / 255.0f;
    }
}

// High-quality magnification: the BC-spline family (Mitchell-Netravali 1988).
// Catmull-Rom is (B, C) = (0, 1/2); Mitchell's compromise is (1/3, 1/3).
// Swap this one line to try the other -- deliberately not runtime state, just
// easy to fiddle.  (tests/test_sampling.c pins Catmull-Rom's half-phase
// weights; re-pin there too.)
static float const CUBIC_B = 0.0f, CUBIC_C = 0.5f;

// The family's kernel: two cubic pieces, |x| < 1 and 1 <= |x| < 2, zero
// beyond.  The four tap weights around any phase sum to 1.
static float cubic_weight(float x) {
    float const B = CUBIC_B, C = CUBIC_C;
    x = fabsf(x);
    if (x < 1.0f) {
        return ((12.0f - 9.0f * B - 6.0f * C) * x * x * x +
                (-18.0f + 12.0f * B + 6.0f * C) * x * x +
                (6.0f - 2.0f * B)) * (1.0f / 6.0f);
    }
    if (x < 2.0f) {
        return ((-B - 6.0f * C) * x * x * x +
                (6.0f * B + 30.0f * C) * x * x +
                (-12.0f * B - 48.0f * C) * x +
                (8.0f * B + 24.0f * C)) * (1.0f / 6.0f);
    }
    return 0.0f;
}

// 4x4 BC-spline sample of an RGBA8 source at source-pixel (fx, fy),
// clamp-to-edge.  A straight-alpha tap premultiplies BEFORE weighting -- a
// negative lobe against straight alpha would synthesize colour out of fully
// transparent texels -- and a premultiplied tap (premul true: snapshots)
// weights as-is; either way the result is premultiplied, clamped back into
// the premultiplied range (overshoot both ways is part of the kernel; the
// clamp is the invariant).
static void sample_src_cubic(uint8_t const *__counted_by(slen) src, int slen,
                             int sw, int sh, float fx, float fy, bool premul,
                             float *__counted_by(4) out) {
    (void)slen;
    float const gx = fx - 0.5f, gy = fy - 0.5f;
    float const fxx = floorf(gx), fyy = floorf(gy);
    // Saturate the bases into a range the +-2 tap walk cannot overflow
    // (cnvs_f2i saturates huge coords to INT_MAX); the per-tap edge clamp
    // below does the real work.
    int bx = cnvs_f2i(fxx), by = cnvs_f2i(fyy);
    bx = bx < -2 ? -2 : (bx > sw ? sw : bx);
    by = by < -2 ? -2 : (by > sh ? sh : by);
    float const tx = gx - fxx, ty = gy - fyy;
    float wx[4], wy[4];
    for (int t = 0; t < 4; t++) {
        wx[t] = cubic_weight((float)(t - 1) - tx);
        wy[t] = cubic_weight((float)(t - 1) - ty);
    }
    float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int j = 0; j < 4; j++) {
        int y = by - 1 + j;
        if (y < 0) { y = 0; } else if (y > sh - 1) { y = sh - 1; }
        float row[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (int t = 0; t < 4; t++) {
            int x = bx - 1 + t;
            if (x < 0) { x = 0; } else if (x > sw - 1) { x = sw - 1; }
            int const o = (y * sw + x) * 4;
            float const a = (float)src[o + 3] * (1.0f / 255.0f);
            float const w = premul ? wx[t] : wx[t] * a;
            row[0] += w * ((float)src[o + 0] * (1.0f / 255.0f));
            row[1] += w * ((float)src[o + 1] * (1.0f / 255.0f));
            row[2] += w * ((float)src[o + 2] * (1.0f / 255.0f));
            row[3] += wx[t] * a;
        }
        for (int c = 0; c < 4; c++) {
            acc[c] += wy[j] * row[c];
        }
    }
    float const a = acc[3] < 0.0f ? 0.0f : (acc[3] > 1.0f ? 1.0f : acc[3]);
    out[3] = a;
    for (int c = 0; c < 3; c++) {
        float const v = acc[c];
        out[c] = v < 0.0f ? 0.0f : (v > a ? a : v);
    }
}

// The f16 sampler twins -- same shapes as the u8 three above, reading
// _Float16 channels from byte buffers (ld16, defined with the mip machinery
// below).  Values come back as the stored numbers, straight or premultiplied
// per the image's alpha type; no 1/255 normalize.  None of the four image
// formats is the favourite: u8 and f16 each get the full sampler set.
static _Float16 ld16(uint8_t const *__counted_by(2) p);

static void sample_src_f16(uint8_t const *__counted_by(slen) src, int slen,
                           int sw, int sh, float fx, float fy,
                           float *__counted_by(4) out) {
    (void)slen;
    float gx = fx - 0.5f, gy = fy - 0.5f;
    float fxx = floorf(gx), fyy = floorf(gy);
    int x0 = cnvs_f2i(fxx), y0 = cnvs_f2i(fyy);
    int x1 = x0 < INT_MAX ? x0 + 1 : x0, y1 = y0 < INT_MAX ? y0 + 1 : y0;
    float tx = gx - fxx, ty = gy - fyy;
    if (x0 < 0) { x0 = 0; } else if (x0 > sw - 1) { x0 = sw - 1; }
    if (x1 < 0) { x1 = 0; } else if (x1 > sw - 1) { x1 = sw - 1; }
    if (y0 < 0) { y0 = 0; } else if (y0 > sh - 1) { y0 = sh - 1; }
    if (y1 < 0) { y1 = 0; } else if (y1 > sh - 1) { y1 = sh - 1; }
    for (int k = 0; k < 4; k++) {
        float const c00 = (float)ld16(src + ((y0 * sw + x0) * 4 + k) * 2);
        float const c10 = (float)ld16(src + ((y0 * sw + x1) * 4 + k) * 2);
        float const c01 = (float)ld16(src + ((y1 * sw + x0) * 4 + k) * 2);
        float const c11 = (float)ld16(src + ((y1 * sw + x1) * 4 + k) * 2);
        float const top = c00 + (c10 - c00) * tx;
        float const bot = c01 + (c11 - c01) * tx;
        out[k] = top + (bot - top) * ty;
    }
}

static void sample_src_nearest_f16(uint8_t const *__counted_by(slen) src,
                                   int slen, int sw, int sh, float fx,
                                   float fy, float *__counted_by(4) out) {
    (void)slen;
    int x = cnvs_f2i(floorf(fx));
    int y = cnvs_f2i(floorf(fy));
    if (x < 0) { x = 0; } else if (x > sw - 1) { x = sw - 1; }
    if (y < 0) { y = 0; } else if (y > sh - 1) { y = sh - 1; }
    for (int k = 0; k < 4; k++) {
        out[k] = (float)ld16(src + ((y * sw + x) * 4 + k) * 2);
    }
}

static void sample_src_cubic_f16(uint8_t const *__counted_by(slen) src,
                                 int slen, int sw, int sh, float fx, float fy,
                                 bool premul, float *__counted_by(4) out) {
    (void)slen;
    float const gx = fx - 0.5f, gy = fy - 0.5f;
    float const fxx = floorf(gx), fyy = floorf(gy);
    int bx = cnvs_f2i(fxx), by = cnvs_f2i(fyy);
    bx = bx < -2 ? -2 : (bx > sw ? sw : bx);
    by = by < -2 ? -2 : (by > sh ? sh : by);
    float const tx = gx - fxx, ty = gy - fyy;
    float wx[4], wy[4];
    for (int t = 0; t < 4; t++) {
        wx[t] = cubic_weight((float)(t - 1) - tx);
        wy[t] = cubic_weight((float)(t - 1) - ty);
    }
    float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int j = 0; j < 4; j++) {
        int y = by - 1 + j;
        if (y < 0) { y = 0; } else if (y > sh - 1) { y = sh - 1; }
        float row[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (int t = 0; t < 4; t++) {
            int x = bx - 1 + t;
            if (x < 0) { x = 0; } else if (x > sw - 1) { x = sw - 1; }
            int const o = ((y * sw + x) * 4) * 2;
            float const a = (float)ld16(src + o + 3 * 2);
            float const w = premul ? wx[t] : wx[t] * a;
            row[0] += w * (float)ld16(src + o + 0);
            row[1] += w * (float)ld16(src + o + 1 * 2);
            row[2] += w * (float)ld16(src + o + 2 * 2);
            row[3] += wx[t] * a;
        }
        for (int c = 0; c < 4; c++) {
            acc[c] += wy[j] * row[c];
        }
    }
    float const a = acc[3] < 0.0f ? 0.0f : (acc[3] > 1.0f ? 1.0f : acc[3]);
    out[3] = a;
    for (int c = 0; c < 3; c++) {
        float const v = acc[c];
        out[c] = v < 0.0f ? 0.0f : (v > a ? a : v);
    }
}

// One mip level's placement in the canvas's mips scratch.
typedef struct {
    int off, w, h;
} mip_level;

// Bytes per pixel for each colour type: 4 RGBA channels, u8 or _Float16.
static int px_bpp(enum canvas_color_type ct) {
    return ct == CANVAS_COLOR_F16 ? 8 : 4;
}

enum { MIP_MAX_LEVELS = 16 };  // 1 << 15 > CANVAS_DIM_MAX: a chain always fits

static bool ensure_mips(struct canvas *__single cv, int n) {
    if (n > cv->mips_cap) {
        uint8_t *na = realloc(cv->mips, (size_t)n);
        if (!na) {
            return false;
        }
        cv->mips = na;
        cv->mips_cap = n;
    }
    return true;
}

// Plan a sw x sh source's mip chain: level 0 at full size, ceil-halves after,
// down to `need` levels past 0 (capped at the natural 1x1 floor and the level
// budget).  Fills lv, returns the count, and leaves the chain's total byte
// size in *total.
static int plan_mips(int sw, int sh, int need, int bpp,
                     mip_level *__counted_by(MIP_MAX_LEVELS) lv,
                     int *__single total) {
    int n = 0, bytes = 0;
    for (int w = sw, h = sh;;) {
        lv[n] = (mip_level){ .off = bytes, .w = w, .h = h };
        bytes += w * h * bpp;
        n++;
        if (n > need || n == MIP_MAX_LEVELS || (w == 1 && h == 1)) {
            break;
        }
        w = (w + 1) / 2;
        h = (h + 1) / 2;
    }
    *total = bytes;
    return n;
}

// f16 channels live in byte buffers (every image format shares one byte
// layout); these two are the alignment-clean load/store seam.
static _Float16 ld16(uint8_t const *__counted_by(2) p) {
    _Float16 v;
    memcpy(&v, p, sizeof v);
    return v;
}

static void st16(uint8_t *__counted_by(2) p, _Float16 v) {
    memcpy(p, &v, sizeof v);
}

// The f16 chain halve: 2x2 average in float, rounded once per channel to
// f16.  Float averaging preserves the premul invariant exactly, but the
// independent f16 roundings could break rgb <= a by an ulp, so the clamp
// restores the contract (the u8 halve's shared-rounding trick, by other
// means).
static void mip_halve_f16(uint8_t const *__counted_by(sw * sh * 8) src,
                          int sw, int sh,
                          uint8_t *__counted_by(dw * dh * 8) dst,
                          int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int const y0 = 2 * y;
        int const y1 = y0 + 1 < sh ? y0 + 1 : y0;
        for (int x = 0; x < dw; x++) {
            int const x0 = 2 * x;
            int const x1 = x0 + 1 < sw ? x0 + 1 : x0;
            float c[4];
            for (int k = 0; k < 4; k++) {
                c[k] = 0.25f *
                       ((float)ld16(src + ((y0 * sw + x0) * 4 + k) * 2) +
                        (float)ld16(src + ((y0 * sw + x1) * 4 + k) * 2) +
                        (float)ld16(src + ((y1 * sw + x0) * 4 + k) * 2) +
                        (float)ld16(src + ((y1 * sw + x1) * 4 + k) * 2));
            }
            int const o = ((y * dw + x) * 4) * 2;
            _Float16 const a = (_Float16)c[3];
            for (int k = 0; k < 3; k++) {
                _Float16 const v = (_Float16)c[k];
                st16(dst + o + k * 2, v > a ? a : v);
            }
            st16(dst + o + 3 * 2, a);
        }
    }
}

// Fill a planned chain: level 0 premultiplies a straight-alpha source
// (filtering must happen premultiplied; averaging straight RGBA bleeds
// colour from transparent texels) or copies an already premultiplied one,
// and each level after ceil-halves the one before -- the u8 arm through
// cnvs_mip_halve (the emoji captures' kernel), the f16 arm through
// mip_halve_f16.  The four image formats are peers here: each colour type
// has its own halve, each alpha type its own level-0 entry.
static void fill_mips(uint8_t *__counted_by(total) dst, int total,
                      uint8_t const *__counted_by(slen) src, int slen,
                      int sw, int sh, enum canvas_color_type ct, bool premul,
                      mip_level const *__counted_by(MIP_MAX_LEVELS) lv, int n) {
    (void)slen;
    (void)total;
    if (premul) {
        memcpy(dst, src, (size_t)(sw * sh * px_bpp(ct)));
    } else if (ct == CANVAS_COLOR_F16) {
        for (int p = 0; p < sw * sh; p++) {
            float const a = (float)ld16(src + (p * 4 + 3) * 2);
            for (int k = 0; k < 3; k++) {
                float const v = (float)ld16(src + (p * 4 + k) * 2) * a;
                st16(dst + (p * 4 + k) * 2, (_Float16)v);
            }
            st16(dst + (p * 4 + 3) * 2, (_Float16)a);
        }
    } else {
        for (int p = 0; p < sw * sh; p++) {
            int const a = src[p * 4 + 3];
            dst[p * 4 + 0] = (uint8_t)((src[p * 4 + 0] * a + 127) / 255);
            dst[p * 4 + 1] = (uint8_t)((src[p * 4 + 1] * a + 127) / 255);
            dst[p * 4 + 2] = (uint8_t)((src[p * 4 + 2] * a + 127) / 255);
            dst[p * 4 + 3] = (uint8_t)a;
        }
    }
    for (int i = 1; i < n; i++) {
        if (ct == CANVAS_COLOR_F16) {
            mip_halve_f16(dst + lv[i - 1].off, lv[i - 1].w, lv[i - 1].h,
                          dst + lv[i].off, lv[i].w, lv[i].h);
        } else {
            cnvs_mip_halve(dst + lv[i - 1].off, lv[i - 1].w, lv[i - 1].h,
                           dst + lv[i].off, lv[i].w, lv[i].h);
        }
    }
}

// The per-draw chain for a borrowed bitmap source, built into the canvas's
// reused scratch: a borrowed buffer has no identity to cache a pyramid
// against (a canvas_image does -- canvas_image_build_mips).  Returns the
// count built, or 0 when the scratch can't grow -- the caller falls back to
// bilinear, best-effort like the other OOM paths.
static int build_src_mips(struct canvas *__single cv,
                          uint8_t const *__counted_by(slen) src, int slen,
                          int sw, int sh, int need,
                          enum canvas_color_type ct, bool premul,
                          mip_level *__counted_by(MIP_MAX_LEVELS) lv) {
    int total = 0;
    int const n = plan_mips(sw, sh, need, px_bpp(ct), lv, &total);
    if (!ensure_mips(cv, total)) {
        return 0;
    }
    fill_mips(cv->mips, cv->mips_cap, src, slen, sw, sh, ct, premul, lv, n);
    return n;
}

// --- reified images ------------------------------------------------------------
//
// A canvas_image is a thing you draw FROM; the canvas is the surface you draw
// TO; both are bitmaps (RGBA8 memory) underneath.  Reifying the pixels gives
// them identity, which is what lets derived data live with them:
// canvas_image_build_mips caches the premultiplied pyramid the bitmap entry
// points must otherwise rebuild per minifying draw.
struct canvas_image {
    int w, h;
    enum canvas_color_type ct;  // unorm8 or f16 channels...
    enum canvas_alpha_type at;  // ...straight or premultiplied -- all four
                                // format combinations are peers
    uint8_t *__counted_by(len) px;  // raw bytes, w * h * px_bpp(ct)
    int len;
    // The explicit mip chain (canvas_image_build_mips): fill_mips' layout in
    // one buffer, each level a self-slice in lv.  nlevels == 0 until built.
    uint8_t *__counted_by(mips_len) mips;
    int mips_len;
    cnvs_mip lv[MIP_MAX_LEVELS];
    int nlevels;
};

// The shared constructor body: copy `len` bytes of pixels into a fresh image
// of the given format.  NULL on bad dims or OOM.
static struct canvas_image *__single image_make(
        uint8_t const *__counted_by(len) px, int len, int w, int h,
        enum canvas_color_type ct, enum canvas_alpha_type at) {
    if (!rgba8_dims_ok(w, h) || len != w * h * px_bpp(ct)) {
        return NULL;
    }
    struct canvas_image *img = calloc(1, sizeof *img);
    if (!img) {
        return NULL;
    }
    int const n = len;  // a counted local cannot bind a parameter's count
    uint8_t *__counted_by_or_null(n) copy = malloc((size_t)n);
    if (!copy) {
        free(img);
        return NULL;
    }
    memcpy(copy, px, (size_t)n);
    img->w = w;
    img->h = h;
    img->ct = ct;
    img->at = at;
    img->len = len;
    img->px = copy;
    return img;
}

struct canvas_image *__single canvas_image_unorm8(
        uint8_t const *__counted_by(w * h * 4) px, int w, int h,
        enum canvas_alpha_type at) {
    if (!rgba8_dims_ok(w, h)) {
        return NULL;
    }
    return image_make(px, w * h * 4, w, h, CANVAS_COLOR_UNORM8, at);
}

struct canvas_image *__single canvas_image_f16(
        _Float16 const *__counted_by(w * h * 4) px, int w, int h,
        enum canvas_alpha_type at) {
    if (!rgba8_dims_ok(w, h)) {
        return NULL;
    }
    int const len = w * h * 4 * (int)sizeof(_Float16);
    uint8_t const *bytes = (uint8_t const *)px;
    return image_make(bytes, len, w, h, CANVAS_COLOR_F16, at);
}

struct canvas_image *__single canvas_snapshot(struct canvas *__single cv) {
    int const w = cv->width, h = cv->height;
    if (!rgba8_dims_ok(w, h)) {
        return NULL;
    }
    // The surface is premultiplied f16 and so is the snapshot: one memcpy,
    // bit-lossless -- THE fast path, no quantize, no unpremultiply.
    uint8_t const *bytes = (uint8_t const *)cv->target;
    return image_make(bytes, w * h * 8, w, h,
                      CANVAS_COLOR_F16, CANVAS_ALPHA_PREMUL);
}

bool canvas_image_build_mips(struct canvas_image *__single img) {
    if (img->nlevels > 0) {
        return true;  // idempotent
    }
    int const bpp = px_bpp(img->ct);
    mip_level plan[MIP_MAX_LEVELS];
    int total = 0;
    int const n = plan_mips(img->w, img->h, MIP_MAX_LEVELS, bpp, plan, &total);
    int const cap = total;
    uint8_t *__counted_by_or_null(cap) chain = malloc((size_t)cap);
    if (!chain) {
        return false;  // the image stays valid, mip-less
    }
    fill_mips(chain, cap, img->px, img->len, img->w, img->h, img->ct,
              img->at == CANVAS_ALPHA_PREMUL, plan, n);
    img->mips_len = cap;
    img->mips = chain;
    for (int i = 0; i < n; i++) {
        img->lv[i] = (cnvs_mip){ .px = img->mips + plan[i].off,
                                 .len = plan[i].w * plan[i].h * bpp,
                                 .w = plan[i].w, .h = plan[i].h };
    }
    img->nlevels = n;
    return true;
}

int canvas_image_width(struct canvas_image const *__single img) {
    return img->w;
}

int canvas_image_height(struct canvas_image const *__single img) {
    return img->h;
}

void canvas_image_free(struct canvas_image *__single img) {
    if (!img) {
        return;
    }
    free(img->px);
    free(img->mips);
    free(img);
}

void canvas_set_image_smoothing_enabled(struct canvas *__single cv, bool enabled) {
    if (cv->rec) { cnvs_rec_floats_bool(cv->rec, "set_image_smoothing_enabled", NULL, 0, enabled); }
    cv->cur.image_smoothing_enabled = enabled;
}

void canvas_set_image_smoothing_quality(struct canvas *__single cv,
                                        enum canvas_image_smoothing_quality quality) {
    switch (quality) {
        case CANVAS_SMOOTHING_LOW:
        case CANVAS_SMOOTHING_MEDIUM:
        case CANVAS_SMOOTHING_HIGH:
            if (cv->rec) { cnvs_rec_smoothing_quality(cv->rec, quality); }
            cv->cur.image_smoothing_quality = quality;
            break;
    }
}

// The shared back half of drawImage and the emoji-capture draw: map the
// user-space dest rect through the CTM to a device quad, rasterize its
// coverage, cast the shadow, then sample the source per device pixel through
// the inverse transform.  `premul_src` says how to read the source bytes:
// false = straight (unpremultiplied) alpha, the public drawImage contract;
// true = premultiplied, the canonical emoji capture -- bilinear interpolation
// of premultiplied bytes is fringe-free, and the sample just scales by
// alpha * coverage on its way into the premultiplied tile.  `hi`/`lo`/`lt`
// are a caller-picked trilinear level pair + blend (the emoji captures' cached
// pyramid, where `src` is `hi`'s own bytes); zero views + 0 for the public
// path, which picks its own pair from the source's derived chain below.
//
// LINEAR-WORKING-SPACE DEFERRAL: image/bitmap source pixels are untagged
// encoded sRGB, so on a CANVAS_WS_LINEAR canvas they ought to decode
// sRGB->linear before sampling -- AND the mip box-halving and the bilinear/
// cubic taps ought to average in linear, not sRGB, to be correct.  That is a
// pyramid-and-sampler-wide change (every sample_src* and the mip builder), out
// of this phase's scope; the entry decode lands here when the image pipeline is
// taken up.  Today an image drawn onto a linear canvas samples its bytes as if
// they were already linear -- visibly wrong only for translucent overlaps of
// imagery, untouched on an sRGB canvas (the gallery's linear demonstrator uses
// putImageData, which DOES decode, for its sRGB half).
static void draw_image_quad(struct canvas *__single cv,
                            uint8_t const *__counted_by(slen) src, int slen,
                            int sw, int sh, float sx, float sy,
                            float sww, float shh, float dx, float dy,
                            float dw, float dh, bool premul_src,
                            enum canvas_color_type src_ct,
                            bool quality_tiers, bool chain_on_demand,
                            struct canvas_image const *__single img,
                            cnvs_mip hi, cnvs_mip lo, float lt) {
    if (!rgba8_dims_ok(sw, sh) || slen < sw * sh * px_bpp(src_ct) ||
        dw <= 0.0f || dh <= 0.0f) {
        return;
    }
    // The dest rect transforms to a (possibly rotated) device-space quad.
    cnvs_vec2 q[4] = { xf(cv, dx, dy), xf(cv, dx + dw, dy),
                       xf(cv, dx + dw, dy + dh), xf(cv, dx, dy + dh) };
    cbbox const b = points_bbox(cv, q, 4, filter_margin(cv));
    if (b.w <= 0 || b.h <= 0 || !ensure_tile(cv, b.w * b.h) ||
        !cnvs_cover_reset(&cv->cover, b.w, b.h)) {
        return;
    }
    cover_edge(cv, b, q[0], q[1]);
    cover_edge(cv, b, q[1], q[2]);
    cover_edge(cv, b, q[2], q[3]);
    cover_edge(cv, b, q[3], q[0]);
    cnvs_cover_resolve(&cv->cover, b.w, b.h, CNVS_NONZERO, cv->cov);

    // Blocks of eight pixels, the paint_tile_pattern shape: the SAMPLING stays
    // scalar per lane -- data-dependent taps at arbitrary source coords --
    // with the fold + premultiply around it run as planes: the sampled colours
    // are born f32, sample alpha x global alpha folds there, narrows once,
    // and the coverage fold finishes in f16.  Zero-coverage lanes skip their
    // sample and fold to transparent black.  The premultiplied-data arm
    // (emoji captures, mip and cubic samples) has no premultiply: every
    // channel narrows and scales by ga x coverage in f16.
    cnvs_mat const inv = cnvs_mat_invert(cv->cur.ctm);  // device -> user
    float const ga = cv->cur.global_alpha;
    half8 const gah = (half8)(_Float16)ga;
    bool const fold = shade_folds_coverage(cv);
    bool const smooth = cv->cur.image_smoothing_enabled;
    // Sampler tier.  imageSmoothingQuality is live for the quality_tiers
    // paths (bitmaps, reified images, replayed blocks): a minifying draw
    // (source footprint past one source px per device px) at medium/high
    // samples the premultiplied mip chain with trilinear filtering -- an
    // image's cached chain when built, the per-draw scratch rebuild when
    // chain_on_demand (bitmaps), plain bilinear otherwise (a mip-less image's
    // documented fallback: canvas_image_build_mips is the explicit opt-in) --
    // and a magnifying draw at high runs the 4x4 BC-spline (CUBIC_B/CUBIC_C).
    // The emoji-capture path arrives with its level pair already picked from
    // the text cache's pyramid (cnvs_glyph_mip_pair, the same doubling rule)
    // and joins at SAMP_TRILINEAR; a zero blend there is just bilinear on
    // `src`, which IS the finer level.  The footprint is one Jacobian for the
    // whole quad (the CTM is affine), the emoji rule's max mapped axis.
    enum { SAMP_NEAREST, SAMP_BILINEAR, SAMP_TRILINEAR, SAMP_CUBIC } samp =
        smooth ? SAMP_BILINEAR : SAMP_NEAREST;
    if (samp == SAMP_BILINEAR && premul_src && lt > 0.0f && lo.px) {
        samp = SAMP_TRILINEAR;
    }
    if (samp == SAMP_BILINEAR && quality_tiers &&
        cv->cur.image_smoothing_quality != CANVAS_SMOOTHING_LOW) {
        float const kx = sww / dw, ky = shh / dh;  // source px per user px
        float const ex = hypotf(inv.a * kx, inv.b * ky);
        float const ey = hypotf(inv.c * kx, inv.d * ky);
        float const f = ex > ey ? ex : ey;
        if (f > 1.0f) {
            // The level pair around the footprint: doubling finds the floor
            // level, exact float arithmetic finds the blend -- no log2f, so
            // replay cannot drift across libms.
            int need = 0;
            float scale = 1.0f;
            while (scale * 2.0f <= f && need < MIP_MAX_LEVELS - 2) {
                scale *= 2.0f;
                need++;
            }
            bool picked = false;
            if (img && img->nlevels > 0) {
                int const n = img->nlevels;
                hi = img->lv[need < n ? need : n - 1];
                lo = img->lv[need + 1 < n ? need + 1 : n - 1];
                picked = true;
            } else if (!img && chain_on_demand) {
                mip_level lv[MIP_MAX_LEVELS];
                int const n = build_src_mips(cv, src, slen, sw, sh, need + 1,
                                             src_ct, premul_src, lv);
                if (n > 0) {
                    mip_level const la = lv[need < n ? need : n - 1];
                    mip_level const lb = lv[need + 1 < n ? need + 1 : n - 1];
                    int const bpp = px_bpp(src_ct);
                    hi = (cnvs_mip){ .px = cv->mips + la.off,
                                     .len = la.w * la.h * bpp,
                                     .w = la.w, .h = la.h };
                    lo = (cnvs_mip){ .px = cv->mips + lb.off,
                                     .len = lb.w * lb.h * bpp,
                                     .w = lb.w, .h = lb.h };
                    picked = true;
                }
            }
            if (picked) {
                lt = hi.px == lo.px ? 0.0f : (f - scale) / scale;
                lt = lt < 0.0f ? 0.0f : (lt > 1.0f ? 1.0f : lt);
                samp = SAMP_TRILINEAR;
            }
        } else if (f < 1.0f &&
                   cv->cur.image_smoothing_quality == CANVAS_SMOOTHING_HIGH) {
            samp = SAMP_CUBIC;
        }
    }
    // Each mip level is the whole source at proportionally shrunk dims; one
    // coordinate scale per level maps the source coords onto it.  (For the
    // emoji pair, hi is the source frame itself, so its scale is exactly 1.)
    float const lax = (float)hi.w / (float)sw, lay = (float)hi.h / (float)sh;
    float const lbx = (float)lo.w / (float)sw, lby = (float)lo.h / (float)sh;
    bool const premul_data =
        premul_src || samp == SAMP_TRILINEAR || samp == SAMP_CUBIC;
    float8 const lane = { 0, 1, 2, 3, 4, 5, 6, 7 };
    for (int py = 0; py < b.h; py++) {
        float const devy = (float)b.y + (float)py + 0.5f;
        for (int px = 0; px < b.w; px += 8) {
            int const i = py * b.w + px;
            int const k = b.w - px < 8 ? b.w - px : 8;
            // Device pixel centre -> user space -> dest-rect uv -> source
            // coords, eight lanes at once (elementwise; the scalar expression
            // per lane, bit for bit).  The pixel-centre x sums are
            // integer-exact f32, so the grouping can't differ from the
            // scalar (float)b.x + (float)(px+l) + 0.5f.
            float8 const xs = (float)b.x + ((float)px + lane) + 0.5f;
            foldv8 const u = mat_apply8(inv, xs, devy);
            float8 const fsx = sx + ((u.x - dx) / dw) * sww;
            float8 const fsy = sy + ((u.y - dy) / dh) * shh;
            float8 sr = (float8)0.0f, sg = (float8)0.0f, sb = (float8)0.0f,
                   sa = (float8)0.0f;
            for (int l = 0; l < k; l++) {
                if (cv->cov[i + l] == 0) {  // a zero-coverage lane skips its taps
                    continue;
                }
                float s[4];
                bool const f16 = src_ct == CANVAS_COLOR_F16;
                switch (samp) {
                    case SAMP_NEAREST:
                        (f16 ? sample_src_nearest_f16 : sample_src_nearest)(
                            src, slen, sw, sh, fsx[l], fsy[l], s);
                        break;
                    case SAMP_BILINEAR:
                        (f16 ? sample_src_f16 : sample_src)(
                            src, slen, sw, sh, fsx[l], fsy[l], s);
                        break;
                    case SAMP_CUBIC:
                        (f16 ? sample_src_cubic_f16 : sample_src_cubic)(
                            src, slen, sw, sh, fsx[l], fsy[l], premul_src, s);
                        break;
                    case SAMP_TRILINEAR:
                        (f16 ? sample_src_f16 : sample_src)(
                            hi.px, hi.len, hi.w, hi.h,
                            fsx[l] * lax, fsy[l] * lay, s);
                        if (lt > 0.0f) {
                            float s2[4];
                            (f16 ? sample_src_f16 : sample_src)(
                                lo.px, lo.len, lo.w, lo.h,
                                fsx[l] * lbx, fsy[l] * lby, s2);
                            for (int c = 0; c < 4; c++) {
                                s[c] += (s2[c] - s[c]) * lt;
                            }
                        }
                        break;
                }
                sr[l] = s[0];
                sg[l] = s[1];
                sb[l] = s[2];
                sa[l] = s[3];
            }
            half8 const covh = fold ? (k < 8 ? cover8_k(cv->cov + i, k)
                                             : cover8(cv->cov + i))
                                    : (half8)(_Float16)1.0f;  // unused when !fold
            cnvs_px8 out;
            if (premul_data) {
                half8 const m = fold ? gah * covh : gah;
                out = (cnvs_px8){ __builtin_convertvector(sr, half8) * m,
                                  __builtin_convertvector(sg, half8) * m,
                                  __builtin_convertvector(sb, half8) * m,
                                  __builtin_convertvector(sa, half8) * m };
            } else {
                half8 alpha = __builtin_convertvector(sa * ga, half8);
                if (fold) {
                    alpha = alpha * covh;
                }
                out = shade8(__builtin_convertvector(sr, half8),
                             __builtin_convertvector(sg, half8),
                             __builtin_convertvector(sb, half8),
                             alpha);
            }
            if (k < 8) {
                cnvs_px8_store_k(cv->tile + i, k, out);
            } else {
                cnvs_px8_store(cv->tile + i, out);
            }
        }
    }
    blend_tile(cv, b, fold);
}

void canvas_draw_bitmap_subrect(struct canvas *__single cv,
                               uint8_t const *__counted_by(sw * sh * 4) src,
                               int sw, int sh, float sx, float sy,
                               float sww, float shh, float dx, float dy,
                               float dw, float dh) {
    if (!rgba8_dims_ok(sw, sh)) {
        return;
    }
    if (cv->rec) {
        // The source's dims ride the image block; the op line carries the two
        // user-space rects.  Suspended when draw_image/draw_image_scaled is
        // the op the caller actually issued (they record as themselves).
        int const id = cnvs_rec_image(cv->rec, src, sw * sh * 4, sw, sh,
                                      CANVAS_COLOR_UNORM8, CANVAS_ALPHA_UNPREMUL);
        if (id >= 0) {
            cnvs_rec_image_mips(cv->rec, id);  // bitmap draws: chain on demand
            cnvs_rec_image_floats(cv->rec, "draw_image_subrect", id,
                                  (float[]){ sx, sy, sww, shh, dx, dy, dw, dh },
                                  8);
        }
    }
    draw_image_quad(cv, src, sw * sh * 4, sw, sh, sx, sy, sww, shh,
                    dx, dy, dw, dh, false, CANVAS_COLOR_UNORM8, true, true,
                    NULL,
                    (cnvs_mip){ .px = NULL, .len = 0, .w = 0, .h = 0 },
                    (cnvs_mip){ .px = NULL, .len = 0, .w = 0, .h = 0 }, 0.0f);
}

void canvas_draw_bitmap(struct canvas *__single cv,
                       uint8_t const *__counted_by(sw * sh * 4) src,
                       int sw, int sh, float dx, float dy) {
    // Record `draw_image` as itself, then swallow the subrect form it
    // delegates to.  rgba8_dims_ok gates the w*h*4 the block needs (the same
    // predicate the delegate applies before painting).
    if (cv->rec && rgba8_dims_ok(sw, sh)) {
        int const id = cnvs_rec_image(cv->rec, src, sw * sh * 4, sw, sh,
                                      CANVAS_COLOR_UNORM8, CANVAS_ALPHA_UNPREMUL);
        if (id >= 0) {
            cnvs_rec_image_mips(cv->rec, id);  // bitmap draws: chain on demand
            cnvs_rec_image_floats(cv->rec, "draw_image", id,
                                  (float[]){ dx, dy }, 2);
        }
    }
    cnvs_rec_enter(cv->rec);
    canvas_draw_bitmap_subrect(cv, src, sw, sh, 0.0f, 0.0f, (float)sw, (float)sh,
                              dx, dy, (float)sw, (float)sh);
    cnvs_rec_leave(cv->rec);
}

void canvas_draw_bitmap_scaled(struct canvas *__single cv,
                              uint8_t const *__counted_by(sw * sh * 4) src,
                              int sw, int sh, float dx, float dy,
                              float dw, float dh) {
    if (cv->rec && rgba8_dims_ok(sw, sh)) {
        int const id = cnvs_rec_image(cv->rec, src, sw * sh * 4, sw, sh,
                                      CANVAS_COLOR_UNORM8, CANVAS_ALPHA_UNPREMUL);
        if (id >= 0) {
            cnvs_rec_image_mips(cv->rec, id);  // bitmap draws: chain on demand
            cnvs_rec_image_floats(cv->rec, "draw_image_scaled", id,
                                  (float[]){ dx, dy, dw, dh }, 4);
        }
    }
    cnvs_rec_enter(cv->rec);
    canvas_draw_bitmap_subrect(cv, src, sw, sh, 0.0f, 0.0f, (float)sw, (float)sh,
                              dx, dy, dw, dh);
    cnvs_rec_leave(cv->rec);
}

// The reified-image draw trio.  Each records the image's pixels as an image
// block naming its format (deduplicated -- the object is a natural key, but
// content-dedup already covers it) plus the same draw_image op lines the
// bitmap trio writes: the format speaks in images either way.  An
// `image_mips` line rides along only once the image's chain is built, so a
// mip-less image's bilinear-fallback draws replay faithfully.
static int rec_image_obj(struct canvas *__single cv,
                         struct canvas_image const *__single img) {
    int const id = cnvs_rec_image(cv->rec, img->px, img->len, img->w, img->h,
                                  img->ct, img->at);
    if (id >= 0 && img->nlevels > 0) {
        cnvs_rec_image_mips(cv->rec, id);
    }
    return id;
}

void canvas_draw_image_subrect(struct canvas *__single cv,
                               struct canvas_image const *__single img,
                               float sx, float sy, float sww, float shh,
                               float dx, float dy, float dw, float dh) {
    if (cv->rec) {
        int const id = rec_image_obj(cv, img);
        if (id >= 0) {
            cnvs_rec_image_floats(cv->rec, "draw_image_subrect", id,
                                  (float[]){ sx, sy, sww, shh, dx, dy, dw, dh },
                                  8);
        }
    }
    draw_image_quad(cv, img->px, img->len, img->w, img->h, sx, sy, sww, shh,
                    dx, dy, dw, dh, img->at == CANVAS_ALPHA_PREMUL, img->ct,
                    true, false, img,
                    (cnvs_mip){ .px = NULL, .len = 0, .w = 0, .h = 0 },
                    (cnvs_mip){ .px = NULL, .len = 0, .w = 0, .h = 0 }, 0.0f);
}

void canvas_draw_image(struct canvas *__single cv,
                       struct canvas_image const *__single img,
                       float dx, float dy) {
    if (cv->rec) {
        int const id = rec_image_obj(cv, img);
        if (id >= 0) {
            cnvs_rec_image_floats(cv->rec, "draw_image", id,
                                  (float[]){ dx, dy }, 2);
        }
    }
    cnvs_rec_enter(cv->rec);
    canvas_draw_image_subrect(cv, img, 0.0f, 0.0f, (float)img->w,
                              (float)img->h, dx, dy, (float)img->w,
                              (float)img->h);
    cnvs_rec_leave(cv->rec);
}

void canvas_draw_image_scaled(struct canvas *__single cv,
                              struct canvas_image const *__single img,
                              float dx, float dy, float dw, float dh) {
    if (cv->rec) {
        int const id = rec_image_obj(cv, img);
        if (id >= 0) {
            cnvs_rec_image_floats(cv->rec, "draw_image_scaled", id,
                                  (float[]){ dx, dy, dw, dh }, 4);
        }
    }
    cnvs_rec_enter(cv->rec);
    canvas_draw_image_subrect(cv, img, 0.0f, 0.0f, (float)img->w,
                              (float)img->h, dx, dy, dw, dh);
    cnvs_rec_leave(cv->rec);
}

// Replay's draw of one image block (cnvs_replay.h): ct/at are the block's
// format as named on its line, mips whether the block's draws carry
// mip-chain semantics (an `image_mips` line) -- per-draw rebuild here, byte-
// identical to a cached chain.  Re-records in the replayed op's own spelling
// (`form`) when replaying onto a recording canvas, so the round trip is
// byte-idempotent.
void cnvs_canvas_draw_block(struct canvas *__single cv,
                            uint8_t const *__counted_by(slen) px, int slen,
                            int w, int h, enum canvas_color_type ct,
                            enum canvas_alpha_type at, bool mips, int form,
                            float sx, float sy, float sww, float shh,
                            float dx, float dy, float dw, float dh) {
    if (!rgba8_dims_ok(w, h) || slen < w * h * px_bpp(ct)) {
        return;
    }
    if (cv->rec) {
        int const id = cnvs_rec_image(cv->rec, px, w * h * px_bpp(ct), w, h,
                                      ct, at);
        if (id >= 0) {
            if (mips) {
                cnvs_rec_image_mips(cv->rec, id);
            }
            switch (form) {
                case 0:
                    cnvs_rec_image_floats(cv->rec, "draw_image", id,
                                          (float[]){ dx, dy }, 2);
                    break;
                case 1:
                    cnvs_rec_image_floats(cv->rec, "draw_image_scaled", id,
                                          (float[]){ dx, dy, dw, dh }, 4);
                    break;
                default:
                    cnvs_rec_image_floats(cv->rec, "draw_image_subrect", id,
                                          (float[]){ sx, sy, sww, shh,
                                                     dx, dy, dw, dh }, 8);
                    break;
            }
        }
    }
    draw_image_quad(cv, px, slen, w, h, sx, sy, sww, shh, dx, dy, dw, dh,
                    at == CANVAS_ALPHA_PREMUL, ct, true, mips, NULL,
                    (cnvs_mip){ .px = NULL, .len = 0, .w = 0, .h = 0 },
                    (cnvs_mip){ .px = NULL, .len = 0, .w = 0, .h = 0 }, 0.0f);
}

// One planar slab's un-premultiply and 8-bit quantize, in _Float16: the
// divide, clamp, and 255-scale with no f32 anywhere (docs/decisions/
// color-axis.md).  A fully transparent lane (a <= 0) un-premultiplies to all
// zero -- selected bitwise BEFORE the byte convert, so the masked divide's
// inf/NaN lanes never reach the (undefined for them) float->int conversion.
// Every 8-bit edge value still quantizes back exactly (test_image's
// exhaustive round-trip).  Returns finished byte values in [0.5, 255.5) for
// the truncating store seam (cnvs_px8_store_rgba8).
static cnvs_px8 unpremul_to_unorm8(cnvs_px8 p) {
    half8 const zero = (half8)(_Float16)0.0f, one = (half8)(_Float16)1.0f;
    short8 const opaque = p.a > zero;
    cnvs_px8 u = { p.r / p.a, p.g / p.a, p.b / p.a, p.a };
    u.r = half8_if_then_else(opaque, __builtin_elementwise_min(one,
                        __builtin_elementwise_max(zero, u.r)), zero);
    u.g = half8_if_then_else(opaque, __builtin_elementwise_min(one,
                        __builtin_elementwise_max(zero, u.g)), zero);
    u.b = half8_if_then_else(opaque, __builtin_elementwise_min(one,
                        __builtin_elementwise_max(zero, u.b)), zero);
    u.a = half8_if_then_else(opaque, __builtin_elementwise_min(one,
                        __builtin_elementwise_max(zero, u.a)), zero);
    _Float16 const bias = (_Float16)0.5f, k255 = (_Float16)255.0f;
    return (cnvs_px8){ u.r * k255 + bias, u.g * k255 + bias,
                       u.b * k255 + bias, u.a * k255 + bias };
}

// The LINEAR canvas readback: the same un-premultiply and 8-bit quantize, but
// with a linear->sRGB encode (cnvs_linear_to_srgb) inserted between the divide
// and the clamp -- the one and only clamp in the linear pipeline lives in this
// exit, and the transfer must run BEFORE it so an extended linear value collapses
// to its encoded byte rather than being crushed to 1.0 first.  Scalar per lane:
// the encode is a precision-sensitive f32 pow (cnvs_color.c's deferral note),
// with no half8 spelling in libm; the readback hot path is the sRGB-canvas
// SIMD function above, never this.  Alpha takes no transfer (coverage, not
// colour).  An out-of-[0,1] alpha still clamps, like the planar path.
static cnvs_px8 unpremul_encode_to_unorm8(cnvs_px8 p) {
    cnvs_px8 o = { (half8)(_Float16)0.0f, (half8)(_Float16)0.0f,
                   (half8)(_Float16)0.0f, (half8)(_Float16)0.0f };
    for (int i = 0; i < 8; i++) {
        float const a = (float)p.a[i];
        float r = 0.0f, g = 0.0f, b = 0.0f, ca = 0.0f;
        if (a > 0.0f) {  // a transparent lane stays all-zero (the planar mask's twin)
            float const inv = 1.0f / a;
            r  = cnvs_clamp01(cnvs_linear_to_srgb((float)p.r[i] * inv));
            g  = cnvs_clamp01(cnvs_linear_to_srgb((float)p.g[i] * inv));
            b  = cnvs_clamp01(cnvs_linear_to_srgb((float)p.b[i] * inv));
            ca = cnvs_clamp01(a);
        }
        o.r[i] = (_Float16)(r  * 255.0f + 0.5f);
        o.g[i] = (_Float16)(g  * 255.0f + 0.5f);
        o.b[i] = (_Float16)(b  * 255.0f + 0.5f);
        o.a[i] = (_Float16)(ca * 255.0f + 0.5f);
    }
    return o;
}

// Per-canvas readback: the sRGB SIMD bypass, or the linear scalar encode.
static cnvs_px8 read_unorm8(struct canvas *__single cv, cnvs_px8 p) {
    return cv->space == CANVAS_WS_LINEAR ? unpremul_encode_to_unorm8(p)
                                         : unpremul_to_unorm8(p);
}

// Read the canvas back as unpremultiplied RGBA8, straight off the
// premultiplied target: the un-premultiply and 8-bit quantize happen here,
// eight pixels per step over channel planes with st4 re-interleaving at the
// RGBA8 seam (cnvs_planar.h); the n%8 tail runs the same slab gathered.  An
// sRGB canvas is the SIMD bypass (byte-identical); a linear canvas encodes
// linear->sRGB at this exit (read_unorm8).  write_png and get_image_data read
// back through this same entry point.
void canvas_read_rgba(struct canvas *__single cv, uint8_t *__counted_by(len) out, int len) {
    if (len < cv->width * cv->height * 4) {
        return;
    }
    int const n = cv->target_len;
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        cnvs_px8_store_rgba8(out + i * 4,
                             read_unorm8(cv, cnvs_px8_load(cv->target + i)));
    }
    if (i < n) {
        int const k = n - i;
        cnvs_px8_store_rgba8_k(out + i * 4, k,
                               read_unorm8(cv, cnvs_px8_load_k(cv->target + i, k)));
    }
}

bool canvas_write_png(struct canvas *__single cv, char const *__null_terminated path) {
    int const len = cv->width * cv->height * 4;
    uint8_t *__counted_by_or_null(len) out = malloc((size_t)len);
    if (!out) {
        return false;
    }
    canvas_read_rgba(cv, out, len);
    bool const ok = cnvs_png_write(path, out, cv->width, cv->height);
    free(out);
    return ok;
}

uint8_t *__counted_by_or_null(*len)
canvas_read_png(char const *__null_terminated path,
                int *__single w, int *__single h, int *__single len) {
    return cnvs_png_read(path, w, h, len);
}

void canvas_get_image_data(struct canvas *__single cv, int x, int y, int w, int h,
                           uint8_t *__counted_by(len) out, int len) {
    if (!rgba8_dims_ok(w, h) || len < w * h * 4) {
        return;
    }
    memset(out, 0, (size_t)len);  // pixels outside the canvas stay transparent
    int const clen = cv->width * cv->height * 4;
    uint8_t *__counted_by_or_null(clen) buf = malloc((size_t)clen);
    if (!buf) {
        return;
    }
    canvas_read_rgba(cv, buf, clen);
    cnvs_blit_rgba(out, w, h, 0, 0, buf, cv->width, cv->height, x, y, w, h);
    free(buf);
}

uint8_t *__counted_by_or_null(*len)
canvas_create_image_data(int sw, int sh, int *__single len) {
    if (!rgba8_dims_ok(sw, sh)) {
        *len = 0;
        return NULL;
    }
    // rgba8_dims_ok guarantees sw*sh*4 fits a positive int, so this is overflow-free.
    int const n = sw * sh * 4;
    uint8_t *buf = calloc((size_t)n, 1);  // zeroed == transparent black
    if (!buf) {
        *len = 0;
        return NULL;
    }
    *len = n;
    return buf;
}

// Decode the RGB planes of a [0,1]-normalized straight slab sRGB->linear, in
// place, leaving alpha alone -- the linear canvas's putImageData entry transfer.
// Scalar per lane (the f32 decode, cnvs_color.c's deferral note); incoming
// putImageData bytes are in [0,1] so the decode stays in [0,1], no extended
// values to carry.  Reached only on cv->space == CANVAS_WS_LINEAR.
static cnvs_px8 px8_decode_rgb(cnvs_px8 p) {
    for (int i = 0; i < 8; i++) {
        p.r[i] = (_Float16)cnvs_srgb_to_linear((float)p.r[i]);
        p.g[i] = (_Float16)cnvs_srgb_to_linear((float)p.g[i]);
        p.b[i] = (_Float16)cnvs_srgb_to_linear((float)p.b[i]);
    }
    return p;
}

// Copy the sub-rectangle [sx, sx+sw) x [sy, sy+sh) of the w-wide RGBA8 source onto
// the canvas with the ImageData origin at (dx, dy): source pixel (col, row) lands
// at (dx+col, dy+row).  Overwrites (no blending) and ignores the clip, clipped to
// the canvas.  The caller guarantees the sub-rect lies within the source
// ([0,w] x [0,h]) with sw, sh > 0, and len >= w*h*4.
static void put_image_sub(struct canvas *__single cv,
                          uint8_t const *__counted_by(len) data, int len,
                          int w, int dx, int dy, int sx, int sy, int sw, int sh) {
    (void)len;
    // Destination rect in canvas space, clamped to the canvas.  64-bit so a wild
    // (dx, dy) can't overflow the clamp arithmetic (the API boundary is untrusted).
    int64_t xs = (int64_t)dx + sx, ys = (int64_t)dy + sy;
    int64_t cx0 = xs < 0 ? 0 : xs, cy0 = ys < 0 ? 0 : ys;
    int64_t cx1 = xs + sw, cy1 = ys + sh;
    if (cx1 > cv->width)  { cx1 = cv->width; }
    if (cy1 > cv->height) { cy1 = cv->height; }
    int const rw = cx1 > cx0 ? (int)(cx1 - cx0) : 0;
    int const rh = cy1 > cy0 ? (int)(cy1 - cy0) : 0;
    if (rw <= 0 || rh <= 0 || !ensure_tile(cv, rw * rh)) {
        return;
    }
    // Source column/row of the first painted canvas pixel; col0+px stays in
    // [sx, sx+sw) ⊆ [0,w) and row0+py in [sy, sy+sh) ⊆ [0,h), so si < w*h*4 <= len.
    // Eight pixels per step: ld4 deinterleaves the RGBA8 source into channel
    // planes (cnvs_planar.h), a true _Float16 divide scales each to [0,1],
    // and the planar premultiply writes finished tile pixels through st4.
    int const col0 = (int)(cx0 - dx);
    int const row0 = (int)(cy0 - dy);
    bool const lin = cv->space == CANVAS_WS_LINEAR;  // decode incoming sRGB->linear
    _Float16 const k255 = (_Float16)255.0f;
    for (int py = 0; py < rh; py++) {
        int px = 0;
        for (; px + 8 <= rw; px += 8) {
            int si = ((row0 + py) * w + (col0 + px)) * 4;
            cnvs_px8 p = cnvs_px8_load_rgba8(data + si);
            p = (cnvs_px8){ p.r / k255, p.g / k255, p.b / k255, p.a / k255 };
            if (lin) { p = px8_decode_rgb(p); }
            cnvs_px8_store(cv->tile + py * rw + px, cnvs_px8_premultiply(p));
        }
        if (px < rw) {
            int const k = rw - px;
            int const si = ((row0 + py) * w + (col0 + px)) * 4;
            cnvs_px8 p = cnvs_px8_load_rgba8_k(data + si, k);
            p = (cnvs_px8){ p.r / k255, p.g / k255, p.b / k255, p.a / k255 };
            if (lin) { p = px8_decode_rgb(p); }
            cnvs_px8_store_k(cv->tile + py * rw + px, k, cnvs_px8_premultiply(p));
        }
    }
    // putImageData overwrites and ignores the clip: composite COPY with no clip.
    cnvs_blend(cv, (int)cx0, (int)cy0, rw, rh, cv->tile, NULL, NULL, 0,
               CANVAS_OP_COPY);
}

void canvas_put_image_data(struct canvas *__single cv,
                           uint8_t const *__counted_by(len) data, int len,
                           int w, int h, int dx, int dy) {
    if (!rgba8_dims_ok(w, h) || len < w * h * 4) {
        return;
    }
    if (cv->rec) {
        // Exactly the w*h*4 pixels the op reads ride the block; the int-typed
        // placement rides the op line.
        int const id = cnvs_rec_image(cv->rec, data, w * h * 4, w, h,
                                      CANVAS_COLOR_UNORM8, CANVAS_ALPHA_UNPREMUL);
        if (id >= 0) {
            cnvs_rec_image_ints(cv->rec, "put_image_data", id,
                                (int[]){ dx, dy }, 2);
        }
    }
    put_image_sub(cv, data, len, w, dx, dy, 0, 0, w, h);
}

void canvas_put_image_data_dirty(struct canvas *__single cv,
                                 uint8_t const *__counted_by(len) data, int len,
                                 int w, int h, int dx, int dy,
                                 int dirty_x, int dirty_y,
                                 int dirty_w, int dirty_h) {
    if (!rgba8_dims_ok(w, h) || len < w * h * 4) {
        return;
    }
    if (cv->rec) {
        // The raw dirty args ride the op line; replay re-normalises them
        // through this very function, so the recorded form stays the call.
        int const id = cnvs_rec_image(cv->rec, data, w * h * 4, w, h,
                                      CANVAS_COLOR_UNORM8, CANVAS_ALPHA_UNPREMUL);
        if (id >= 0) {
            cnvs_rec_image_ints(cv->rec, "put_image_data_dirty", id,
                                (int[]){ dx, dy, dirty_x, dirty_y,
                                         dirty_w, dirty_h }, 6);
        }
    }
    // Normalise the dirty rect (ImageData space) into a sub-rect of [0,w] x [0,h],
    // per the spec: flip negative extents, then clamp to the source bounds.  All
    // in 64-bit so extreme/negative dirty args can't overflow.
    int64_t dxx = dirty_x, dyy = dirty_y, dww = dirty_w, dhh = dirty_h;
    if (dww < 0) { dxx += dww; dww = -dww; }
    if (dhh < 0) { dyy += dhh; dhh = -dhh; }
    if (dxx < 0) { dww += dxx; dxx = 0; }
    if (dyy < 0) { dhh += dyy; dyy = 0; }
    if (dxx + dww > w) { dww = (int64_t)w - dxx; }
    if (dyy + dhh > h) { dhh = (int64_t)h - dyy; }
    if (dww <= 0 || dhh <= 0) {
        return;  // empty (or fully-clipped) dirty rect: nothing to copy
    }
    put_image_sub(cv, data, len, w, dx, dy,
                  (int)dxx, (int)dyy, (int)dww, (int)dhh);
}

// --- Path2D -----------------------------------------------------------------
//
// A Path2D records its commands in user space and replays them through the
// canvas's path methods (reusing all the curve/arc/rounding logic) into a fresh
// device-space path at draw time, so it honours the current transform without
// disturbing the canvas's own current path.  The command-list storage lives in
// cnvs_path2d.h so the recorder can serialize a path as a `path` block.

struct canvas_path2d *__single canvas_path2d(void) {
    return calloc(1, sizeof(struct canvas_path2d));  // cmds=NULL, ncmds=cap=0 (consistent)
}

void canvas_path2d_free(struct canvas_path2d *__single p) {
    if (!p) {
        return;
    }
    free(p->cmds);
    free(p);
}

static void p2d_push(struct canvas_path2d *__single p, p2d_cmd c) {
    if (p->ncmds >= p->cap) {
        int const nc = cnvs_grow_cap(p->cap, p->ncmds + 1);
        p2d_cmd *ncmds = realloc(p->cmds, (size_t)nc * sizeof *ncmds);
        if (!ncmds) {
            return;  // OOM: drop the command (best-effort, matches the path builders)
        }
        p->cmds = ncmds;
        p->cap = nc;
    }
    p->cmds[p->ncmds] = c;
    p->ncmds += 1;
}

void canvas_path2d_move_to(struct canvas_path2d *__single p, float x, float y) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_MOVE, .a = { x, y } });
}

void canvas_path2d_line_to(struct canvas_path2d *__single p, float x, float y) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_LINE, .a = { x, y } });
}

void canvas_path2d_quadratic_curve_to(struct canvas_path2d *__single p,
                                      float cpx, float cpy, float x, float y) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_QUAD, .a = { cpx, cpy, x, y } });
}

void canvas_path2d_bezier_curve_to(struct canvas_path2d *__single p, float c1x, float c1y,
                                   float c2x, float c2y, float x, float y) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_CUBIC, .a = { c1x, c1y, c2x, c2y, x, y } });
}

void canvas_path2d_arc(struct canvas_path2d *__single p, float x, float y, float radius,
                       float start_angle, float end_angle, bool anticlockwise) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_ARC,
                           .a = { x, y, radius, start_angle, end_angle },
                           .ccw = anticlockwise });
}

void canvas_path2d_ellipse(struct canvas_path2d *__single p, float x, float y,
                           float rx, float ry, float rotation,
                           float start_angle, float end_angle, bool anticlockwise) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_ELLIPSE,
                           .a = { x, y, rx, ry, rotation, start_angle, end_angle },
                           .ccw = anticlockwise });
}

void canvas_path2d_arc_to(struct canvas_path2d *__single p, float x1, float y1,
                          float x2, float y2, float radius) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_ARC_TO, .a = { x1, y1, x2, y2, radius } });
}

void canvas_path2d_rect(struct canvas_path2d *__single p, float x, float y,
                        float w, float h) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_RECT, .a = { x, y, w, h } });
}

void canvas_path2d_round_rect(struct canvas_path2d *__single p, float x, float y,
                              float w, float h, float radius) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_ROUND_RECT, .a = { x, y, w, h, radius } });
}

void canvas_path2d_close_path(struct canvas_path2d *__single p) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_CLOSE });
}

void canvas_path2d_add_path(struct canvas_path2d *__single dst,
                            struct canvas_path2d const *__single src) {
    for (int i = 0; i < src->ncmds; i++) {
        p2d_push(dst, src->cmds[i]);
    }
}

// Replay a Path2D's commands into cv->path through the canvas path methods (which
// transform each coordinate by the current CTM and flatten curves at device tol).
static void p2d_replay(struct canvas *__single cv, struct canvas_path2d const *__single p) {
    for (int i = 0; i < p->ncmds; i++) {
        p2d_cmd const c = p->cmds[i];
        float const *a = c.a;
        switch (c.verb) {
            case P2D_MOVE:       canvas_move_to(cv, a[0], a[1]); break;
            case P2D_LINE:       canvas_line_to(cv, a[0], a[1]); break;
            case P2D_QUAD:       canvas_quadratic_curve_to(cv, a[0], a[1], a[2], a[3]); break;
            case P2D_CUBIC:      canvas_bezier_curve_to(cv, a[0], a[1], a[2], a[3],
                                                        a[4], a[5]); break;
            case P2D_ARC:        canvas_arc(cv, a[0], a[1], a[2], a[3], a[4], c.ccw); break;
            case P2D_ELLIPSE:    canvas_ellipse(cv, a[0], a[1], a[2], a[3], a[4],
                                                a[5], a[6], c.ccw); break;
            case P2D_ARC_TO:     canvas_arc_to(cv, a[0], a[1], a[2], a[3], a[4]); break;
            case P2D_RECT:       canvas_rect(cv, a[0], a[1], a[2], a[3]); break;
            case P2D_ROUND_RECT: canvas_round_rect(cv, a[0], a[1], a[2], a[3], a[4]);
                                 break;
            case P2D_CLOSE:      canvas_close_path(cv); break;
        }
    }
}

// Borrow the current-path machinery for a Path2D without disturbing it: swap_in
// copies the current path (and its user-space pen) aside and builds `p` into a
// fresh one in its place; swap_out frees the scratch and restores the original.
// Every Path2D draw and hit-test runs between the two.
struct p2d_scratch {
    struct cnvs_path path;
    cnvs_vec2 user;
};

static void p2d_swap_in(struct canvas *__single cv, struct canvas_path2d const *__single p,
                        struct p2d_scratch *__single sv) {
    sv->path = cv->path;
    sv->user = cv->cur_user;
    cnvs_path_init(&cv->path);
    p2d_replay(cv, p);
}

static void p2d_swap_out(struct canvas *__single cv, struct p2d_scratch *__single sv) {
    cnvs_path_free(&cv->path);
    cv->path = sv->path;
    cv->cur_user = sv->user;
}

void canvas_fill_path(struct canvas *__single cv, struct canvas_path2d const *__single p,
                      enum canvas_fill_rule rule) {
    // Record `fill_path <id> <rule>` against the path's numbered block, then
    // swallow the public path methods p2d_replay drives -- the file keeps the
    // op the caller issued, not the path's expansion into the current path.
    if (cv->rec) {
        int const id = cnvs_rec_path(cv->rec, p);
        if (id >= 0) { cnvs_rec_path_rule(cv->rec, "fill_path", id, rule); }
    }
    cnvs_rec_enter(cv->rec);
    struct p2d_scratch sv;
    p2d_swap_in(cv, p, &sv);
    fill_device_path(cv, &cv->path,
                     rule == CANVAS_EVENODD ? CNVS_EVENODD : CNVS_NONZERO);
    p2d_swap_out(cv, &sv);
    cnvs_rec_leave(cv->rec);
}

void canvas_stroke_path(struct canvas *__single cv, struct canvas_path2d const *__single p) {
    if (cv->rec) {
        int const id = cnvs_rec_path(cv->rec, p);
        if (id >= 0) { cnvs_rec_path_op(cv->rec, "stroke_path", id); }
    }
    cnvs_rec_enter(cv->rec);
    struct p2d_scratch sv;
    p2d_swap_in(cv, p, &sv);
    stroke_device_path(cv, &cv->path);
    p2d_swap_out(cv, &sv);
    cnvs_rec_leave(cv->rec);
}

void canvas_clip_path(struct canvas *__single cv, struct canvas_path2d const *__single p,
                      enum canvas_fill_rule rule) {
    if (cv->rec) {
        int const id = cnvs_rec_path(cv->rec, p);
        if (id >= 0) { cnvs_rec_path_rule(cv->rec, "clip_path", id, rule); }
    }
    // Swallow both p2d_replay's path methods and the nested canvas_clip.
    cnvs_rec_enter(cv->rec);
    struct p2d_scratch sv;
    p2d_swap_in(cv, p, &sv);
    canvas_clip(cv, rule);
    p2d_swap_out(cv, &sv);
    cnvs_rec_leave(cv->rec);
}

bool canvas_is_point_in_path2d(struct canvas *__single cv, struct canvas_path2d const *__single p,
                               float x, float y, enum canvas_fill_rule rule) {
    if (!isfinite(x) || !isfinite(y)) {
        return false;
    }
    cnvs_rec_enter(cv->rec);  // a query: p2d_replay's path methods record nothing
    struct p2d_scratch sv;
    p2d_swap_in(cv, p, &sv);
    bool inside = path_contains(&cv->path, xf(cv, x, y),
                                rule == CANVAS_EVENODD ? CNVS_EVENODD : CNVS_NONZERO);
    p2d_swap_out(cv, &sv);
    cnvs_rec_leave(cv->rec);
    return inside;
}

bool canvas_is_point_in_stroke_path(struct canvas *__single cv,
                                    struct canvas_path2d const *__single p,
                                    float x, float y) {
    if (!isfinite(x) || !isfinite(y)) {
        return false;
    }
    cnvs_rec_enter(cv->rec);  // a query: p2d_replay's path methods record nothing
    struct p2d_scratch sv;
    p2d_swap_in(cv, p, &sv);
    bool inside = build_stroke_verts(cv, &cv->path) &&
                  stroke_verts_contain(cv, xf(cv, x, y));
    p2d_swap_out(cv, &sv);
    cnvs_rec_leave(cv->rec);
    return inside;
}
