#include "canvas.h"

#include "compositor.h"
#include "blur.h"
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
#define CANVAS_MAX_DASH 16

// Cap canvas dimensions so width*height and width*height*4 stay well within a
// positive int -- the whole pipeline's RGBA8 size math is `int`.  Mirrors the
// cnvs_png.c clamp and the Metal max-texture limit (a larger canvas would fail
// texture creation anyway).
#define CANVAS_MAX_DIM 16384

// A caller-supplied image rectangle (get/put_image_data region, drawImage
// source) is honoured only if its RGBA8 byte size fits a positive int: that is
// what makes the `w * h * 4` size arithmetic at the call sites overflow-free.
// (Canvas dims are already bounded by CANVAS_MAX_DIM; these come straight from
// the caller and are otherwise unbounded.)
static bool rgba8_dims_ok(int w, int h) {
    return w > 0 && h > 0 && (int64_t)w * (int64_t)h <= (int64_t)INT_MAX / 4;
}

// Colour components and the global alpha clamp to [0,1] (Canvas spec); NaN -> 0.
// Keeps non-finite/out-of-range paint out of the float->_Float16->uint8 pipeline.
static float clamp01(float v) {
    if (!(v > 0.0f)) {   // <= 0, or NaN
        return 0.0f;
    }
    return v > 1.0f ? 1.0f : v;
}

// One RGBA8 buffer adopted from a replayed `image` block
// (cnvs_canvas_own_image): a singly linked list the canvas frees only at
// canvas_destroy.  Patterns borrow their source, so a replayed program's
// images must outlive replay -- and survive reset(), which restores drawing
// state but does not invalidate the program's blocks mid-replay.
struct cnvs_owned_image {
    struct cnvs_owned_image *__single next;
    uint8_t *__counted_by(len) data;
    int len;
};

// Which paint a fill/stroke uses.  SOLID reads the `fill`/`stroke` colour,
// GRADIENT the `*_grad`, PATTERN the `*_pattern`.
typedef enum {
    CNVS_PAINT_SOLID, CNVS_PAINT_GRADIENT, CNVS_PAINT_PATTERN
} cnvs_paint_kind;

// An image pattern paint.  The source is borrowed (the caller owns it); `len`
// (== w*h*4) bounds it for -fbounds-safety.  `to_pattern` maps a device point to
// pattern-image space (the inverse of the CTM captured when the pattern was set),
// so the pattern is pinned in device space like the gradients.
typedef struct {
    uint8_t const *__counted_by(len) data;
    int len;
    int w, h;
    canvas_pattern_repeat repeat;
    cnvs_mat to_pattern;
} cnvs_pattern;

struct canvas_state {
    cnvs_mat ctm;
    cnvs_unpremul fill;
    cnvs_paint_kind fill_kind;
    cnvs_gradient fill_grad;
    cnvs_pattern fill_pattern;
    cnvs_unpremul stroke;
    cnvs_paint_kind stroke_kind;
    cnvs_gradient stroke_grad;
    cnvs_pattern stroke_pattern;
    float global_alpha;
    compositor_blend_mode composite;  // globalCompositeOperation
    float line_width;
    cnvs_fill_rule fill_rule;
    cnvs_line_join line_join;
    cnvs_line_cap line_cap;
    float miter_limit;
    float dash[CANVAS_MAX_DASH];
    int dash_count;
    float dash_offset;
    float font_size;  // text size in user px (Canvas default 10px)
    canvas_text_align text_align;
    canvas_text_baseline text_baseline;
    canvas_direction direction;  // paragraph direction: resolves start/end and
                                 // is the base direction text shapes under
    bool image_smoothing_enabled;
    canvas_image_smoothing_quality image_smoothing_quality;
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
    compositor *__single comp;
    int width;
    int height;
    struct canvas_state cur;
    struct canvas_state *__counted_by(stack_cap) stack;
    int stack_len;
    int stack_cap;
    cnvs_path path;
    cnvs_path text_path;  // scratch glyph outlines (fill_text/stroke_text)
    cnvs_font *__single font;  // cached for cur.font_size; rebuilt when it changes
    float font_built_size;
    cnvs_text_cache text_cache;  // params->derived-data memo of Core Text results:
                                 // shaped lines + canonical glyph curves, checked
                                 // before the boundary is called (cnvs_text.h)
    cnvs_vec2 cur_user;  // current point in user space (path.cur is device space)
    cnvs_verts scratch_verts;  // stroke triangle output, fed to the coverage rasterizer
    cnvs_cover cover;
    uint8_t *__counted_by(cov_cap) cov;     // per-pixel coverage for the current op's bbox
    int cov_cap;
    cnvs_premul *__counted_by(tile_cap) tile;  // premultiplied tile for the current op's bbox
    int tile_cap;
    float *__counted_by(trow_cap) trow;    // one row of gradient parameters (vectorized solve)
    int trow_cap;
    cnvs_unpremul *__counted_by(crow_cap) crow;  // one row of gradient colours (vectorized stop lerp)
    int crow_cap;
    cnvs_recorder *__single rec;  // NULL unless canvas_record_to is active
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
};

static cnvs_vec2 xf(canvas *__single cv, float x, float y);
static bool ensure_tile(canvas *__single cv, int npix);
static int sigma_box_radius(float sigma);
static int shadow_offset(float v);
static void draw_image_quad(canvas *__single cv,
                            uint8_t const *__counted_by(slen) src, int slen,
                            int sw, int sh, float sx, float sy,
                            float sww, float shh, float dx, float dy,
                            float dw, float dh, bool premul_src);

// Reset a pattern to empty (no source).  Counts first: a NULL pointer must never
// be paired with a positive count under -fbounds-safety.  An empty pattern stays
// consistent across the state copies that save/restore make.
static void pattern_clear(cnvs_pattern *p) {
    p->len = 0;
    p->data = NULL;
    p->w = 0;
    p->h = 0;
    p->repeat = CANVAS_NO_REPEAT;
    p->to_pattern = cnvs_mat_identity();
}

// The initial drawing state (Canvas defaults): identity transform, opaque black
// fill/stroke, source-over, 1px miter strokes, no dash, 10px text, open clip.
// Shared by canvas_create and canvas_reset so the two can't drift.  Assigned
// field by field (not an init list): a compound literal of side-effecting calls
// has indeterminate evaluation order, which -fbounds-safety flags for a struct
// carrying a __counted_by member.  Clearing the gradient scratch isn't needed
// (read only when the kind is GRADIENT), but the patterns must be cleared so the
// borrowed-buffer (data, len) pair stays consistent when the state is copied.
static void state_defaults(struct canvas_state *s) {
    s->ctm = cnvs_mat_identity();
    s->fill = cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 1.0f);
    s->fill_kind = CNVS_PAINT_SOLID;
    pattern_clear(&s->fill_pattern);
    s->stroke = cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 1.0f);
    s->stroke_kind = CNVS_PAINT_SOLID;
    pattern_clear(&s->stroke_pattern);
    s->global_alpha = 1.0f;
    s->composite = COMPOSITOR_SRC_OVER;
    s->line_width = 1.0f;
    s->fill_rule = CNVS_NONZERO;
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

canvas *__single canvas_create(int width, int height) {
    if (width <= 0 || height <= 0 ||
        width > CANVAS_MAX_DIM || height > CANVAS_MAX_DIM) {
        return NULL;
    }
    compositor *__single comp = compositor_create(width, height);
    if (!comp) {
        return NULL;
    }
    canvas *__single cv = calloc(1, sizeof *cv);
    if (!cv) {
        compositor_destroy(comp);
        return NULL;
    }
    cv->comp = comp;
    cv->width = width;
    cv->height = height;
    state_defaults(&cv->cur);
    cv->stack = NULL;
    cv->stack_len = 0;
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

void canvas_destroy(canvas *__single cv) {
    if (!cv) {
        return;
    }
    cnvs_recorder_close(cv->rec);  // flush and close any active recording
    compositor_destroy(cv->comp);
    for (int i = 0; i < cv->stack_len; i++) {
        free(cv->stack[i].filters);
        free(cv->stack[i].clip_mask);
    }
    free(cv->stack);
    free(cv->cur.filters);
    free(cv->cur.clip_mask);
    cnvs_font_destroy(cv->font);
    cnvs_text_cache_clear(&cv->text_cache);  // owned shaped lines + glyph curves
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
    for (struct cnvs_owned_image *__single n = cv->owned_images; n;) {
        struct cnvs_owned_image *__single next = n->next;
        free(n->data);
        free(n);
        n = next;
    }
    free(cv);
}

bool cnvs_canvas_own_image(canvas *__single cv,
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

bool canvas_record_to(canvas *__single cv, char const *__null_terminated path) {
    cnvs_recorder_close(cv->rec);  // stop any prior recording first
    cv->rec = cnvs_recorder_open(path);
    // A new file holds no blocks yet: forget what any prior recording emitted,
    // so warm cache entries serialize afresh into this one.
    cnvs_text_cache_unmark(&cv->text_cache);
    return cv->rec != NULL;
}

bool canvas_is_context_lost(canvas *__single cv) {
    (void)cv;
    return false;  // a headless renderer owns its backing store; never lost.
}

static bool stack_reserve(canvas *__single cv, int need) {
    if (need <= cv->stack_cap) {
        return true;
    }
    int newcap = cnvs_grow_cap(cv->stack_cap, need);
    struct canvas_state *ns =
        realloc(cv->stack, (size_t)newcap * sizeof *ns);
    if (!ns) {
        return false;
    }
    cv->stack = ns;
    cv->stack_cap = newcap;
    return true;
}

void canvas_save(canvas *__single cv) {
    if (cv->rec) { cnvs_rec_op(cv->rec, "save"); }
    if (!stack_reserve(cv, cv->stack_len + 1)) {
        return;
    }
    cv->stack[cv->stack_len] = cv->cur;
    // Give the saved entry its own copy of the clip mask so clip() can mutate
    // cur's independently.
    if (cv->cur.clip_mask) {
        int n = cv->cur.clip_len;
        uint8_t *copy = malloc((size_t)n);
        if (copy) {
            memcpy(copy, cv->cur.clip_mask, (size_t)n);
            cv->stack[cv->stack_len].clip_mask = copy;
            cv->stack[cv->stack_len].clip_len = n;
        } else {
            cv->stack[cv->stack_len].clip_mask = NULL;
            cv->stack[cv->stack_len].clip_len = 0;
        }
    }
    // Likewise the filter list, so add_filter/set_filter_none can mutate cur's
    // independently.  On allocation failure the saved entry keeps no filters
    // (best-effort, matching the clip's degraded copy above).
    if (cv->cur.filters) {
        int n = cv->cur.filter_count;
        cnvs_filter *copy = malloc((size_t)n * sizeof *copy);
        if (copy) {
            memcpy(copy, cv->cur.filters, (size_t)n * sizeof *copy);
            cv->stack[cv->stack_len].filters = copy;
            cv->stack[cv->stack_len].filter_count = n;
        } else {
            cv->stack[cv->stack_len].filters = NULL;
            cv->stack[cv->stack_len].filter_count = 0;
        }
    }
    cv->stack_len += 1;
}

void canvas_restore(canvas *__single cv) {
    if (cv->rec) { cnvs_rec_op(cv->rec, "restore"); }
    if (cv->stack_len > 0) {
        cv->stack_len -= 1;
        free(cv->cur.filters);
        free(cv->cur.clip_mask);
        cv->cur = cv->stack[cv->stack_len];  // adopts the saved clip mask + filters
        compositor_set_clip(cv->comp, cv->cur.clip_mask, cv->cur.clip_len);
    }
}

void canvas_reset(canvas *__single cv) {
    // Recording continues across a reset: the cleared text cache means the
    // file's font-id space restarts with it (later text re-interns from 0 and
    // re-emits its blocks), which replay mirrors when it executes this line.
    if (cv->rec) { cnvs_rec_op(cv->rec, "reset"); }
    // Empty the saved-state stack (each entry may own clip-mask and filter-list
    // copies); keep the backing allocation for reuse.
    for (int i = 0; i < cv->stack_len; i++) {
        free(cv->stack[i].filters);
        free(cv->stack[i].clip_mask);
    }
    cv->stack_len = 0;
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
    cnvs_text_cache_clear(&cv->text_cache);
    // Open the clip, then clear the whole bitmap to transparent black: a
    // destination-out of a unit-alpha tile leaves dst*(1 - 1) = 0 everywhere.
    compositor_set_clip(cv->comp, NULL, 0);
    int npix = cv->width * cv->height;
    if (ensure_tile(cv, npix)) {
        for (int i = 0; i < npix; i++) {
            cv->tile[i] = (cnvs_premul){ .r = 0, .g = 0, .b = 0, .a = (_Float16)1.0f };
        }
        compositor_blend(cv->comp, 0, 0, cv->width, cv->height, cv->tile,
                         COMPOSITOR_DST_OUT);
    }
}

bool canvas_resize(canvas *__single cv, int width, int height) {
    if (width <= 0 || height <= 0 ||
        width > CANVAS_MAX_DIM || height > CANVAS_MAX_DIM) {
        return false;
    }
    // Build the new-sized compositor first; on failure leave the canvas intact.
    compositor *__single nc = compositor_create(width, height);
    if (!nc) {
        return false;
    }
    compositor_destroy(cv->comp);
    cv->comp = nc;
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

void canvas_translate(canvas *__single cv, float tx, float ty) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "translate", (float[]){ tx, ty }, 2); }
    cv->cur.ctm = cnvs_mat_mul(cv->cur.ctm, cnvs_mat_translate(tx, ty));
}

void canvas_scale(canvas *__single cv, float sx, float sy) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "scale", (float[]){ sx, sy }, 2); }
    cv->cur.ctm = cnvs_mat_mul(cv->cur.ctm, cnvs_mat_scale(sx, sy));
}

void canvas_rotate(canvas *__single cv, float radians) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "rotate", (float[]){ radians }, 1); }
    cv->cur.ctm = cnvs_mat_mul(cv->cur.ctm, cnvs_mat_rotate(radians));
}

void canvas_transform(canvas *__single cv,
                      float a, float b, float c, float d, float e, float f) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "transform", (float[]){ a, b, c, d, e, f }, 6); }
    cnvs_mat m = { .a = a, .b = b, .c = c, .d = d, .e = e, .f = f };
    cv->cur.ctm = cnvs_mat_mul(cv->cur.ctm, m);
}

void canvas_set_transform(canvas *__single cv,
                          float a, float b, float c, float d, float e, float f) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_transform", (float[]){ a, b, c, d, e, f }, 6); }
    cv->cur.ctm = (cnvs_mat){ .a = a, .b = b, .c = c, .d = d, .e = e, .f = f };
}

void canvas_reset_transform(canvas *__single cv) {
    if (cv->rec) { cnvs_rec_op(cv->rec, "reset_transform"); }
    cv->cur.ctm = cnvs_mat_identity();
}

canvas_matrix canvas_get_transform(canvas *__single cv) {
    cnvs_mat m = cv->cur.ctm;
    return (canvas_matrix){ .a = m.a, .b = m.b, .c = m.c,
                            .d = m.d, .e = m.e, .f = m.f };
}

void canvas_set_fill_rgba(canvas *__single cv, float r, float g, float b, float a) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_fill_rgba", (float[]){ r, g, b, a }, 4); }
    cv->cur.fill = cnvs_unpremul_of(clamp01(r), clamp01(g), clamp01(b), clamp01(a));
    cv->cur.fill_kind = CNVS_PAINT_SOLID;
}

// Average CTM scale, used to bake user-space radii into device space.
static float ctm_scale(cnvs_mat m) {
    float det = m.a * m.d - m.b * m.c;
    return sqrtf(fabsf(det));
}

// Initialise a gradient struct in device space (the CTM is baked in now); the
// caller sets the matching paint kind to GRADIENT.
static void grad_set_linear(canvas *__single cv, cnvs_gradient *gr,
                            float x0, float y0, float x1, float y1) {
    gr->kind = CNVS_GRAD_LINEAR;
    gr->p0 = xf(cv, x0, y0);
    gr->p1 = xf(cv, x1, y1);
    gr->r0 = 0.0f;
    gr->r1 = 0.0f;
    gr->angle = 0.0f;
    gr->stop_count = 0;
}

static void grad_set_radial(canvas *__single cv, cnvs_gradient *gr, float x0,
                            float y0, float r0, float x1, float y1, float r1) {
    float s = ctm_scale(cv->cur.ctm);
    gr->kind = CNVS_GRAD_RADIAL;
    gr->p0 = xf(cv, x0, y0);
    gr->p1 = xf(cv, x1, y1);
    gr->r0 = r0 * s;
    gr->r1 = r1 * s;
    gr->angle = 0.0f;
    gr->stop_count = 0;
}

// Rotation angle (radians) of the CTM's x-axis basis, for baking a conic
// gradient's start angle into device space.  Exact for similarity transforms;
// skew / non-uniform scale distort the angles (as they do the radial circles).
static float ctm_rotation(cnvs_mat m) {
    return atan2f(m.b, m.a);
}

static void grad_set_conic(canvas *__single cv, cnvs_gradient *gr,
                           float start_angle, float cx, float cy) {
    gr->kind = CNVS_GRAD_CONIC;
    gr->p0 = xf(cv, cx, cy);  // centre in device space
    gr->p1 = gr->p0;
    gr->r0 = 0.0f;
    gr->r1 = 0.0f;
    gr->angle = start_angle + ctm_rotation(cv->cur.ctm);
    gr->stop_count = 0;
}

// Configure `p` to tile `src` (borrowed) with `repeat`, pinned in device space
// via the inverse of the current CTM.  The (data, len) pair is set together so
// -fbounds-safety can verify the __counted_by(len) invariant: src is itself
// __counted_by(w*h*4), exactly the new len.
static void pattern_set(canvas *__single cv, cnvs_pattern *p,
                        uint8_t const *__counted_by(w * h * 4) src, int w, int h,
                        canvas_pattern_repeat repeat) {
    p->data = src;
    p->len = w * h * 4;
    p->w = w;
    p->h = h;
    p->repeat = repeat;
    p->to_pattern = cnvs_mat_invert(cv->cur.ctm);  // device -> pattern image space
}

void canvas_set_fill_linear_gradient(canvas *__single cv,
                                     float x0, float y0, float x1, float y1) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_fill_linear_gradient", (float[]){ x0, y0, x1, y1 }, 4); }
    grad_set_linear(cv, &cv->cur.fill_grad, x0, y0, x1, y1);
    cv->cur.fill_kind = CNVS_PAINT_GRADIENT;
}

void canvas_set_fill_radial_gradient(canvas *__single cv, float x0, float y0,
                                     float r0, float x1, float y1, float r1) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_fill_radial_gradient", (float[]){ x0, y0, r0, x1, y1, r1 }, 6); }
    grad_set_radial(cv, &cv->cur.fill_grad, x0, y0, r0, x1, y1, r1);
    cv->cur.fill_kind = CNVS_PAINT_GRADIENT;
}

void canvas_set_fill_conic_gradient(canvas *__single cv, float start_angle,
                                    float x, float y) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_fill_conic_gradient", (float[]){ start_angle, x, y }, 3); }
    grad_set_conic(cv, &cv->cur.fill_grad, start_angle, x, y);
    cv->cur.fill_kind = CNVS_PAINT_GRADIENT;
}

void canvas_add_fill_color_stop(canvas *__single cv, float offset,
                                float r, float g, float b, float a) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "add_fill_color_stop", (float[]){ offset, r, g, b, a }, 5); }
    cnvs_gradient_add_stop(&cv->cur.fill_grad, clamp01(offset),
                           cnvs_unpremul_of(clamp01(r), clamp01(g), clamp01(b), clamp01(a)));
}

void canvas_set_fill_pattern(canvas *__single cv,
                             uint8_t const *__counted_by(w * h * 4) src,
                             int w, int h, canvas_pattern_repeat repeat) {
    if (!rgba8_dims_ok(w, h)) {
        return;  // invalid dimensions: leave the fill paint unchanged
    }
    if (cv->rec) {
        // The pattern pixels ride a content-deduped image block; when the
        // block can't be carried (caps/OOM) the op line is skipped with it.
        int const id = cnvs_rec_image(cv->rec, src, w * h * 4, w, h);
        if (id >= 0) { cnvs_rec_pattern(cv->rec, "set_fill_pattern", id, repeat); }
    }
    pattern_set(cv, &cv->cur.fill_pattern, src, w, h, repeat);
    cv->cur.fill_kind = CNVS_PAINT_PATTERN;
}

void canvas_set_stroke_linear_gradient(canvas *__single cv,
                                       float x0, float y0, float x1, float y1) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_stroke_linear_gradient", (float[]){ x0, y0, x1, y1 }, 4); }
    grad_set_linear(cv, &cv->cur.stroke_grad, x0, y0, x1, y1);
    cv->cur.stroke_kind = CNVS_PAINT_GRADIENT;
}

void canvas_set_stroke_radial_gradient(canvas *__single cv, float x0, float y0,
                                       float r0, float x1, float y1, float r1) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_stroke_radial_gradient", (float[]){ x0, y0, r0, x1, y1, r1 }, 6); }
    grad_set_radial(cv, &cv->cur.stroke_grad, x0, y0, r0, x1, y1, r1);
    cv->cur.stroke_kind = CNVS_PAINT_GRADIENT;
}

void canvas_set_stroke_conic_gradient(canvas *__single cv, float start_angle,
                                      float x, float y) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_stroke_conic_gradient", (float[]){ start_angle, x, y }, 3); }
    grad_set_conic(cv, &cv->cur.stroke_grad, start_angle, x, y);
    cv->cur.stroke_kind = CNVS_PAINT_GRADIENT;
}

void canvas_add_stroke_color_stop(canvas *__single cv, float offset,
                                  float r, float g, float b, float a) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "add_stroke_color_stop", (float[]){ offset, r, g, b, a }, 5); }
    cnvs_gradient_add_stop(&cv->cur.stroke_grad, clamp01(offset),
                           cnvs_unpremul_of(clamp01(r), clamp01(g), clamp01(b), clamp01(a)));
}

void canvas_set_stroke_pattern(canvas *__single cv,
                               uint8_t const *__counted_by(w * h * 4) src,
                               int w, int h, canvas_pattern_repeat repeat) {
    if (!rgba8_dims_ok(w, h)) {
        return;
    }
    if (cv->rec) {
        int const id = cnvs_rec_image(cv->rec, src, w * h * 4, w, h);
        if (id >= 0) { cnvs_rec_pattern(cv->rec, "set_stroke_pattern", id, repeat); }
    }
    pattern_set(cv, &cv->cur.stroke_pattern, src, w, h, repeat);
    cv->cur.stroke_kind = CNVS_PAINT_PATTERN;
}

void canvas_set_global_alpha(canvas *__single cv, float alpha) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_global_alpha", (float[]){ alpha }, 1); }
    cv->cur.global_alpha = clamp01(alpha);
}

// canvas_composite_op mirrors compositor_blend_mode value-for-value (canvas.h
// notes the coupling), so the validated cast is the whole mapping.
void canvas_set_global_composite_operation(canvas *__single cv,
                                           canvas_composite_op op) {
    if ((int)op < 0 || (int)op >= COMPOSITOR_MODE_COUNT) {
        return;
    }
    if (cv->rec) { cnvs_rec_composite(cv->rec, op); }
    cv->cur.composite = (compositor_blend_mode)op;
}

void canvas_set_shadow_color_rgba(canvas *__single cv,
                                  float r, float g, float b, float a) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_shadow_color_rgba", (float[]){ r, g, b, a }, 4); }
    cv->cur.shadow_color =
        cnvs_unpremul_of(clamp01(r), clamp01(g), clamp01(b), clamp01(a));
}

void canvas_set_shadow_blur(canvas *__single cv, float blur) {
    if (isfinite(blur) && blur >= 0.0f) {  // spec: ignore negative / non-finite
        // The hook sits inside the guard: an ignored call records nothing
        // (and %.9g of a non-finite would not reparse anyway).
        if (cv->rec) { cnvs_rec_floats(cv->rec, "set_shadow_blur", (float[]){ blur }, 1); }
        cv->cur.shadow_blur = blur;
    }
}

void canvas_set_shadow_offset_x(canvas *__single cv, float offset) {
    if (isfinite(offset)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "set_shadow_offset_x", (float[]){ offset }, 1); }
        cv->cur.shadow_offset_x = offset;
    }
}

void canvas_set_shadow_offset_y(canvas *__single cv, float offset) {
    if (isfinite(offset)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "set_shadow_offset_y", (float[]){ offset }, 1); }
        cv->cur.shadow_offset_y = offset;
    }
}

void canvas_set_filter_none(canvas *__single cv) {
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
static void filter_append(canvas *__single cv, cnvs_filter f) {
    int n = cv->cur.filter_count;
    cnvs_filter *nf = realloc(cv->cur.filters, ((size_t)n + 1) * sizeof *nf);
    if (!nf) {
        return;
    }
    nf[n] = f;
    cv->cur.filters = nf;
    cv->cur.filter_count = n + 1;
}

// The unbounded-above amounts (brightness/contrast/saturate) clamp only below.
static float clamp_lo(float v) {
    return v < 0.0f ? 0.0f : v;
}

void canvas_add_filter_blur(canvas *__single cv, float px) {
    if (!isfinite(px) || px < 0.0f) {
        return;  // spec: ignore an unparseable (or negative) length
    }
    // filter blur(px) IS the Gaussian's stdDev (where shadowBlur is twice it);
    // px == 0 maps to radius 0 -- an identity blur, so nothing is appended.
    // The recorder hooks sit inside each add_filter_*'s accept guard: an
    // ignored call records nothing, and the raw amount rides the line (replay
    // re-clamps and re-compiles it through this same code).
    int r = sigma_box_radius(px);
    if (r > 0) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "add_filter_blur", (float[]){ px }, 1); }
        filter_append(cv, cnvs_filter_blur(r));
    }
}

void canvas_add_filter_brightness(canvas *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "add_filter_brightness", (float[]){ amount }, 1); }
        filter_append(cv, cnvs_filter_brightness(clamp_lo(amount)));
    }
}

void canvas_add_filter_contrast(canvas *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "add_filter_contrast", (float[]){ amount }, 1); }
        filter_append(cv, cnvs_filter_contrast(clamp_lo(amount)));
    }
}

void canvas_add_filter_drop_shadow(canvas *__single cv, float dx, float dy,
                                   float blur, float r, float g, float b,
                                   float a) {
    if (!isfinite(dx) || !isfinite(dy) || !isfinite(blur) || blur < 0.0f) {
        return;  // spec: ignore an unparseable (or negative-blur) drop-shadow
    }
    if (!(clamp01(a) > 0.0f)) {
        return;  // a fully transparent shadow composites as nothing
    }
    if (cv->rec) {
        // dx/dy/blur are guarded finite and ride raw; the colour rides
        // clamped (identity for in-range values, and a NaN channel would
        // otherwise print as unreparseable "nan").
        cnvs_rec_floats(cv->rec, "add_filter_drop_shadow",
                        (float[]){ dx, dy, blur, clamp01(r), clamp01(g),
                                   clamp01(b), clamp01(a) }, 7);
    }
    // The offsets round to whole device pixels (shadow_offset, as for
    // shadowOffset{X,Y}); blur IS the Gaussian's stdDev, like blur() -- but
    // unlike blur(), radius 0 is a real entry (a sharp shadow, not identity).
    filter_append(cv, cnvs_filter_drop_shadow(
                          shadow_offset(dx), shadow_offset(dy),
                          sigma_box_radius(blur), clamp01(r), clamp01(g),
                          clamp01(b), clamp01(a)));
}

void canvas_add_filter_grayscale(canvas *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "add_filter_grayscale", (float[]){ amount }, 1); }
        filter_append(cv, cnvs_filter_grayscale(clamp01(amount)));
    }
}

void canvas_add_filter_hue_rotate(canvas *__single cv, float radians) {
    if (isfinite(radians)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "add_filter_hue_rotate", (float[]){ radians }, 1); }
        filter_append(cv, cnvs_filter_hue_rotate(radians));
    }
}

void canvas_add_filter_invert(canvas *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "add_filter_invert", (float[]){ amount }, 1); }
        filter_append(cv, cnvs_filter_invert(clamp01(amount)));
    }
}

void canvas_add_filter_opacity(canvas *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "add_filter_opacity", (float[]){ amount }, 1); }
        filter_append(cv, cnvs_filter_opacity(clamp01(amount)));
    }
}

void canvas_add_filter_saturate(canvas *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "add_filter_saturate", (float[]){ amount }, 1); }
        filter_append(cv, cnvs_filter_saturate(clamp_lo(amount)));
    }
}

void canvas_add_filter_sepia(canvas *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { cnvs_rec_floats(cv->rec, "add_filter_sepia", (float[]){ amount }, 1); }
        filter_append(cv, cnvs_filter_sepia(clamp01(amount)));
    }
}

static cnvs_vec2 xf(canvas *__single cv, float x, float y) {
    return cnvs_mat_apply(cv->cur.ctm, (cnvs_vec2){ .x = x, .y = y });
}

// Integer device-space bounding box of a point set, padded by `margin` device
// pixels on every side, clamped to the canvas.  The margin is applied *before*
// the canvas clamp so a shape just off-canvas still gets a box for the part of
// its margin (e.g. a blur's soft skirt) that reaches on-canvas.
typedef struct {
    int x, y, w, h;
} cbbox;

static cbbox points_bbox(canvas *__single cv,
                         cnvs_vec2 const *__counted_by(n) pts, int n, int margin) {
    if (n <= 0) {
        return (cbbox){ .x = 0, .y = 0, .w = 0, .h = 0 };
    }
    float minx = pts[0].x, maxx = pts[0].x, miny = pts[0].y, maxy = pts[0].y;
    for (int i = 1; i < n; i++) {
        cnvs_vec2 p = pts[i];
        minx = p.x < minx ? p.x : minx;
        maxx = p.x > maxx ? p.x : maxx;
        miny = p.y < miny ? p.y : miny;
        maxy = p.y > maxy ? p.y : maxy;
    }
    float m = (float)margin;
    float fx0 = floorf(minx) - m, fy0 = floorf(miny) - m;
    float fx1 = ceilf(maxx) + m, fy1 = ceilf(maxy) + m;
    int x0 = cnvs_f2i(fx0), y0 = cnvs_f2i(fy0), x1 = cnvs_f2i(fx1), y1 = cnvs_f2i(fy1);
    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > cv->width) { x1 = cv->width; }
    if (y1 > cv->height) { y1 = cv->height; }
    cbbox b = { .x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0 };
    if (b.w < 0) { b.w = 0; }
    if (b.h < 0) { b.h = 0; }
    return b;
}

static bool ensure_tile(canvas *__single cv, int npix) {
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
static bool ensure_grad_rows(canvas *__single cv, int w) {
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
static bool ensure_blur_tmp(canvas *__single cv, int npix) {
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
static int filter_margin(canvas const *__single cv) {
    int m = 0;
    for (int i = 0; i < cv->cur.filter_count; i++) {
        cnvs_filter const f = cv->cur.filters[i];
        m += 3 * f.blur;
        if (f.shadow) {
            int ax = f.dx < 0 ? -f.dx : f.dx;
            int ay = f.dy < 0 ? -f.dy : f.dy;
            m += ax > ay ? ax : ay;
        }
        if (m > CANVAS_MAX_DIM) {
            return CANVAS_MAX_DIM;
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
// by filter_margin, so the shifted, blurred shadow has tile to land on.  If
// the scratch can't grow the entry is skipped (the op paints shadowless),
// like the other best-effort OOM paths.
static void apply_drop_shadow(canvas *__single cv, cnvs_filter f, int w, int h) {
    int const npix = w * h;
    if (!ensure_blur_tmp(cv, 2 * npix)) {
        return;
    }
    cnvs_premul const tint = cnvs_premultiply(
        cnvs_unpremul_of(f.color[0], f.color[1], f.color[2], f.color[3]));
    for (int y = 0; y < h; y++) {
        int sy = y - f.dy;
        for (int x = 0; x < w; x++) {
            int sx = x - f.dx;
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
    if (f.blur > 0) {  // three box passes ~ a Gaussian, between the two halves
        for (int pass = 0; pass < 3; pass++) {
            blur_box_h_f16(cv->blur_tmp + npix, cv->blur_tmp, w, h, f.blur);
            blur_box_v_f16(cv->blur_tmp, cv->blur_tmp + npix, w, h, f.blur);
        }
    }
    for (int i = 0; i < npix; i++) {  // premultiplied source-over: tile OVER shadow
        cnvs_premul t = cv->tile[i], s = cv->blur_tmp[i];
        float k = 1.0f - (float)t.a;
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
// best-effort OOM paths.  Spec order is filter-then-shadow; our shadowColor
// shadow is already cast from the op's coverage silhouette, not its pixels
// (emit_shadow), so no filter entry reaches it -- the canvas shadow keeps the
// shape's sharp silhouette, the same approximation it makes for
// semi-transparent paint, while a drop-shadow() lives inside the tile.
static void apply_filters(canvas *__single cv, int w, int h) {
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
static void cover_edge(canvas *__single cv, cbbox b, cnvs_vec2 p0, cnvs_vec2 p1) {
    cnvs_cover_add_edge(&cv->cover, b.w, b.h, p0.x - (float)b.x, p0.y - (float)b.y,
                        p1.x - (float)b.x, p1.y - (float)b.y);
}

static void cover_path_edges(canvas *__single cv, cbbox b, cnvs_path const *p) {
    for (int s = 0; s < p->sp_len; s++) {
        cnvs_subpath sp = p->subs[s];
        if (sp.count < 2) {
            continue;
        }
        for (int k = 0; k < sp.count; k++) {
            cnvs_vec2 a = p->pts[sp.start + k];
            cnvs_vec2 c = p->pts[sp.start + (k + 1) % sp.count];
            cover_edge(cv, b, a, c);
        }
    }
}

// --- the planar shade stage --------------------------------------------------
//
// The coverage -> premultiplied-tile fold, eight pixels per step over channel
// planes (cnvs_planar.h).  The fold itself stays f32 -- visibly typed, exactly
// the scalar form's arithmetic and association: alpha = (col.a * ga) *
// (cov / 255.0f) per lane, one narrowing convert to _Float16, then the planar
// premultiply -- so no gallery byte moves (docs/rasterization.md §3.1's
// f32-fold arm).

typedef float foldf8 __attribute__((ext_vector_type(8)));  // the f32 fold plane

// Eight coverage bytes as an f32 plane in [0, 1]: exact widen (every u8 value
// is exact in _Float16 and f32), then the scalar fold's true /255.0f divide,
// lane for lane the same rounding.
static foldf8 cover8(uint8_t const *__counted_by(8) cov) {
    return __builtin_convertvector(cnvs_h8_from_u8(cov), foldf8) / 255.0f;
}

static foldf8 cover8_k(uint8_t const *__counted_by(k) cov, int k) {
    return __builtin_convertvector(cnvs_h8_from_u8_k(cov, k), foldf8) / 255.0f;
}

// Narrow the folded f32 alpha plane once (the scalar (_Float16)alpha cast,
// lane for lane) and premultiply the colour planes under it.
static cnvs_px8 shade8(cnvs_h8 r, cnvs_h8 g, cnvs_h8 b, foldf8 alpha) {
    return cnvs_px8_premultiply(
        (cnvs_px8){ r, g, b, __builtin_convertvector(alpha, cnvs_h8) });
}

// Build an RGBA16F tile from the coverage in cv->cov and the given paint, then
// composite it.  Each pixel's alpha is paint_alpha * global_alpha * coverage.
static void paint_tile(canvas *__single cv, cbbox b, int is_grad,
                       cnvs_gradient const *gr, cnvs_unpremul solid) {
    float const ga = cv->cur.global_alpha;
    if (!is_grad) {
        // Solid paint: the colour planes are splats and every alpha factor but
        // coverage is one constant, so the loop is a coverage widen, two
        // multiplies, and the premultiply -> st4.  Coverage and tile are both
        // dense over the bbox, so one flat loop covers all rows.
        foldf8 const base = (foldf8)((float)solid.a * ga);
        cnvs_h8 const cr = (cnvs_h8)solid.r, cg = (cnvs_h8)solid.g,
                 cb = (cnvs_h8)solid.b;
        int const npix = b.w * b.h;
        int i = 0;
        for (; i + 8 <= npix; i += 8) {
            cnvs_px8_store(cv->tile + i,
                           shade8(cr, cg, cb, base * cover8(cv->cov + i)));
        }
        if (i < npix) {  // tail: k < 8 pixels through the same planar block
            int const k = npix - i;
            cnvs_px8_store_k(cv->tile + i, k,
                             shade8(cr, cg, cb, base * cover8_k(cv->cov + i, k)));
        }
    } else if (ensure_grad_rows(cv, b.w)) {
        // Evaluate the gradient a row at a time, all three stages vectorized:
        // solve the parameters (cnvs_gradient_param_row), lerp the stop
        // colours from them (cnvs_gradient_color_row) -- the exact
        // piecewise-linear colour, no precomputed ramp
        // (docs/decisions/gradient-eval.md) -- then pick the colours back up
        // as planes (ld4 over the row buffer) for the fold.
        for (int py = 0; py < b.h; py++) {
            cnvs_gradient_param_row(gr, b.x, (float)b.y + (float)py + 0.5f, b.w,
                                    cv->trow);
            cnvs_gradient_color_row(gr, cv->trow, b.w, cv->crow);
            int const row = py * b.w;
            int px = 0;
            for (; px + 8 <= b.w; px += 8) {
                cnvs_px8 const col = cnvs_px8_load_unpremul(cv->crow + px);
                foldf8 const alpha = __builtin_convertvector(col.a, foldf8) *
                                     ga * cover8(cv->cov + row + px);
                cnvs_px8_store(cv->tile + row + px,
                               shade8(col.r, col.g, col.b, alpha));
            }
            if (px < b.w) {  // tail: k < 8 pixels through the same planar block
                int const k = b.w - px;
                cnvs_px8 const col = cnvs_px8_load_unpremul_k(cv->crow + px, k);
                foldf8 const alpha = __builtin_convertvector(col.a, foldf8) *
                                     ga * cover8_k(cv->cov + row + px, k);
                cnvs_px8_store_k(cv->tile + row + px, k,
                                 shade8(col.r, col.g, col.b, alpha));
            }
        }
    } else {
        // OOM fallback: the row buffers couldn't grow, so run the scalar
        // per-pixel parameter solve + stop lerp.
        for (int py = 0; py < b.h; py++) {
            for (int px = 0; px < b.w; px++) {
                int i = py * b.w + px;
                float covf = (float)cv->cov[i] / 255.0f;
                float t;
                cnvs_vec2 p = { .x = (float)b.x + (float)px + 0.5f,
                                .y = (float)b.y + (float)py + 0.5f };
                cnvs_unpremul col = cnvs_gradient_param(gr, p, &t)
                                        ? cnvs_gradient_color_at(gr, t)
                                        : cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 0.0f);
                // Fold coverage and global alpha into the paint's alpha, then
                // premultiply -- the tile stores premultiplied pixels.
                float alpha = (float)col.a * ga * covf;
                cv->tile[i] = cnvs_premultiply((cnvs_unpremul){
                    .r = col.r, .g = col.g, .b = col.b, .a = (_Float16)alpha });
            }
        }
    }
    apply_filters(cv, b.w, b.h);
    compositor_blend(cv->comp, b.x, b.y, b.w, b.h, cv->tile, cv->cur.composite);
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
static void pattern_sample(cnvs_pattern const *p, float u, float v, bool smooth,
                           float *__counted_by(4) out) {
    bool rx = p->repeat == CANVAS_REPEAT || p->repeat == CANVAS_REPEAT_X;
    bool ry = p->repeat == CANVAS_REPEAT || p->repeat == CANVAS_REPEAT_Y;
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
            float c00 = (float)p->data[(v0 * w + u0) * 4 + k];
            float c10 = (float)p->data[(v0 * w + u1) * 4 + k];
            float c01 = (float)p->data[(v1 * w + u0) * 4 + k];
            float c11 = (float)p->data[(v1 * w + u1) * 4 + k];
            float top = c00 + (c10 - c00) * tu;
            float bot = c01 + (c11 - c01) * tu;
            out[k] = (top + (bot - top) * tv) / 255.0f;
        }
    } else {
        int iu = wrap_idx(cnvs_f2i(floorf(u)), w, rx);
        int iv = wrap_idx(cnvs_f2i(floorf(v)), h, ry);
        int o = (iv * w + iu) * 4;
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
// skips its sample and stays transparent black, exactly the scalar early-out
// (covf <= 0 iff the coverage byte is 0; the all-zero lanes fold to the same
// {0,0,0,0} bits the early-out stored).  The samples land in f32 planes
// because the scalar fold is f32: alpha = (s[3] * ga) * covf lane for lane,
// each channel narrowed once to _Float16 (the cnvs_unpremul_of casts), then
// the planar premultiply.
static void paint_tile_pattern(canvas *__single cv, cbbox b, cnvs_pattern const *p) {
    float const ga = cv->cur.global_alpha;
    bool const smooth = cv->cur.image_smoothing_enabled;
    for (int py = 0; py < b.h; py++) {
        float const dy = (float)b.y + (float)py + 0.5f;
        for (int px = 0; px < b.w; px += 8) {
            int const i = py * b.w + px;
            int const k = b.w - px < 8 ? b.w - px : 8;
            foldf8 sr = (foldf8)0.0f, sg = (foldf8)0.0f, sb = (foldf8)0.0f,
                   sa = (foldf8)0.0f;
            for (int l = 0; l < k; l++) {
                if (cv->cov[i + l] == 0) {  // the scalar covf <= 0.0f early-out
                    continue;
                }
                cnvs_vec2 d = { .x = (float)b.x + (float)(px + l) + 0.5f, .y = dy };
                cnvs_vec2 uv = cnvs_mat_apply(p->to_pattern, d);
                float s[4];
                pattern_sample(p, uv.x, uv.y, smooth, s);
                sr[l] = s[0];
                sg[l] = s[1];
                sb[l] = s[2];
                sa[l] = s[3];
            }
            foldf8 const covf = k < 8 ? cover8_k(cv->cov + i, k)
                                      : cover8(cv->cov + i);
            cnvs_px8 const out = shade8(__builtin_convertvector(sr, cnvs_h8),
                                        __builtin_convertvector(sg, cnvs_h8),
                                        __builtin_convertvector(sb, cnvs_h8),
                                        sa * ga * covf);
            if (k < 8) {
                cnvs_px8_store_k(cv->tile + i, k, out);
            } else {
                cnvs_px8_store(cv->tile + i, out);
            }
        }
    }
    apply_filters(cv, b.w, b.h);
    compositor_blend(cv->comp, b.x, b.y, b.w, b.h, cv->tile, cv->cur.composite);
}

// Grow the two shadow ping-pong masks to at least n bytes each.
static bool ensure_shadow(canvas *__single cv, int n) {
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
static bool shadow_active(canvas const *__single cv) {
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
    int r = (int)(sigma + 0.5f);
    return r < 1 ? 1 : (r > 1024 ? 1024 : r);
}

// The spec's shadow Gaussian has stdDev = shadowBlur / 2.
static int shadow_radius(float blur) {
    return sigma_box_radius(blur * 0.5f);
}

// Round a shadow offset to an integer device pixel, clamped to a sane range (a
// larger offset just pushes the shadow off-canvas).
static int shadow_offset(float v) {
    float m = (float)(2 * CANVAS_MAX_DIM);
    v = v > m ? m : (v < -m ? -m : v);
    return cnvs_f2i(roundf(v));
}

// Cast the current op's shadow from the coverage in cv->cov over bbox b: build a
// single-channel mask of the op's silhouette, blur it (~Gaussian), tint it with
// the shadow colour (global alpha folded in), and composite it -- offset, under
// the shape (which the caller paints next).  Blur and offset are device-space and
// unaffected by the CTM, per spec.  All CPU-side, so both backends agree.
static void emit_shadow(canvas *__single cv, cbbox b) {
    if (!shadow_active(cv) || b.w <= 0 || b.h <= 0) {
        return;
    }
    int r = shadow_radius(cv->cur.shadow_blur);
    int offx = shadow_offset(cv->cur.shadow_offset_x);
    int offy = shadow_offset(cv->cur.shadow_offset_y);
    // Three box passes each spread the blur by r, so the falloff reaches ~0 only
    // at 3r beyond the shape -- the mask region must include that whole spread or
    // the soft edge gets clipped to a rectangle.
    int margin = 3 * r;
    int sx0 = b.x + offx - margin, sy0 = b.y + offy - margin;
    int sx1 = b.x + b.w + offx + margin, sy1 = b.y + b.h + offy + margin;
    if (sx0 < 0) { sx0 = 0; }
    if (sy0 < 0) { sy0 = 0; }
    if (sx1 > cv->width) { sx1 = cv->width; }
    if (sy1 > cv->height) { sy1 = cv->height; }
    int sw = sx1 - sx0, sh = sy1 - sy0;
    if (sw <= 0 || sh <= 0 || !ensure_shadow(cv, sw * sh) || !ensure_tile(cv, sw * sh)) {
        return;
    }
    int n = sw * sh;
    memset(cv->shadow_src, 0, (size_t)n);
    // Stamp the op coverage into the mask at its offset position (clipped to the
    // mask, which may be tighter than the offset coverage near the canvas edge).
    int mx0 = b.x + offx - sx0, my0 = b.y + offy - sy0;
    for (int cy = 0; cy < b.h; cy++) {
        int my = my0 + cy;
        if (my < 0 || my >= sh) {
            continue;
        }
        for (int cx = 0; cx < b.w; cx++) {
            int mx = mx0 + cx;
            if (mx >= 0 && mx < sw) {
                cv->shadow_src[my * sw + mx] = cv->cov[cy * b.w + cx];
            }
        }
    }
    if (r > 0) {  // three separable box passes ~ a Gaussian (src/dst ping-pong)
        for (int pass = 0; pass < 3; pass++) {
            blur_box_h(cv->shadow_dst, cv->shadow_src, sw, sh, r);
            blur_box_v(cv->shadow_src, cv->shadow_dst, sw, sh, r);
        }
    }
    // Tint the blurred mask into the tile, eight pixels per step: the shadow
    // colour is a splat, so this is the planar shade fold with the blurred
    // mask as its coverage plane -- the scalar form's f32 arithmetic and
    // association ((mask / 255.0f) * sc.a * ga) kept lane for lane.
    cnvs_unpremul const sc = cv->cur.shadow_color;
    float const ga = cv->cur.global_alpha;
    cnvs_h8 const cr = (cnvs_h8)sc.r, cg = (cnvs_h8)sc.g, cb = (cnvs_h8)sc.b;
    float const sa = (float)sc.a;
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        foldf8 const alpha = cover8(cv->shadow_src + i) * sa * ga;
        cnvs_px8_store(cv->tile + i, shade8(cr, cg, cb, alpha));
    }
    if (i < n) {  // tail: k < 8 pixels through the same planar block
        int const k = n - i;
        foldf8 const alpha = cover8_k(cv->shadow_src + i, k) * sa * ga;
        cnvs_px8_store_k(cv->tile + i, k, shade8(cr, cg, cb, alpha));
    }
    compositor_blend(cv->comp, sx0, sy0, sw, sh, cv->tile, cv->cur.composite);
}

// Paint the resolved coverage with the current fill / stroke paint, dispatching
// on its kind (solid and gradient share paint_tile; pattern has its own loop).
// The shadow, if any, is cast first so it lands under the shape.
static void paint_fill(canvas *__single cv, cbbox b) {
    emit_shadow(cv, b);
    if (cv->cur.fill_kind == CNVS_PAINT_PATTERN) {
        paint_tile_pattern(cv, b, &cv->cur.fill_pattern);
    } else {
        paint_tile(cv, b, cv->cur.fill_kind == CNVS_PAINT_GRADIENT,
                   &cv->cur.fill_grad, cv->cur.fill);
    }
}

static void paint_stroke(canvas *__single cv, cbbox b) {
    emit_shadow(cv, b);
    if (cv->cur.stroke_kind == CNVS_PAINT_PATTERN) {
        paint_tile_pattern(cv, b, &cv->cur.stroke_pattern);
    } else {
        paint_tile(cv, b, cv->cur.stroke_kind == CNVS_PAINT_GRADIENT,
                   &cv->cur.stroke_grad, cv->cur.stroke);
    }
}

void canvas_clear_rect(canvas *__single cv, float x, float y, float w, float h) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "clear_rect", (float[]){ x, y, w, h }, 4); }
    cnvs_vec2 q[4] = { xf(cv, x, y), xf(cv, x + w, y),
                       xf(cv, x + w, y + h), xf(cv, x, y + h) };
    cbbox b = points_bbox(cv, q, 4, 0);  // clear_rect bypasses filters: no margin
    if (b.w <= 0 || b.h <= 0 || !ensure_tile(cv, b.w * b.h)) {
        return;
    }
    // Erase = destination-out of a unit-alpha tile: out = dst*(1 - alpha), and the
    // clip attenuates alpha to the coverage, so a clip leaves dst*(1 - clip).
    int npix = b.w * b.h;
    for (int i = 0; i < npix; i++) {
        cv->tile[i] = (cnvs_premul){ .r = 0, .g = 0, .b = 0, .a = (_Float16)1.0f };
    }
    compositor_blend(cv->comp, b.x, b.y, b.w, b.h, cv->tile, COMPOSITOR_DST_OUT);
}

void canvas_fill_rect(canvas *__single cv, float x, float y, float w, float h) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "fill_rect", (float[]){ x, y, w, h }, 4); }
    cnvs_vec2 q[4] = { xf(cv, x, y), xf(cv, x + w, y),
                       xf(cv, x + w, y + h), xf(cv, x, y + h) };
    cbbox b = points_bbox(cv, q, 4, filter_margin(cv));
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

void canvas_begin_path(canvas *__single cv) {
    if (cv->rec) { cnvs_rec_op(cv->rec, "begin_path"); }
    cnvs_path_reset(&cv->path);
}

void canvas_move_to(canvas *__single cv, float x, float y) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "move_to", (float[]){ x, y }, 2); }
    cnvs_path_move_to(&cv->path, xf(cv, x, y));
    cv->cur_user = (cnvs_vec2){ .x = x, .y = y };
}

void canvas_line_to(canvas *__single cv, float x, float y) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "line_to", (float[]){ x, y }, 2); }
    cnvs_path_line_to(&cv->path, xf(cv, x, y));
    cv->cur_user = (cnvs_vec2){ .x = x, .y = y };
}

void canvas_rect(canvas *__single cv, float x, float y, float w, float h) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "rect", (float[]){ x, y, w, h }, 4); }
    cnvs_path_rect(&cv->path, xf(cv, x, y), xf(cv, x + w, y),
                   xf(cv, x + w, y + h), xf(cv, x, y + h));
    cv->cur_user = (cnvs_vec2){ .x = x, .y = y };
}

void canvas_quadratic_curve_to(canvas *__single cv,
                               float cpx, float cpy, float x, float y) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "quadratic_curve_to", (float[]){ cpx, cpy, x, y }, 4); }
    cnvs_path_quad_to(&cv->path, xf(cv, cpx, cpy), xf(cv, x, y),
                      CANVAS_FLATTEN_TOL);
    cv->cur_user = (cnvs_vec2){ .x = x, .y = y };
}

void canvas_bezier_curve_to(canvas *__single cv, float c1x, float c1y,
                            float c2x, float c2y, float x, float y) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "bezier_curve_to", (float[]){ c1x, c1y, c2x, c2y, x, y }, 6); }
    cnvs_path_cubic_to(&cv->path, xf(cv, c1x, c1y), xf(cv, c2x, c2y),
                       xf(cv, x, y), CANVAS_FLATTEN_TOL);
    cv->cur_user = (cnvs_vec2){ .x = x, .y = y };
}

void canvas_ellipse(canvas *__single cv, float x, float y, float rx, float ry,
                    float rotation, float start_angle, float end_angle,
                    bool anticlockwise) {
    if (cv->rec) {
        cnvs_rec_floats_bool(cv->rec, "ellipse",
                             (float[]){ x, y, rx, ry, rotation, start_angle, end_angle },
                             7, anticlockwise);
    }
    float two_pi = 2.0f * (float)M_PI;
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
    float arx = rx < 0.0f ? -rx : rx;
    float ary = ry < 0.0f ? -ry : ry;
    float rmax = arx > ary ? arx : ary;
    float rr = rmax > CANVAS_FLATTEN_TOL ? rmax : CANVAS_FLATTEN_TOL;
    float dstep = 2.0f * acosf(fmaxf(-1.0f, 1.0f - CANVAS_FLATTEN_TOL / rr));
    if (!(dstep > 1e-4f)) {
        dstep = 1e-4f;  // guard against tiny/NaN step
    }
    float fsegs = ceilf(fabsf(sweep) / dstep);
    int segs = cnvs_f2i(fsegs);
    if (segs < 2) {
        segs = 2;
    }
    if (segs > 4096) {
        segs = 4096;
    }
    float cosr = cosf(rotation);
    float sinr = sinf(rotation);
    for (int i = 0; i <= segs; i++) {
        float t = start_angle + sweep * ((float)i / (float)segs);
        float ex = rx * cosf(t);
        float ey = ry * sinf(t);
        cnvs_vec2 p = xf(cv, x + ex * cosr - ey * sinr, y + ex * sinr + ey * cosr);
        if (i == 0 && !cv->path.has_cur) {
            cnvs_path_move_to(&cv->path, p);
        } else {
            cnvs_path_line_to(&cv->path, p);
        }
    }
    float te = start_angle + sweep;
    cv->cur_user = (cnvs_vec2){
        .x = x + rx * cosf(te) * cosr - ry * sinf(te) * sinr,
        .y = y + rx * cosf(te) * sinr + ry * sinf(te) * cosr,
    };
}

void canvas_arc(canvas *__single cv, float x, float y, float radius,
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

void canvas_round_rect(canvas *__single cv, float x, float y, float w, float h,
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
    float q = (float)M_PI * 0.5f;
    float pi = (float)M_PI;
    canvas_move_to(cv, x + r, y);
    canvas_arc(cv, x + w - r, y + r, r, -q, 0.0f, false);     // top-right
    canvas_arc(cv, x + w - r, y + h - r, r, 0.0f, q, false);  // bottom-right
    canvas_arc(cv, x + r, y + h - r, r, q, pi, false);        // bottom-left
    canvas_arc(cv, x + r, y + r, r, pi, pi + q, false);       // top-left
    canvas_close_path(cv);
    cnvs_rec_leave(cv->rec);
}

// CSS border-radius overlap rule: reduce the scale factor `f` so that two radii
// summing to `sum` fit within an edge of length `len`.  `sum` 0 (no radii on the
// edge) imposes no constraint.
static float radii_fit(float f, float len, float sum) {
    if (sum > 0.0f) {
        float g = len / sum;
        if (g < f) {
            f = g;
        }
    }
    return f;
}

static void round_rect_radii_impl(canvas *__single cv, float x, float y,
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
    float q = (float)M_PI * 0.5f;
    float pi = (float)M_PI;
    canvas_move_to(cv, x + r[0], y);
    canvas_ellipse(cv, x + w - r[2], y + r[3], r[2], r[3], 0.0f, -q, 0.0f, false);
    canvas_ellipse(cv, x + w - r[4], y + h - r[5], r[4], r[5], 0.0f, 0.0f, q, false);
    canvas_ellipse(cv, x + r[6], y + h - r[7], r[6], r[7], 0.0f, q, pi, false);
    canvas_ellipse(cv, x + r[0], y + r[1], r[0], r[1], 0.0f, pi, pi + q, false);
    canvas_close_path(cv);
}

void canvas_round_rect_radii(canvas *__single cv, float x, float y,
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

static void arc_to_impl(canvas *__single cv, float x1, float y1, float x2, float y2,
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
    float l0 = sqrtf(u0x * u0x + u0y * u0y);
    float l2 = sqrtf(u2x * u2x + u2y * u2y);
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
    float ang = acosf(cosang);
    if (ang < 1e-3f || (float)M_PI - ang < 1e-3f) {
        canvas_line_to(cv, x1, y1);  // collinear: no arc
        return;
    }
    float td = radius / tanf(ang * 0.5f);   // P1 -> tangent point distance
    float bx = u0x + u2x;
    float by = u0y + u2y;
    float bl = sqrtf(bx * bx + by * by);
    float cdist = radius / sinf(ang * 0.5f);  // P1 -> arc centre distance
    float cx = x1 + bx / bl * cdist;
    float cy = y1 + by / bl * cdist;
    float t1x = x1 + u0x * td;
    float t1y = y1 + u0y * td;
    float t2x = x1 + u2x * td;
    float t2y = y1 + u2y * td;
    float sa = atan2f(t1y - cy, t1x - cx);
    float ea = atan2f(t2y - cy, t2x - cx);
    bool ccw = (u0x * u2y - u0y * u2x) > 0.0f;
    canvas_line_to(cv, t1x, t1y);
    canvas_arc(cv, cx, cy, radius, sa, ea, ccw);
    cv->cur_user = (cnvs_vec2){ .x = t2x, .y = t2y };
}

void canvas_arc_to(canvas *__single cv, float x1, float y1, float x2, float y2,
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

void canvas_close_path(canvas *__single cv) {
    if (cv->rec) { cnvs_rec_op(cv->rec, "close_path"); }
    cnvs_path_close(&cv->path);
}

void canvas_set_fill_rule(canvas *__single cv, canvas_fill_rule rule) {
    if (cv->rec) { cnvs_rec_fill_rule(cv->rec, rule); }
    switch (rule) {
        case CANVAS_NONZERO: cv->cur.fill_rule = CNVS_NONZERO; break;
        case CANVAS_EVENODD: cv->cur.fill_rule = CNVS_EVENODD; break;
    }
}

// Rasterize a device-space path under `rule` and paint it with the fill paint
// over its clamped bbox.
static void fill_device_path(canvas *__single cv, cnvs_path const *p,
                             cnvs_fill_rule rule) {
    cbbox b = points_bbox(cv, p->pts, p->pt_len, filter_margin(cv));
    if (b.w <= 0 || b.h <= 0 || !ensure_tile(cv, b.w * b.h) ||
        !cnvs_cover_reset(&cv->cover, b.w, b.h)) {
        return;
    }
    cover_path_edges(cv, b, p);
    cnvs_cover_resolve(&cv->cover, b.w, b.h, rule, cv->cov);
    paint_fill(cv, b);
}

void canvas_fill(canvas *__single cv) {
    if (cv->rec) { cnvs_rec_op(cv->rec, "fill"); }
    fill_device_path(cv, &cv->path, cv->cur.fill_rule);
}

// Point-in-path for hit testing.  Each subpath is treated as implicitly closed
// (as the fill rasterizer does).  Casts a ray in +x from `q` and counts edge
// crossings: the signed count is the winding number (nonzero rule) and the raw
// count is the crossing number (even-odd rule).  The half-open vertical test
// (a.y <= q.y < b.y for an upward edge, and the reverse for downward) counts each
// shared vertex exactly once.
static bool path_contains(cnvs_path const *p, cnvs_vec2 q, cnvs_fill_rule rule) {
    int wn = 0;   // winding number  (nonzero rule)
    int cn = 0;   // crossing number (even-odd rule)
    for (int s = 0; s < p->sp_len; s++) {
        cnvs_subpath sp = p->subs[s];
        if (sp.count < 2) {
            continue;
        }
        for (int k = 0; k < sp.count; k++) {
            cnvs_vec2 a = p->pts[sp.start + k];
            cnvs_vec2 b = p->pts[sp.start + (k + 1) % sp.count];
            bool up = a.y <= q.y && b.y > q.y;
            bool down = a.y > q.y && b.y <= q.y;
            if (!up && !down) {
                continue;  // edge doesn't straddle the ray's row
            }
            // isLeft > 0 means q is left of the directed edge a->b, i.e. the +x
            // ray from q crosses it.  An upward edge then winds +1, a downward -1.
            float is_left = (b.x - a.x) * (q.y - a.y) - (q.x - a.x) * (b.y - a.y);
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

bool canvas_is_point_in_path(canvas *__single cv, float x, float y,
                             canvas_fill_rule rule) {
    if (!isfinite(x) || !isfinite(y)) {
        return false;
    }
    cnvs_fill_rule r = rule == CANVAS_EVENODD ? CNVS_EVENODD : CNVS_NONZERO;
    return path_contains(&cv->path, xf(cv, x, y), r);
}

void canvas_clip(canvas *__single cv) {
    if (cv->rec) { cnvs_rec_op(cv->rec, "clip"); }
    int n = cv->width * cv->height;
    uint8_t *nm = malloc((size_t)n);
    if (!nm) {
        return;
    }
    // Rasterize the path's coverage into cv->cov over its (clamped) bbox.
    cbbox b = points_bbox(cv, cv->path.pts, cv->path.pt_len, 0);  // the clip is unfiltered
    if (b.w > 0 && b.h > 0 && ensure_tile(cv, b.w * b.h) &&
        cnvs_cover_reset(&cv->cover, b.w, b.h)) {
        cover_path_edges(cv, b, &cv->path);
        cnvs_cover_resolve(&cv->cover, b.w, b.h, cv->cur.fill_rule, cv->cov);
    } else {
        b.w = 0;
        b.h = 0;  // empty path: clip to nothing
    }
    // new_clip = old_clip * path_coverage, zero outside the path's bbox.
    for (int yy = 0; yy < cv->height; yy++) {
        for (int xx = 0; xx < cv->width; xx++) {
            int i = yy * cv->width + xx;
            int pc = 0;
            if (xx >= b.x && xx < b.x + b.w && yy >= b.y && yy < b.y + b.h) {
                pc = cv->cov[(yy - b.y) * b.w + (xx - b.x)];
            }
            int old = cv->cur.clip_mask ? cv->cur.clip_mask[i] : 255;
            nm[i] = (uint8_t)(old * pc / 255);
        }
    }
    free(cv->cur.clip_mask);
    cv->cur.clip_mask = nm;
    cv->cur.clip_len = n;
    compositor_set_clip(cv->comp, cv->cur.clip_mask, n);
}

void canvas_set_stroke_rgba(canvas *__single cv, float r, float g, float b, float a) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_stroke_rgba", (float[]){ r, g, b, a }, 4); }
    cv->cur.stroke = cnvs_unpremul_of(clamp01(r), clamp01(g), clamp01(b), clamp01(a));
    cv->cur.stroke_kind = CNVS_PAINT_SOLID;
}

void canvas_set_line_width(canvas *__single cv, float width) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_line_width", (float[]){ width }, 1); }
    cv->cur.line_width = width;
}

void canvas_set_line_join(canvas *__single cv, canvas_line_join join) {
    if (cv->rec) { cnvs_rec_line_join(cv->rec, join); }
    switch (join) {
        case CANVAS_JOIN_MITER: cv->cur.line_join = CNVS_JOIN_MITER; break;
        case CANVAS_JOIN_ROUND: cv->cur.line_join = CNVS_JOIN_ROUND; break;
        case CANVAS_JOIN_BEVEL: cv->cur.line_join = CNVS_JOIN_BEVEL; break;
    }
}

void canvas_set_line_cap(canvas *__single cv, canvas_line_cap cap) {
    if (cv->rec) { cnvs_rec_line_cap(cv->rec, cap); }
    switch (cap) {
        case CANVAS_CAP_BUTT:   cv->cur.line_cap = CNVS_CAP_BUTT;   break;
        case CANVAS_CAP_ROUND:  cv->cur.line_cap = CNVS_CAP_ROUND;  break;
        case CANVAS_CAP_SQUARE: cv->cur.line_cap = CNVS_CAP_SQUARE; break;
    }
}

void canvas_set_miter_limit(canvas *__single cv, float limit) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_miter_limit", (float[]){ limit }, 1); }
    cv->cur.miter_limit = limit;
}

void canvas_set_line_dash(canvas *__single cv,
                          float const *__counted_by(count) pattern, int count) {
    // Clamp into a separate variable: mutating `count` would desync the
    // __counted_by(count) bound on `pattern`.
    int m = count < 0 ? 0 : count;
    if (m > CANVAS_MAX_DASH) {
        m = CANVAS_MAX_DASH;
    }
    for (int i = 0; i < m; i++) {
        cv->cur.dash[i] = pattern[i];
    }
    cv->cur.dash_count = m;
    // Record the effective (clamped) pattern, so the line never exceeds the
    // parser's per-line dash cap and re-clamps to the same state on replay.
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_line_dash", cv->cur.dash, m); }
}

int canvas_get_line_dash(canvas *__single cv,
                         float *__counted_by(cap) out, int cap) {
    int n = cv->cur.dash_count;
    // Copy at most `cap` entries; never write past the caller's buffer, and never
    // mutate `cap` itself (it bounds `out`).  A negative cap copies nothing.
    int m = cap < n ? cap : n;
    for (int i = 0; i < m; i++) {
        out[i] = cv->cur.dash[i];
    }
    return n;
}

void canvas_set_line_dash_offset(canvas *__single cv, float offset) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_line_dash_offset", (float[]){ offset }, 1); }
    cv->cur.dash_offset = offset;
}

// Build the stroke triangles for `p` into cv->scratch_verts under the current
// line styles (width/join/cap/dash, CTM scale baked in).  False on alloc failure.
static bool build_stroke_verts(canvas *__single cv, cnvs_path const *p) {
    cnvs_verts_reset(&cv->scratch_verts);
    // Line width and dash lengths are in user units; bake the CTM scale in.
    float scale = ctm_scale(cv->cur.ctm);
    float hw = cv->cur.line_width * 0.5f * scale;

    bool dashed = cv->cur.dash_count > 0;
    float sdash[CANVAS_MAX_DASH];
    for (int i = 0; i < cv->cur.dash_count; i++) {
        sdash[i] = cv->cur.dash[i] * scale;
    }
    float soff = cv->cur.dash_offset * scale;

    for (int s = 0; s < p->sp_len; s++) {
        cnvs_subpath sp = p->subs[s];
        if (sp.count < 2) {
            continue;
        }
        cnvs_vec2 *poly = p->pts + sp.start;
        bool ok = dashed
                      ? cnvs_stroke_dashed(poly, sp.count, sp.closed, hw, sdash,
                                           cv->cur.dash_count, soff,
                                           &cv->scratch_verts)
                      : cnvs_stroke_polyline(poly, sp.count, sp.closed, hw,
                                             cv->cur.line_join, cv->cur.line_cap,
                                             cv->cur.miter_limit,
                                             &cv->scratch_verts);
        if (!ok) {
            return false;
        }
    }
    return true;
}

static void stroke_device_path(canvas *__single cv, cnvs_path const *p) {
    if (!build_stroke_verts(cv, p) || cv->scratch_verts.len < 3) {
        return;
    }
    cbbox b = points_bbox(cv, cv->scratch_verts.data, cv->scratch_verts.len,
                           filter_margin(cv));
    if (b.w <= 0 || b.h <= 0 || !ensure_tile(cv, b.w * b.h) ||
        !cnvs_cover_reset(&cv->cover, b.w, b.h)) {
        return;
    }
    // Feed each stroke triangle as edges, forced to a consistent winding so the
    // overlapping join/cap triangles union (nonzero) instead of cancelling.
    for (int i = 0; i + 2 < cv->scratch_verts.len; i += 3) {
        cnvs_vec2 p0 = cv->scratch_verts.data[i];
        cnvs_vec2 p1 = cv->scratch_verts.data[i + 1];
        cnvs_vec2 p2 = cv->scratch_verts.data[i + 2];
        float area = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
        if (area < 0.0f) {
            cnvs_vec2 t = p1;
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

void canvas_stroke(canvas *__single cv) {
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
    bool neg = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
    bool pos = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
    return !(neg && pos);
}

// Whether q lies in the stroke triangles currently in cv->scratch_verts (their
// union -- inside any triangle counts).
static bool stroke_verts_contain(canvas *__single cv, cnvs_vec2 q) {
    for (int i = 0; i + 2 < cv->scratch_verts.len; i += 3) {
        if (point_in_tri(q, cv->scratch_verts.data[i], cv->scratch_verts.data[i + 1],
                         cv->scratch_verts.data[i + 2])) {
            return true;
        }
    }
    return false;
}

bool canvas_is_point_in_stroke(canvas *__single cv, float x, float y) {
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

void canvas_stroke_rect(canvas *__single cv, float x, float y, float w, float h) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "stroke_rect", (float[]){ x, y, w, h }, 4); }
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h)) {
        return;  // Canvas spec: non-finite args paint nothing.
    }
    // strokeRect builds and strokes its own rectangle without touching the
    // current path; the corners go through the CTM exactly as fill_rect's quad.
    cnvs_path rp;
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
static cnvs_font *__single ensure_font(canvas *__single cv) {
    if (!cv->font || fabsf(cv->font_built_size - cv->cur.font_size) > 1e-6f) {
        cnvs_font_destroy(cv->font);
        cv->font = cnvs_font_create(k_font_family, (int)sizeof k_font_family - 1,
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
static bool canvas_vmetrics(canvas *__single cv, float *__single ascent,
                            float *__single descent) {
    float size = cv->cur.font_size;
    int fid = cnvs_text_cache_intern(&cv->text_cache, k_font_family,
                                     (int)sizeof k_font_family - 1);
    float a1 = 0.0f, d1 = 0.0f;
    if (cnvs_text_cache_get_vmetrics(&cv->text_cache, fid, &a1, &d1)) {
        *ascent = a1 * size;
        *descent = d1 * size;
        return true;
    }
    cnvs_font *__single f = ensure_font(cv);
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

void canvas_set_font_size(canvas *__single cv, float px) {
    if (cv->rec) { cnvs_rec_floats(cv->rec, "set_font_size", (float[]){ px }, 1); }
    cv->cur.font_size = px > 0.0f ? px : 0.0f;
}

void canvas_set_text_align(canvas *__single cv, canvas_text_align align) {
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

void canvas_set_direction(canvas *__single cv, canvas_direction dir) {
    switch (dir) {
        case CANVAS_DIRECTION_LTR:
        case CANVAS_DIRECTION_RTL:
            if (cv->rec) { cnvs_rec_direction(cv->rec, dir); }
            cv->cur.direction = dir;
            break;
    }
}

void canvas_set_text_baseline(canvas *__single cv, canvas_text_baseline baseline) {
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
static float text_align_frac(canvas_text_align a, canvas_direction dir) {
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
static float text_baseline_offset(canvas *__single cv) {
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
static cnvs_shaped const *__single shape_text(canvas *__single cv,
                                              char const *__counted_by(len) text,
                                              int len) {
    return cnvs_text_cache_shape(&cv->text_cache, k_font_family,
                                 (int)sizeof k_font_family - 1,
                                 cv->cur.font_size,
                                 cv->cur.direction == CANVAS_DIRECTION_RTL,
                                 text, len);
}

// Render one color (emoji) glyph from its canonical capture: pick the mip
// level whose resolution best matches the glyph quad's device footprint and
// sample it through the same transform-aware bilinear path drawImage takes --
// so the emoji takes the transform, clip, global alpha, and shadow like any
// other image, and no boundary call (indeed, no CTFontRef at all) is needed
// once the capture exists.  The capture covers the glyph-space rect
// [ink_x0, ink_x0 + cap_w] x [ink_y0, ink_y0 + cap_h] in capture px (y up,
// baseline-relative); scaling by size_px / CNVS_CAPTURE_EM and pinning to the
// pen maps it to user space.
static void draw_glyph_capture(canvas *__single cv, cnvs_glyph_slot *__single slot,
                               float pen_x, float baseline_y, float size_px) {
    float const k = size_px / (float)CNVS_CAPTURE_EM;
    float const dw = (float)slot->cap_w * k;
    float const dh = (float)slot->cap_h * k;
    if (!(dw > 0.0f) || !(dh > 0.0f)) {
        return;
    }
    float const dx = pen_x + slot->ink_x0 * k;
    float const dy = baseline_y - (slot->ink_y0 + (float)slot->cap_h) * k;
    // The mip rule: the quad's device footprint is its longer mapped edge, and
    // the level sampled is the smallest one >= that footprint -- bilinear then
    // never downscales by more than 2x within the level, which box-halved
    // sources handle without visible aliasing or softness.
    cnvs_mat const m = cv->cur.ctm;
    float const ex = hypotf(m.a * dw, m.b * dw);
    float const ey = hypotf(m.c * dh, m.d * dh);
    cnvs_mip const lvl = cnvs_glyph_mip(slot, ex > ey ? ex : ey);
    if (!lvl.px) {
        return;
    }
    draw_image_quad(cv, lvl.px, lvl.len, lvl.w, lvl.h, 0.0f, 0.0f,
                    (float)lvl.w, (float)lvl.h, dx, dy, dw, dh, true);
}

// The degraded path when the capture cache can't serve (full table, OOM) but
// the run still has its boundary handle: ask the boundary for the ink box,
// draw into a checked RGBA8 buffer at device size, unpremultiply, then
// composite through the CTM with canvas_draw_image_subrect -- the per-draw
// rasterization that used to be the only emoji path.
static void draw_color_glyph(canvas *__single cv, void *__single font,
                             uint16_t glyph, float pen_x, float baseline_y) {
    float x0, y0, x1, y1;
    cnvs_glyph_bounds(font, glyph, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0) {
        return;  // blank glyph (e.g. a space in the color font)
    }
    int const margin = 1;
    float bw = ceilf(x1 - x0);
    float bh = ceilf(y1 - y0);
    int gw = (int)bw + 2 * margin;
    int gh = (int)bh + 2 * margin;
    if (gw < 1 || gh < 1 || gw > 4096 || gh > 4096) {
        return;
    }
    int glen = gw * gh * 4;
    uint8_t *buf = malloc((size_t)glen);
    if (!buf) {
        return;
    }
    memset(buf, 0, (size_t)glen);
    // Draw with the glyph origin placed so its ink box sits `margin` px inside the
    // buffer (cnvs_glyph_draw is bitmap space: y up from the bottom).
    cnvs_glyph_draw(font, glyph, (float)margin - x0, (float)margin - y0, buf, gw, gh);
    // CGBitmapContext gives premultiplied RGBA, top row first (the orientation
    // canvas_draw_image_subrect wants); just unpremultiply to straight alpha.
    for (int i = 0; i < glen; i += 4) {
        int a = buf[i + 3];
        if (a > 0 && a < 255) {
            buf[i + 0] = (uint8_t)((buf[i + 0] * 255 + a / 2) / a);
            buf[i + 1] = (uint8_t)((buf[i + 1] * 255 + a / 2) / a);
            buf[i + 2] = (uint8_t)((buf[i + 2] * 255 + a / 2) / a);
        }
    }
    // The buffer maps to a user-space rect: its left edge is `margin` px left of
    // the glyph ink, its top edge `gh - margin + y0` glyph-px above the baseline.
    float dest_x = pen_x + x0 - (float)margin;
    float dest_y = baseline_y - ((float)gh - (float)margin + y0);
    canvas_draw_image_subrect(cv, buf, gw, gh, 0.0f, 0.0f, (float)gw, (float)gh,
                              dest_x, dest_y, (float)gw, (float)gh);
    free(buf);
}

// cnvs_shaped_outline's color-glyph callback context: the canvas plus the size
// the line was shaped at (the capture px scale's numerator).
struct color_glyph_ctx {
    canvas *__single cv;
    float size_px;
};

// cnvs_shaped_outline's color-glyph callback: the context rides along untyped
// (checked C on both ends of the void* hop, so no forge), and each emoji glyph
// composites immediately at its pen position -- interleaved with the outline
// accumulation exactly as the old hand-rolled walk did.  The canonical capture
// draws it (one boundary rasterization per glyph ever, and none at all when it
// came from a replayed bitmap block); only when the cache can't serve does a
// live font handle fall back to the per-draw boundary render, and a handle-less
// run with no capture draws as a blank advance.
static void paint_color_glyph(void *__single ctx, int fid, void *__single font,
                              uint16_t glyph, float pen_x, float baseline_y) {
    struct color_glyph_ctx *__single cc = ctx;
    cnvs_glyph_slot *__single slot =
        cnvs_text_cache_color(&cc->cv->text_cache, fid, font, glyph);
    if (slot) {
        if (slot->cap_w > 0) {
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
static void paint_shaped(canvas *__single cv, cnvs_shaped const *__single s,
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
static void do_text(canvas *__single cv, char const *__counted_by(len) text, int len,
                    float x, float y, float max_width, bool stroke) {
    if (!isfinite(x) || !isfinite(y)) {
        return;  // spec: fillText/strokeText with non-finite coordinates draw
    }            // nothing (and an inf pen would poison every glyph point)
    cnvs_shaped const *__single s = shape_text(cv, text, len);
    if (!s) {
        return;
    }
    float advance = cnvs_shaped_width(s);
    float sx = 1.0f;
    if (isfinite(max_width) && max_width > 0.0f && advance > max_width) {
        sx = max_width / advance;  // condense in x to fit
    }
    float ox = x - text_align_frac(cv->cur.text_align, cv->cur.direction)
                       * advance * sx;
    float oy = y + text_baseline_offset(cv);
    cnvs_mat td = cv->cur.ctm;
    if (sx != 1.0f) {
        // Scale x by sx about the anchor: X' = sx*X + ox*(1-sx), Y' = Y; then the CTM.
        cnvs_mat cond = { .a = sx, .b = 0.0f, .c = 0.0f, .d = 1.0f,
                          .e = ox * (1.0f - sx), .f = 0.0f };
        td = cnvs_mat_mul(cv->cur.ctm, cond);
    }
    paint_shaped(cv, s, ox, oy, td, stroke);
}

float canvas_measure_text(canvas *__single cv, char const *__null_terminated text) {
    int len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    cnvs_shaped const *__single s = shape_text(cv, t, len);
    if (!s) {
        return 0.0f;
    }
    return cnvs_shaped_width(s);
}

canvas_text_metrics canvas_measure_text_full(canvas *__single cv,
                                             char const *__null_terminated text) {
    canvas_text_metrics m;
    memset(&m, 0, sizeof m);  // all-zero if the font/shaping can't be built
    float a = 0.0f, d = 0.0f;
    bool have_vm = canvas_vmetrics(cv, &a, &d);  // cached: no font handle needed
    int len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    cnvs_shaped const *__single s = shape_text(cv, t, len);
    if (have_vm && s) {
        cnvs_text_metrics tm;  // fallback-aware: each glyph in its run's font
        cnvs_shaped_metrics(&cv->text_cache, s, cv->cur.font_size, a, d, &tm);
        m.width = tm.width;
        m.actual_bounding_box_left = tm.actual_left;
        m.actual_bounding_box_right = tm.actual_right;
        m.actual_bounding_box_ascent = tm.actual_ascent;
        m.actual_bounding_box_descent = tm.actual_descent;
        m.font_bounding_box_ascent = tm.font_ascent;
        m.font_bounding_box_descent = tm.font_descent;
        m.em_height_ascent = tm.em_ascent;
        m.em_height_descent = tm.em_descent;
        m.alphabetic_baseline = tm.alphabetic_baseline;
        m.hanging_baseline = tm.hanging_baseline;
        m.ideographic_baseline = tm.ideographic_baseline;
    }
    return m;
}

// The per-canvas text cache, for tests and stats -- declared in cnvs_text.h so
// it stays off the public canvas.h surface (tests include internal headers).
cnvs_text_cache *__single cnvs_canvas_text_cache(canvas *__single cv) {
    return &cv->text_cache;
}

// Recording a text op: first make sure the cache holds everything the op is
// about to use (the family's vmetrics record and the shaped line -- the same
// lookups the draw takes, so this adds no boundary traffic), then serialize
// the not-yet-emitted font/glyph/shape blocks ahead of the op line.  The
// recorded program is self-contained: replay rebuilds the cache from the
// blocks and draws with no text boundary at all.
static void record_text_blocks(canvas *__single cv,
                               char const *__counted_by(len) text, int len) {
    float a = 0.0f, d = 0.0f;
    (void)canvas_vmetrics(cv, &a, &d);  // intern the family + its vmetrics
    (void)shape_text(cv, text, len);    // ensure the line is cached
    cnvs_rec_text_blocks(cv->rec, &cv->text_cache, cv->cur.font_size,
                         cv->cur.direction == CANVAS_DIRECTION_RTL, text, len);
}

void canvas_fill_text_n(canvas *__single cv, char const *__counted_by(len) text,
                        int len, float x, float y) {
    if (cv->rec) {
        record_text_blocks(cv, text, len);
        cnvs_rec_text(cv->rec, "fill_text", x, y, text, len);
    }
    do_text(cv, text, len, x, y, -1.0f, false);
}

void canvas_fill_text(canvas *__single cv, char const *__null_terminated text,
                      float x, float y) {
    int len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    canvas_fill_text_n(cv, t, len, x, y);
}

void canvas_fill_text_max_n(canvas *__single cv, char const *__counted_by(len) text,
                            int len, float x, float y, float max_width) {
    if (cv->rec) {
        record_text_blocks(cv, text, len);
        cnvs_rec_text_max(cv->rec, "fill_text_max", x, y, max_width, text, len);
    }
    do_text(cv, text, len, x, y, max_width, false);
}

void canvas_fill_text_max(canvas *__single cv, char const *__null_terminated text,
                          float x, float y, float max_width) {
    int len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    canvas_fill_text_max_n(cv, t, len, x, y, max_width);
}

void canvas_stroke_text_n(canvas *__single cv, char const *__counted_by(len) text,
                          int len, float x, float y) {
    if (cv->rec) {
        record_text_blocks(cv, text, len);
        cnvs_rec_text(cv->rec, "stroke_text", x, y, text, len);
    }
    do_text(cv, text, len, x, y, -1.0f, true);
}

void canvas_stroke_text(canvas *__single cv, char const *__null_terminated text,
                        float x, float y) {
    int len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    canvas_stroke_text_n(cv, t, len, x, y);
}

void canvas_stroke_text_max_n(canvas *__single cv,
                              char const *__counted_by(len) text,
                              int len, float x, float y, float max_width) {
    if (cv->rec) {
        record_text_blocks(cv, text, len);
        cnvs_rec_text_max(cv->rec, "stroke_text_max", x, y, max_width, text, len);
    }
    do_text(cv, text, len, x, y, max_width, true);
}

void canvas_stroke_text_max(canvas *__single cv, char const *__null_terminated text,
                            float x, float y, float max_width) {
    int len = (int)strlen(text);
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
        float c00 = (float)src[(y0 * sw + x0) * 4 + k];
        float c10 = (float)src[(y0 * sw + x1) * 4 + k];
        float c01 = (float)src[(y1 * sw + x0) * 4 + k];
        float c11 = (float)src[(y1 * sw + x1) * 4 + k];
        float top = c00 + (c10 - c00) * tx;
        float bot = c01 + (c11 - c01) * tx;
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
    int o = (y * sw + x) * 4;
    for (int k = 0; k < 4; k++) {
        out[k] = (float)src[o + k] / 255.0f;
    }
}

void canvas_set_image_smoothing_enabled(canvas *__single cv, bool enabled) {
    if (cv->rec) { cnvs_rec_floats_bool(cv->rec, "set_image_smoothing_enabled", NULL, 0, enabled); }
    cv->cur.image_smoothing_enabled = enabled;
}

void canvas_set_image_smoothing_quality(canvas *__single cv,
                                        canvas_image_smoothing_quality quality) {
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
// alpha * coverage on its way into the premultiplied tile.
static void draw_image_quad(canvas *__single cv,
                            uint8_t const *__counted_by(slen) src, int slen,
                            int sw, int sh, float sx, float sy,
                            float sww, float shh, float dx, float dy,
                            float dw, float dh, bool premul_src) {
    if (!rgba8_dims_ok(sw, sh) || slen < sw * sh * 4 ||
        dw <= 0.0f || dh <= 0.0f) {
        return;
    }
    // The dest rect transforms to a (possibly rotated) device-space quad.
    cnvs_vec2 q[4] = { xf(cv, dx, dy), xf(cv, dx + dw, dy),
                       xf(cv, dx + dw, dy + dh), xf(cv, dx, dy + dh) };
    cbbox b = points_bbox(cv, q, 4, filter_margin(cv));
    if (b.w <= 0 || b.h <= 0 || !ensure_tile(cv, b.w * b.h) ||
        !cnvs_cover_reset(&cv->cover, b.w, b.h)) {
        return;
    }
    cover_edge(cv, b, q[0], q[1]);
    cover_edge(cv, b, q[1], q[2]);
    cover_edge(cv, b, q[2], q[3]);
    cover_edge(cv, b, q[3], q[0]);
    cnvs_cover_resolve(&cv->cover, b.w, b.h, CNVS_NONZERO, cv->cov);

    // Cast the shadow from the destination-quad coverage before sampling the image
    // (the sample loop reuses cv->tile).  As for fills, the shadow is the op's
    // silhouette -- exact for an opaque image; a transparent sprite still shadows
    // its whole quad rather than its alpha shape.
    emit_shadow(cv, b);

    // Blocks of eight pixels, the paint_tile_pattern shape: the SAMPLING stays
    // scalar per lane -- four data-dependent taps at arbitrary source coords --
    // and the f32 fold + premultiply around it run as planes.  Zero-coverage
    // lanes skip their sample and fold to the scalar early-out's transparent
    // black.  The premultiplied-source arm (emoji capture) has no premultiply:
    // every channel just scales by ga * coverage, the scalar arithmetic kept
    // lane for lane.
    cnvs_mat const inv = cnvs_mat_invert(cv->cur.ctm);  // device -> user
    float const ga = cv->cur.global_alpha;
    bool const smooth = cv->cur.image_smoothing_enabled;
    for (int py = 0; py < b.h; py++) {
        float const devy = (float)b.y + (float)py + 0.5f;
        for (int px = 0; px < b.w; px += 8) {
            int const i = py * b.w + px;
            int const k = b.w - px < 8 ? b.w - px : 8;
            foldf8 sr = (foldf8)0.0f, sg = (foldf8)0.0f, sb = (foldf8)0.0f,
                   sa = (foldf8)0.0f;
            for (int l = 0; l < k; l++) {
                if (cv->cov[i + l] == 0) {  // the scalar covf <= 0.0f early-out
                    continue;
                }
                // Device pixel centre -> user space -> dest-rect uv -> source.
                cnvs_vec2 u = cnvs_mat_apply(
                    inv, (cnvs_vec2){ .x = (float)b.x + (float)(px + l) + 0.5f,
                                      .y = devy });
                float fsx = sx + ((u.x - dx) / dw) * sww;
                float fsy = sy + ((u.y - dy) / dh) * shh;
                float s[4];
                if (smooth) {
                    sample_src(src, slen, sw, sh, fsx, fsy, s);
                } else {
                    sample_src_nearest(src, slen, sw, sh, fsx, fsy, s);
                }
                sr[l] = s[0];
                sg[l] = s[1];
                sb[l] = s[2];
                sa[l] = s[3];
            }
            foldf8 const covf = k < 8 ? cover8_k(cv->cov + i, k)
                                      : cover8(cv->cov + i);
            cnvs_px8 out;
            if (premul_src) {
                foldf8 const m = ga * covf;
                out = (cnvs_px8){ __builtin_convertvector(sr * m, cnvs_h8),
                                  __builtin_convertvector(sg * m, cnvs_h8),
                                  __builtin_convertvector(sb * m, cnvs_h8),
                                  __builtin_convertvector(sa * m, cnvs_h8) };
            } else {
                out = shade8(__builtin_convertvector(sr, cnvs_h8),
                             __builtin_convertvector(sg, cnvs_h8),
                             __builtin_convertvector(sb, cnvs_h8),
                             sa * ga * covf);
            }
            if (k < 8) {
                cnvs_px8_store_k(cv->tile + i, k, out);
            } else {
                cnvs_px8_store(cv->tile + i, out);
            }
        }
    }
    apply_filters(cv, b.w, b.h);
    compositor_blend(cv->comp, b.x, b.y, b.w, b.h, cv->tile, cv->cur.composite);
}

void canvas_draw_image_subrect(canvas *__single cv,
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
        int const id = cnvs_rec_image(cv->rec, src, sw * sh * 4, sw, sh);
        if (id >= 0) {
            cnvs_rec_image_floats(cv->rec, "draw_image_subrect", id,
                                  (float[]){ sx, sy, sww, shh, dx, dy, dw, dh },
                                  8);
        }
    }
    draw_image_quad(cv, src, sw * sh * 4, sw, sh, sx, sy, sww, shh,
                    dx, dy, dw, dh, false);
}

void canvas_draw_image(canvas *__single cv,
                       uint8_t const *__counted_by(sw * sh * 4) src,
                       int sw, int sh, float dx, float dy) {
    // Record `draw_image` as itself, then swallow the subrect form it
    // delegates to.  rgba8_dims_ok gates the w*h*4 the block needs (the same
    // predicate the delegate applies before painting).
    if (cv->rec && rgba8_dims_ok(sw, sh)) {
        int const id = cnvs_rec_image(cv->rec, src, sw * sh * 4, sw, sh);
        if (id >= 0) {
            cnvs_rec_image_floats(cv->rec, "draw_image", id,
                                  (float[]){ dx, dy }, 2);
        }
    }
    cnvs_rec_enter(cv->rec);
    canvas_draw_image_subrect(cv, src, sw, sh, 0.0f, 0.0f, (float)sw, (float)sh,
                              dx, dy, (float)sw, (float)sh);
    cnvs_rec_leave(cv->rec);
}

void canvas_draw_image_scaled(canvas *__single cv,
                              uint8_t const *__counted_by(sw * sh * 4) src,
                              int sw, int sh, float dx, float dy,
                              float dw, float dh) {
    if (cv->rec && rgba8_dims_ok(sw, sh)) {
        int const id = cnvs_rec_image(cv->rec, src, sw * sh * 4, sw, sh);
        if (id >= 0) {
            cnvs_rec_image_floats(cv->rec, "draw_image_scaled", id,
                                  (float[]){ dx, dy, dw, dh }, 4);
        }
    }
    cnvs_rec_enter(cv->rec);
    canvas_draw_image_subrect(cv, src, sw, sh, 0.0f, 0.0f, (float)sw, (float)sh,
                              dx, dy, dw, dh);
    cnvs_rec_leave(cv->rec);
}

// One planar block's un-premultiply and 8-bit quantize, in _Float16: the
// divide, clamp, and 255-scale with no f32 anywhere (docs/decisions/
// color-axis.md).  A fully transparent lane (a <= 0) un-premultiplies to all
// zero -- selected bitwise BEFORE the byte convert, so the masked divide's
// inf/NaN lanes never reach the (undefined for them) float->int conversion.
// Every 8-bit edge value still quantizes back exactly (test_image's
// exhaustive round-trip).  Returns finished byte values in [0.5, 255.5) for
// the truncating store seam (cnvs_px8_store_rgba8).
static cnvs_px8 unpremul_quant8(cnvs_px8 p) {
    cnvs_h8 const zero = (cnvs_h8)(_Float16)0.0f, one = (cnvs_h8)(_Float16)1.0f;
    cnvs_m8 opaque = p.a > zero;
    cnvs_px8 u = { p.r / p.a, p.g / p.a, p.b / p.a, p.a };
    u.r = cnvs_h8_sel(opaque, __builtin_elementwise_min(one,
                          __builtin_elementwise_max(zero, u.r)), zero);
    u.g = cnvs_h8_sel(opaque, __builtin_elementwise_min(one,
                          __builtin_elementwise_max(zero, u.g)), zero);
    u.b = cnvs_h8_sel(opaque, __builtin_elementwise_min(one,
                          __builtin_elementwise_max(zero, u.b)), zero);
    u.a = cnvs_h8_sel(opaque, __builtin_elementwise_min(one,
                          __builtin_elementwise_max(zero, u.a)), zero);
    _Float16 const half = (_Float16)0.5f, k255 = (_Float16)255.0f;
    return (cnvs_px8){ u.r * k255 + half, u.g * k255 + half,
                       u.b * k255 + half, u.a * k255 + half };
}

// Read the canvas back as unpremultiplied RGBA8: the compositor returns
// premultiplied pixels, and the un-premultiply and 8-bit quantize happen here,
// eight pixels per step over channel planes with st4 re-interleaving at the
// RGBA8 seam (cnvs_planar.h); the n%8 tail runs the same block gathered.
static void read_unpremul(canvas *__single cv, uint8_t *__counted_by(len) out, int len) {
    if (len < cv->width * cv->height * 4) {
        return;
    }
    int const n = cv->width * cv->height;
    cnvs_premul *__counted_by_or_null(n) buf = malloc((size_t)n * sizeof *buf);
    if (!buf) {
        return;
    }
    compositor_read(cv->comp, buf, n);
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        cnvs_px8_store_rgba8(out + i * 4, unpremul_quant8(cnvs_px8_load(buf + i)));
    }
    if (i < n) {
        int k = n - i;
        cnvs_px8_store_rgba8_k(out + i * 4, k,
                               unpremul_quant8(cnvs_px8_load_k(buf + i, k)));
    }
    free(buf);
}

void canvas_read_rgba(canvas *__single cv, uint8_t *__counted_by(len) out, int len) {
    read_unpremul(cv, out, len);
}

bool canvas_write_png(canvas *__single cv, char const *__null_terminated path) {
    int const len = cv->width * cv->height * 4;
    uint8_t *__counted_by_or_null(len) out = malloc((size_t)len);
    if (!out) {
        return false;
    }
    read_unpremul(cv, out, len);
    bool ok = cnvs_png_write(path, out, cv->width, cv->height);
    free(out);
    return ok;
}

uint8_t *__counted_by_or_null(*len)
canvas_load_png(char const *__null_terminated path,
                int *__single w, int *__single h, int *__single len) {
    return cnvs_png_read(path, w, h, len);
}

void canvas_get_image_data(canvas *__single cv, int x, int y, int w, int h,
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
    read_unpremul(cv, buf, clen);
    cnvs_blit_rgba(out, w, h, 0, 0, buf, cv->width, cv->height, x, y, w, h);
    free(buf);
}

uint8_t *__counted_by_or_null(*len)
canvas_create_image_data(canvas *__single cv, int sw, int sh, int *__single len) {
    (void)cv;  // image data is independent of canvas contents (no colorSpace here)
    if (!rgba8_dims_ok(sw, sh)) {
        *len = 0;
        return NULL;
    }
    // rgba8_dims_ok guarantees sw*sh*4 fits a positive int, so this is overflow-free.
    int n = sw * sh * 4;
    uint8_t *buf = calloc((size_t)n, 1);  // zeroed == transparent black
    if (!buf) {
        *len = 0;
        return NULL;
    }
    *len = n;
    return buf;
}

// Copy the sub-rectangle [sx, sx+sw) x [sy, sy+sh) of the w-wide RGBA8 source onto
// the canvas with the ImageData origin at (dx, dy): source pixel (col, row) lands
// at (dx+col, dy+row).  Overwrites (no blending) and ignores the clip, clipped to
// the canvas.  The caller guarantees the sub-rect lies within the source
// ([0,w] x [0,h]) with sw, sh > 0, and len >= w*h*4.
static void put_image_sub(canvas *__single cv,
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
    int rw = cx1 > cx0 ? (int)(cx1 - cx0) : 0;
    int rh = cy1 > cy0 ? (int)(cy1 - cy0) : 0;
    if (rw <= 0 || rh <= 0 || !ensure_tile(cv, rw * rh)) {
        return;
    }
    // Source column/row of the first painted canvas pixel; col0+px stays in
    // [sx, sx+sw) ⊆ [0,w) and row0+py in [sy, sy+sh) ⊆ [0,h), so si < w*h*4 <= len.
    // Eight pixels per step: ld4 deinterleaves the RGBA8 source into channel
    // planes (cnvs_planar.h), a true _Float16 divide scales each to [0,1] --
    // bit-equal to the old per-channel (float)x / 255.0f-then-narrow for every
    // u8 value (checked exhaustively; f16 division of an exact-in-f16 integer
    // rounds once, where the f32 path rounds twice to the same place) -- and
    // the planar premultiply writes finished tile pixels through st4.
    int col0 = (int)(cx0 - dx);
    int row0 = (int)(cy0 - dy);
    _Float16 const k255 = (_Float16)255.0f;
    for (int py = 0; py < rh; py++) {
        int px = 0;
        for (; px + 8 <= rw; px += 8) {
            int si = ((row0 + py) * w + (col0 + px)) * 4;
            cnvs_px8 p = cnvs_px8_load_rgba8(data + si);
            p = (cnvs_px8){ p.r / k255, p.g / k255, p.b / k255, p.a / k255 };
            cnvs_px8_store(cv->tile + py * rw + px, cnvs_px8_premultiply(p));
        }
        if (px < rw) {  // tail: k < 8 pixels through the same planar block
            int k = rw - px;
            int si = ((row0 + py) * w + (col0 + px)) * 4;
            cnvs_px8 p = cnvs_px8_load_rgba8_k(data + si, k);
            p = (cnvs_px8){ p.r / k255, p.g / k255, p.b / k255, p.a / k255 };
            cnvs_px8_store_k(cv->tile + py * rw + px, k, cnvs_px8_premultiply(p));
        }
    }
    // putImageData overwrites and ignores the clip: composite COPY with the clip
    // open, then restore it.
    compositor_set_clip(cv->comp, NULL, 0);
    compositor_blend(cv->comp, (int)cx0, (int)cy0, rw, rh, cv->tile, COMPOSITOR_COPY);
    compositor_set_clip(cv->comp, cv->cur.clip_mask, cv->cur.clip_len);
}

void canvas_put_image_data(canvas *__single cv,
                           uint8_t const *__counted_by(len) data, int len,
                           int w, int h, int dx, int dy) {
    if (!rgba8_dims_ok(w, h) || len < w * h * 4) {
        return;
    }
    if (cv->rec) {
        // Exactly the w*h*4 pixels the op reads ride the block; the int-typed
        // placement rides the op line.
        int const id = cnvs_rec_image(cv->rec, data, w * h * 4, w, h);
        if (id >= 0) {
            cnvs_rec_image_ints(cv->rec, "put_image_data", id,
                                (int[]){ dx, dy }, 2);
        }
    }
    put_image_sub(cv, data, len, w, dx, dy, 0, 0, w, h);
}

void canvas_put_image_data_dirty(canvas *__single cv,
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
        int const id = cnvs_rec_image(cv->rec, data, w * h * 4, w, h);
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

canvas_path2d *__single canvas_path2d_create(void) {
    return calloc(1, sizeof(canvas_path2d));  // cmds=NULL, len=cap=0 (consistent)
}

void canvas_path2d_destroy(canvas_path2d *__single p) {
    if (!p) {
        return;
    }
    free(p->cmds);
    free(p);
}

static void p2d_push(canvas_path2d *__single p, p2d_cmd c) {
    if (p->len >= p->cap) {
        int nc = cnvs_grow_cap(p->cap, p->len + 1);
        p2d_cmd *ncmds = realloc(p->cmds, (size_t)nc * sizeof *ncmds);
        if (!ncmds) {
            return;  // OOM: drop the command (best-effort, matches the path builders)
        }
        p->cmds = ncmds;
        p->cap = nc;
    }
    p->cmds[p->len] = c;
    p->len += 1;
}

void canvas_path2d_move_to(canvas_path2d *__single p, float x, float y) {
    p2d_push(p, (p2d_cmd){ .op = P2D_MOVE, .a = { x, y } });
}

void canvas_path2d_line_to(canvas_path2d *__single p, float x, float y) {
    p2d_push(p, (p2d_cmd){ .op = P2D_LINE, .a = { x, y } });
}

void canvas_path2d_quadratic_curve_to(canvas_path2d *__single p,
                                      float cpx, float cpy, float x, float y) {
    p2d_push(p, (p2d_cmd){ .op = P2D_QUAD, .a = { cpx, cpy, x, y } });
}

void canvas_path2d_bezier_curve_to(canvas_path2d *__single p, float c1x, float c1y,
                                   float c2x, float c2y, float x, float y) {
    p2d_push(p, (p2d_cmd){ .op = P2D_CUBIC, .a = { c1x, c1y, c2x, c2y, x, y } });
}

void canvas_path2d_arc(canvas_path2d *__single p, float x, float y, float radius,
                       float start_angle, float end_angle, bool anticlockwise) {
    p2d_push(p, (p2d_cmd){ .op = P2D_ARC,
                           .a = { x, y, radius, start_angle, end_angle },
                           .ccw = anticlockwise });
}

void canvas_path2d_ellipse(canvas_path2d *__single p, float x, float y,
                           float rx, float ry, float rotation,
                           float start_angle, float end_angle, bool anticlockwise) {
    p2d_push(p, (p2d_cmd){ .op = P2D_ELLIPSE,
                           .a = { x, y, rx, ry, rotation, start_angle, end_angle },
                           .ccw = anticlockwise });
}

void canvas_path2d_arc_to(canvas_path2d *__single p, float x1, float y1,
                          float x2, float y2, float radius) {
    p2d_push(p, (p2d_cmd){ .op = P2D_ARC_TO, .a = { x1, y1, x2, y2, radius } });
}

void canvas_path2d_rect(canvas_path2d *__single p, float x, float y,
                        float w, float h) {
    p2d_push(p, (p2d_cmd){ .op = P2D_RECT, .a = { x, y, w, h } });
}

void canvas_path2d_round_rect(canvas_path2d *__single p, float x, float y,
                              float w, float h, float radius) {
    p2d_push(p, (p2d_cmd){ .op = P2D_ROUND_RECT, .a = { x, y, w, h, radius } });
}

void canvas_path2d_close_path(canvas_path2d *__single p) {
    p2d_push(p, (p2d_cmd){ .op = P2D_CLOSE });
}

void canvas_path2d_add_path(canvas_path2d *__single dst,
                            canvas_path2d const *__single src) {
    for (int i = 0; i < src->len; i++) {
        p2d_push(dst, src->cmds[i]);
    }
}

// Replay a Path2D's commands into cv->path through the canvas path methods (which
// transform each coordinate by the current CTM and flatten curves at device tol).
static void p2d_replay(canvas *__single cv, canvas_path2d const *__single p) {
    for (int i = 0; i < p->len; i++) {
        p2d_cmd c = p->cmds[i];
        float const *a = c.a;
        switch (c.op) {
            case P2D_MOVE:  canvas_move_to(cv, a[0], a[1]); break;
            case P2D_LINE:  canvas_line_to(cv, a[0], a[1]); break;
            case P2D_QUAD:  canvas_quadratic_curve_to(cv, a[0], a[1], a[2], a[3]); break;
            case P2D_CUBIC: canvas_bezier_curve_to(cv, a[0], a[1], a[2], a[3],
                                                   a[4], a[5]); break;
            case P2D_ARC:   canvas_arc(cv, a[0], a[1], a[2], a[3], a[4], c.ccw); break;
            case P2D_ELLIPSE: canvas_ellipse(cv, a[0], a[1], a[2], a[3], a[4],
                                             a[5], a[6], c.ccw); break;
            case P2D_ARC_TO: canvas_arc_to(cv, a[0], a[1], a[2], a[3], a[4]); break;
            case P2D_RECT:  canvas_rect(cv, a[0], a[1], a[2], a[3]); break;
            case P2D_ROUND_RECT: canvas_round_rect(cv, a[0], a[1], a[2], a[3], a[4]);
                            break;
            case P2D_CLOSE: canvas_close_path(cv); break;
        }
    }
}

void canvas_fill_path(canvas *__single cv, canvas_path2d const *__single p,
                      canvas_fill_rule rule) {
    // Record `fill_path <id> <rule>` against the path's numbered block, then
    // swallow the public path methods p2d_replay drives -- the file keeps the
    // op the caller issued, not the path's expansion into the current path.
    if (cv->rec) {
        int const id = cnvs_rec_path(cv->rec, p);
        if (id >= 0) { cnvs_rec_path_rule(cv->rec, "fill_path", id, rule); }
    }
    cnvs_rec_enter(cv->rec);
    // Replay into a fresh device-space path without touching the current path:
    // copy the current path aside, build, fill, free, restore.
    cnvs_path saved = cv->path;
    cnvs_vec2 saved_user = cv->cur_user;
    cnvs_path_init(&cv->path);
    p2d_replay(cv, p);
    fill_device_path(cv, &cv->path,
                     rule == CANVAS_EVENODD ? CNVS_EVENODD : CNVS_NONZERO);
    cnvs_path_free(&cv->path);
    cv->path = saved;
    cv->cur_user = saved_user;
    cnvs_rec_leave(cv->rec);
}

void canvas_stroke_path(canvas *__single cv, canvas_path2d const *__single p) {
    if (cv->rec) {
        int const id = cnvs_rec_path(cv->rec, p);
        if (id >= 0) { cnvs_rec_path_op(cv->rec, "stroke_path", id); }
    }
    cnvs_rec_enter(cv->rec);
    cnvs_path saved = cv->path;
    cnvs_vec2 saved_user = cv->cur_user;
    cnvs_path_init(&cv->path);
    p2d_replay(cv, p);
    stroke_device_path(cv, &cv->path);
    cnvs_path_free(&cv->path);
    cv->path = saved;
    cv->cur_user = saved_user;
    cnvs_rec_leave(cv->rec);
}

void canvas_clip_path(canvas *__single cv, canvas_path2d const *__single p,
                      canvas_fill_rule rule) {
    if (cv->rec) {
        int const id = cnvs_rec_path(cv->rec, p);
        if (id >= 0) { cnvs_rec_path_rule(cv->rec, "clip_path", id, rule); }
    }
    // Swallow both p2d_replay's path methods and the nested canvas_clip.
    cnvs_rec_enter(cv->rec);
    cnvs_path saved = cv->path;
    cnvs_vec2 saved_user = cv->cur_user;
    cnvs_fill_rule saved_rule = cv->cur.fill_rule;
    cnvs_path_init(&cv->path);
    p2d_replay(cv, p);
    // canvas_clip reads the current fill rule; honour the explicit one here.
    cv->cur.fill_rule = rule == CANVAS_EVENODD ? CNVS_EVENODD : CNVS_NONZERO;
    canvas_clip(cv);
    cv->cur.fill_rule = saved_rule;
    cnvs_path_free(&cv->path);
    cv->path = saved;
    cv->cur_user = saved_user;
    cnvs_rec_leave(cv->rec);
}

bool canvas_is_point_in_path2d(canvas *__single cv, canvas_path2d const *__single p,
                               float x, float y, canvas_fill_rule rule) {
    if (!isfinite(x) || !isfinite(y)) {
        return false;
    }
    cnvs_rec_enter(cv->rec);  // a query: p2d_replay's path methods record nothing
    cnvs_path saved = cv->path;
    cnvs_vec2 saved_user = cv->cur_user;
    cnvs_path_init(&cv->path);
    p2d_replay(cv, p);
    bool inside = path_contains(&cv->path, xf(cv, x, y),
                                rule == CANVAS_EVENODD ? CNVS_EVENODD : CNVS_NONZERO);
    cnvs_path_free(&cv->path);
    cv->path = saved;
    cv->cur_user = saved_user;
    cnvs_rec_leave(cv->rec);
    return inside;
}

bool canvas_is_point_in_stroke_path(canvas *__single cv,
                                    canvas_path2d const *__single p,
                                    float x, float y) {
    if (!isfinite(x) || !isfinite(y)) {
        return false;
    }
    cnvs_rec_enter(cv->rec);  // a query: p2d_replay's path methods record nothing
    cnvs_path saved = cv->path;
    cnvs_vec2 saved_user = cv->cur_user;
    cnvs_path_init(&cv->path);
    p2d_replay(cv, p);
    bool inside = build_stroke_verts(cv, &cv->path) &&
                  stroke_verts_contain(cv, xf(cv, x, y));
    cnvs_path_free(&cv->path);
    cv->path = saved;
    cv->cur_user = saved_user;
    cnvs_rec_leave(cv->rec);
    return inside;
}
