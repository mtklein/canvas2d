#include "canvas2d.h"
#include "canvas2d_image.h"
#include "canvas2d_path2d.h"

#include "canvas2d_blur.h"
#include "canvas2d_blend.h"
#include "canvas2d_context.h"
#include "canvas2d_color.h"
#include "canvas2d_cover.h"
#include "canvas2d_filter.h"
#include "canvas2d_geom.h"
#include "canvas2d_gradient.h"
#include "canvas2d_math.h"
#include "canvas2d_matrix.h"
#include "canvas2d_mem.h"
#include "canvas2d_path.h"
#include "canvas2d_path2d_internal.h"
#include "canvas2d_planar.h"
#include "canvas2d_record.h"
#include "canvas2d_replay.h"
#include "canvas2d_stroke.h"
#include "canvas2d_text.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Maximum chord deviation (device pixels) when flattening curves.
#define CANVAS2D_FLATTEN_TOL 0.25f

// The default font family: the typeface a fresh canvas (and reset/resize) draws
// with until canvas2d_set_font_family changes it.  Carries both Latin and Chinese.
static char const k_font_family[] = "Libian TC";

// struct canvas2d_context, struct canvas2d_state, and the paint helpers (canvas2d_pattern,
// canvas2d_owned_image, the CANVAS2D_* sizing macros) live in canvas2d_context.h so
// the output stages carved into their own TUs (canvas2d_encode.c, canvas2d_imagedata.c)
// share the layout.

bool canvas2d_rgba8_dims_ok(int w, int h) {
    return w > 0 && h > 0 && (int64_t)w * (int64_t)h <= (int64_t)INT_MAX / 4;
}

static canvas2d_vec2 xf(struct canvas2d_context *__single cv, float x, float y);
static void clip_project_contour(canvas2d_mat m, canvas2d_vec2 const *__counted_by(n) src,
                                 int n, bool closed, struct canvas2d_path *__single out);
static void fill_device_path(struct canvas2d_context *__single cv, struct canvas2d_path const *p,
                             enum canvas2d_fill_rule rule);
static int sigma_box_radius(float sigma);
static void shadow_offset_split(float v, int *__single whole, int *__single k256);
struct canvas2d_image;  // reified drawImage source; defined with its API below
static void draw_image_quad(struct canvas2d_context *__single cv,
                            uint8_t const *__counted_by(slen) src, int slen,
                            int sw, int sh, float sx, float sy,
                            float sww, float shh, float dx, float dy,
                            float dw, float dh, bool premul_src,
                            enum canvas2d_color_type src_ct,
                            enum canvas2d_color_space src_space,
                            bool quality_tiers, bool chain_on_demand,
                            struct canvas2d_image const *__single img,
                            canvas2d_mip hi, canvas2d_mip lo, float lt);

// Reset a pattern to empty (no source).  Counts first: a NULL pointer must never
// be paired with a positive count under -fbounds-safety.  An empty pattern stays
// consistent across the state copies that save/restore make.
static void pattern_reset(struct canvas2d_pattern *p) {
    p->len = 0;
    p->data = NULL;
    p->w = 0;
    p->h = 0;
    p->repeat = CANVAS2D_NO_REPEAT;
    p->space = CANVAS2D_CS_SRGB;
    p->to_pattern = canvas2d_mat_identity();
}

// The initial drawing state (Canvas defaults): identity transform, opaque black
// fill/stroke, source-over, 1px miter strokes, no dash, 10px text, open clip.
// Shared by canvas and canvas2d_reset so the two can't drift.  Assigned
// field by field (not an init list): a compound literal of side-effecting calls
// has indeterminate evaluation order, which -fbounds-safety flags for a struct
// carrying a __counted_by member.  Clearing the gradient scratch isn't needed
// (read only when the kind is GRADIENT), but the patterns must be cleared so the
// borrowed-buffer (data, len) pair stays consistent when the state is copied.
static void state_defaults(struct canvas2d_state *s) {
    s->ctm = canvas2d_mat_identity();
    s->fill = canvas2d_unpremul_of(0.0f, 0.0f, 0.0f, 1.0f);
    s->fill_kind = CANVAS2D_PAINT_SOLID;
    pattern_reset(&s->fill_pattern);
    s->stroke = canvas2d_unpremul_of(0.0f, 0.0f, 0.0f, 1.0f);
    s->stroke_kind = CANVAS2D_PAINT_SOLID;
    pattern_reset(&s->stroke_pattern);
    s->global_alpha = 1.0f;
    s->composite = CANVAS2D_OP_SOURCE_OVER;
    s->line_width = 1.0f;
    s->line_join = CANVAS2D_JOIN_MITER;
    s->line_cap = CANVAS2D_CAP_BUTT;
    s->miter_limit = 10.0f;
    s->dash_count = 0;
    s->dash_offset = 0.0f;
    s->font_size = 10.0f;
    s->font_family_len = (int)sizeof k_font_family - 1;  // the default typeface
    memcpy(s->font_family, k_font_family, (size_t)s->font_family_len);
    s->font_weight = 400;                       // CSS normal weight
    s->font_style = CANVAS2D_FONT_STYLE_NORMAL;   // upright
    s->font_kerning = CANVAS2D_FONT_KERNING_AUTO;        // default kerning on
    s->text_rendering = CANVAS2D_TEXT_RENDERING_AUTO;    // default kerning+ligatures
    s->font_variant_caps = CANVAS2D_FONT_VARIANT_CAPS_NORMAL;  // no small caps
    s->font_stretch = CANVAS2D_FONT_STRETCH_NORMAL;            // normal width
    s->lang_len = 0;                            // no language tag
    s->letter_spacing = 0.0f;
    s->word_spacing = 0.0f;
    s->text_align = CANVAS2D_ALIGN_START;
    s->text_baseline = CANVAS2D_BASELINE_ALPHABETIC;
    s->direction = CANVAS2D_DIRECTION_LTR;
    s->image_smoothing_enabled = true;
    s->image_smoothing_quality = CANVAS2D_SMOOTHING_LOW;
    s->shadow_color = canvas2d_unpremul_of(0.0f, 0.0f, 0.0f, 0.0f);  // transparent: off
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

struct canvas2d_context *__single canvas2d(int width, int height,
                               enum canvas2d_color_space space) {
    if (width <= 0 || height <= 0 ||
        width > CANVAS2D_DIM_MAX || height > CANVAS2D_DIM_MAX) {
        return NULL;
    }
    if (space != CANVAS2D_CS_SRGB && space != CANVAS2D_CS_LINEAR_SRGB) {
        return NULL;  // only the compositing spaces are valid here; an
                      // out-of-range value (or CANVAS2D_CS_OKLAB, not a
                      // compositing space) is a caller error
    }
    int const n = width * height;
    canvas2d_premul *__counted_by_or_null(n) target = calloc((size_t)n, sizeof *target);
    if (!target) {
        return NULL;
    }
    struct canvas2d_context *__single cv = calloc(1, sizeof *cv);
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
    canvas2d_path_init(&cv->path);
    canvas2d_path_init(&cv->upath);
    canvas2d_path_init(&cv->pclip);
    canvas2d_path_init(&cv->text_path);
    cv->font = NULL;
    cv->font_built_size = 0.0f;
    cv->font_built_family_len = 0;  // no family built yet; ensure_font's first
                                    // call always rebuilds (family mismatch)
    cv->font_built_weight = 0;
    cv->font_built_style = CANVAS2D_FONT_STYLE_NORMAL;
    cv->font_built_stretch = CANVAS2D_FONT_STRETCH_NORMAL;
    canvas2d_text_cache_init(&cv->text_cache);
    cv->rec = NULL;
    cv->owned_images = NULL;
    return cv;
}

void canvas2d_free(struct canvas2d_context *__single cv) {
    if (!cv) {
        return;
    }
    canvas2d_recorder_end(cv->rec);  // flush and close any active recording
    free(cv->target);
    for (int i = 0; i < cv->nsaved; i++) {
        free(cv->stack[i].filters);
        free(cv->stack[i].clip_mask);
    }
    free(cv->stack);
    free(cv->cur.filters);
    free(cv->cur.clip_mask);
    canvas2d_font_free(cv->font);
    canvas2d_text_cache_reset(&cv->text_cache);  // owned shaped lines + glyph curves
    canvas2d_path_free(&cv->path);
    canvas2d_path_free(&cv->upath);
    canvas2d_path_free(&cv->pclip);
    canvas2d_path_free(&cv->text_path);
    canvas2d_verts_free(&cv->scratch_verts);
    canvas2d_cover_free(&cv->cover);
    free(cv->cov);
    free(cv->tile);
    free(cv->trow);
    free(cv->crow);
    free(cv->shadow_src);
    free(cv->shadow_dst);
    free(cv->blur_tmp);
    free(cv->mips);
    for (struct canvas2d_owned_image *__single n = cv->owned_images; n;) {
        struct canvas2d_owned_image *__single next = n->next;
        free(n->data);
        free(n);
        n = next;
    }
    free(cv);
}

bool canvas2d_canvas_set_working_space(struct canvas2d_context *__single cv,
                                   enum canvas2d_color_space space) {
    // The replay seam for a leading `working_space` line: the canvas is fresh
    // and transparent (the parser only reaches here as the first command), so
    // re-stamping the immutable space is creation-time, not a mid-stream flip.
    if (space == CANVAS2D_CS_SRGB || space == CANVAS2D_CS_LINEAR_SRGB) {
        cv->space = space;
        // Replaying a program onto a recording canvas re-emits the line, so the
        // round trip stays byte-idempotent (the once-per-file latch keeps it
        // from stacking a second line atop the one record_to already wrote).
        canvas2d_rec_working_space(cv->rec, space);
    }
    return true;  // an out-of-range value leaves sRGB; the parser already
}                 // validated the name, so this only guards a direct caller

bool canvas2d_canvas_own_image(struct canvas2d_context *__single cv,
                           uint8_t *__counted_by(len) px, int len) {
    struct canvas2d_owned_image *__single node = calloc(1, sizeof *node);
    if (!node) {
        return false;
    }
    node->data = px;
    node->len = len;
    node->next = cv->owned_images;
    cv->owned_images = node;
    return true;
}

bool canvas2d_record_to(struct canvas2d_context *__single cv, char const *__null_terminated path) {
    canvas2d_recorder_end(cv->rec);  // stop any prior recording first
    cv->rec = canvas2d_recorder_begin(path);
    // A new file holds no blocks yet: forget what any prior recording emitted,
    // so warm cache entries serialize afresh into this one.
    canvas2d_text_cache_unmark(&cv->text_cache);
    // The working space rides the very first line, before any draw -- written
    // unconditionally (sRGB and linear are equal peers, both spelled out).
    // Replay applies it to the fresh canvas before the first colour interns.
    canvas2d_rec_working_space(cv->rec, cv->space);
    return cv->rec != NULL;
}

bool canvas2d_is_context_lost(struct canvas2d_context *__single cv) {
    (void)cv;
    return false;  // a headless renderer owns its backing store; never lost.
}

static bool stack_reserve(struct canvas2d_context *__single cv, int need) {
    if (need <= cv->stack_cap) {
        return true;
    }
    int const newcap = canvas2d_grow_cap(cv->stack_cap, need);
    struct canvas2d_state *ns =
        realloc(cv->stack, (size_t)newcap * sizeof *ns);
    if (!ns) {
        return false;
    }
    cv->stack = ns;
    cv->stack_cap = newcap;
    return true;
}

void canvas2d_save(struct canvas2d_context *__single cv) {
    if (cv->rec) { canvas2d_rec_op(cv->rec, "save"); }
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
        canvas2d_filter *copy = malloc((size_t)n * sizeof *copy);
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

void canvas2d_restore(struct canvas2d_context *__single cv) {
    if (cv->rec) { canvas2d_rec_op(cv->rec, "restore"); }
    if (cv->nsaved > 0) {
        cv->nsaved -= 1;
        free(cv->cur.filters);
        free(cv->cur.clip_mask);
        cv->cur = cv->stack[cv->nsaved];  // adopts the saved clip mask + filters
    }
}

void canvas2d_reset(struct canvas2d_context *__single cv) {
    // Recording continues across a reset: the cleared text cache means the
    // file's font-id space restarts with it (later text re-interns from 0 and
    // re-emits its blocks), which replay mirrors when it executes this line.
    if (cv->rec) { canvas2d_rec_op(cv->rec, "reset"); }
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
    canvas2d_path_reset(&cv->path);
    canvas2d_path_reset(&cv->upath);
    cv->cur_user = (canvas2d_vec2){ .x = 0.0f, .y = 0.0f };
    // Drop the text caches too.  Keeping them would also be correct -- the cache
    // is a pure memo of boundary results, invisible to rendering -- but reset()'s
    // contract is "as if freshly created", and reset is the natural point for a
    // long-lived canvas to shed accumulated memory, so warm entries go (and
    // resize(), which resets, starts its new canvas cold like create() does).
    canvas2d_text_cache_reset(&cv->text_cache);
    // Clear the whole bitmap to transparent black: a destination-out of a
    // unit-alpha splat leaves dst*(1 - 1) = 0 everywhere, with the clip open
    // (state_defaults just dropped the mask; no tile, so a reset can't fail
    // on allocation).
    canvas2d_blend_solid(cv, 0, 0, cv->width, cv->height,
                     (canvas2d_premul){ .r = 0, .g = 0, .b = 0,
                                    .a = (_Float16)1.0f },
                     NULL, NULL, 0, CANVAS2D_OP_DESTINATION_OUT);
}

bool canvas2d_resize(struct canvas2d_context *__single cv, int width, int height) {
    if (width <= 0 || height <= 0 ||
        width > CANVAS2D_DIM_MAX || height > CANVAS2D_DIM_MAX) {
        return false;
    }
    // Build the new-sized target first; on failure leave the canvas intact.
    int const n = width * height;
    canvas2d_premul *__counted_by_or_null(n) nt = calloc((size_t)n, sizeof *nt);
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
    if (cv->rec) { canvas2d_rec_ints(cv->rec, "resize", (int[]){ width, height }, 2); }
    canvas2d_rec_enter(cv->rec);
    // reset() drops the (now wrong-sized) clip masks and saved stack, restores the
    // default state, and clears the fresh bitmap to transparent black.
    canvas2d_reset(cv);
    canvas2d_rec_leave(cv->rec);
    return true;
}

void canvas2d_translate(struct canvas2d_context *__single cv, float tx, float ty) {
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "translate", (float[]){ tx, ty }, 2); }
    cv->cur.ctm = canvas2d_mat_mul(cv->cur.ctm, canvas2d_mat_translate(tx, ty));
}

void canvas2d_scale(struct canvas2d_context *__single cv, float sx, float sy) {
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "scale", (float[]){ sx, sy }, 2); }
    cv->cur.ctm = canvas2d_mat_mul(cv->cur.ctm, canvas2d_mat_scale(sx, sy));
}

void canvas2d_rotate(struct canvas2d_context *__single cv, float radians) {
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "rotate", (float[]){ radians }, 1); }
    cv->cur.ctm = canvas2d_mat_mul(cv->cur.ctm, canvas2d_mat_rotate(radians));
}

void canvas2d_transform(struct canvas2d_context *__single cv,
                      float a, float b, float c, float d, float e, float f) {
    // Recording is unconditional nine numbers (docs/decisions/perspective.md): the
    // affine setter logs its six plus the implicit affine bottom row 0 0 1.
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "transform", (float[]){ a, b, c, d, e, f, 0.0f, 0.0f, 1.0f }, 9); }
    canvas2d_mat const m = { .a = a, .b = b, .c = c, .d = d, .e = e, .f = f,
                         .g = 0.0f, .h = 0.0f, .i = 1.0f };
    cv->cur.ctm = canvas2d_mat_mul(cv->cur.ctm, m);
}

void canvas2d_transform_3x3(struct canvas2d_context *__single cv, float a, float b, float c,
                          float d, float e, float f, float g, float h, float i) {
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "transform", (float[]){ a, b, c, d, e, f, g, h, i }, 9); }
    canvas2d_mat const m = { .a = a, .b = b, .c = c, .d = d, .e = e, .f = f,
                         .g = g, .h = h, .i = i };
    cv->cur.ctm = canvas2d_mat_mul(cv->cur.ctm, m);
}

void canvas2d_set_transform(struct canvas2d_context *__single cv,
                          float a, float b, float c, float d, float e, float f) {
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "set_transform", (float[]){ a, b, c, d, e, f, 0.0f, 0.0f, 1.0f }, 9); }
    cv->cur.ctm = (canvas2d_mat){ .a = a, .b = b, .c = c, .d = d, .e = e, .f = f,
                              .g = 0.0f, .h = 0.0f, .i = 1.0f };
}

void canvas2d_set_transform_3x3(struct canvas2d_context *__single cv, float a, float b, float c,
                              float d, float e, float f, float g, float h, float i) {
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "set_transform", (float[]){ a, b, c, d, e, f, g, h, i }, 9); }
    cv->cur.ctm = (canvas2d_mat){ .a = a, .b = b, .c = c, .d = d, .e = e, .f = f,
                              .g = g, .h = h, .i = i };
}

// Solve the homography mapping the source rect (sx,sy,sw,sh) to four destination
// points, in corner order TL=(x0,y0), TR=(x1,y1), BR=(x2,y2), BL=(x3,y3).  The
// classic two-stage quad map: first the homography from the UNIT square to the
// destination quad (Heckbert's closed form), then pre-compose the affine that
// maps the source rect onto the unit square (a translate+scale, x' = (x-sx)/sw).
// A degenerate destination (sw or sh zero, or collinear corners) leaves the CTM
// untouched.
void canvas2d_set_perspective_quad(struct canvas2d_context *__single cv, float sx, float sy,
                                 float sw, float sh, float x0, float y0,
                                 float x1, float y1, float x2, float y2,
                                 float x3, float y3) {
    if (sw == 0.0f || sh == 0.0f) {
        return;
    }
    // Unit-square -> destination quad.  Vertices in (0,0),(1,0),(1,1),(0,1) order
    // matching TL,TR,BR,BL.  dx1=x1-x2, dx2=x3-x2, sumx=x0-x1+x2-x3 (the standard
    // names); g,h solve the projective row, then the top two rows follow.
    float const dx1 = x1 - x2, dx2 = x3 - x2;
    float const dy1 = y1 - y2, dy2 = y3 - y2;
    float const sX = x0 - x1 + x2 - x3;
    float const sY = y0 - y1 + y2 - y3;
    canvas2d_mat unit;
    float const den = dx1 * dy2 - dx2 * dy1;
    if (den == 0.0f) {
        return;  // collinear destination corners: no homography
    }
    float const g = (sX * dy2 - dx2 * sY) / den;
    float const h = (dx1 * sY - sX * dy1) / den;
    // Columns: a,b,g | c,d,h | e,f,i, applied to (u,v,1).
    unit = (canvas2d_mat){
        .a = x1 - x0 + g * x1,  .b = y1 - y0 + g * y1,  .g = g,
        .c = x3 - x0 + h * x3,  .d = y3 - y0 + h * y3,  .h = h,
        .e = x0,                .f = y0,                .i = 1.0f,
    };
    // Source rect -> unit square: u = (x - sx)/sw, v = (y - sy)/sh.
    canvas2d_mat const to_unit = {
        .a = 1.0f / sw, .c = 0.0f,       .e = -sx / sw,
        .b = 0.0f,      .d = 1.0f / sh,  .f = -sy / sh,
        .g = 0.0f,      .h = 0.0f,       .i = 1.0f,
    };
    canvas2d_mat const m = canvas2d_mat_mul(unit, to_unit);
    if (cv->rec) {
        canvas2d_rec_floats(cv->rec, "set_transform",
                        (float[]){ m.a, m.b, m.c, m.d, m.e, m.f, m.g, m.h, m.i }, 9);
    }
    cv->cur.ctm = m;
}

void canvas2d_reset_transform(struct canvas2d_context *__single cv) {
    if (cv->rec) { canvas2d_rec_op(cv->rec, "reset_transform"); }
    cv->cur.ctm = canvas2d_mat_identity();
}

canvas2d_matrix canvas2d_get_transform(struct canvas2d_context *__single cv) {
    canvas2d_mat const m = cv->cur.ctm;
    return (canvas2d_matrix){ .a = m.a, .b = m.b, .c = m.c,
                            .d = m.d, .e = m.e, .f = m.f };
}

// Intern one boundary colour into the canvas's working space -- the single
// entry-side transfer gate every colour the API takes flows through (fill/stroke/
// shadow/drop-shadow colours and gradient stops; image and pattern pixels intern
// at their own seams).  `space` names the space the INPUT (r,g,b) are given in;
// the result lands in the canvas's WORKING space, in that space's native
// encoding.  alpha is a coverage coordinate, never a colour, so it clamps to
// [0,1] on every path (the premultiply and readback both assume it in range).
//
//   CANVAS2D_CS_SRGB -- the (r,g,b) ARE encoded sRGB.  On an sRGB canvas it is a
//   literal pass-through: clamp01 each channel, no transfer.  On a linear canvas
//   the RGB decode sRGB->linear (canvas2d_srgb_to_linear, the odd extension, total
//   over R) with NO [0,1] clamp -- an extended (out-of-gamut) input must
//   propagate, and the clamp would crush it.
//
//   CANVAS2D_CS_LINEAR_SRGB -- the (r,g,b) ARE linear sRGB.  To a linear canvas,
//   stored as-is (no rgb clamp, extended values propagate).  To an sRGB canvas,
//   encode linear->sRGB then clamp01 into the [0,1] encoded range.
//
//   CANVAS2D_CS_OKLAB -- the (r,g,b) are an Oklab (L,a,b) triple.  Convert to
//   linear sRGB first (canvas2d_oklab_to_linear_srgb), then the LINEAR_SRGB handling.
//
// The transfer / Oklab math runs in f32 (the canvas2d_color kernels) and narrows
// once at canvas2d_unpremul_of.  The recorder logs the RAW input floats upstream of
// this with their space token; replay re-interns through the same gate.
static canvas2d_unpremul intern_color(struct canvas2d_context *__single cv,
                                  enum canvas2d_color_space space,
                                  float r, float g, float b, float a) {
    if (space == CANVAS2D_CS_SRGB) {
        // Encoded-sRGB input: a pass-through on an sRGB canvas, a decode on a
        // linear one.
        if (cv->space == CANVAS2D_CS_LINEAR_SRGB) {
            return canvas2d_unpremul_of(canvas2d_srgb_to_linear(r), canvas2d_srgb_to_linear(g),
                                    canvas2d_srgb_to_linear(b), canvas2d_clamp01(a));
        }
        return canvas2d_unpremul_of(canvas2d_clamp01(r), canvas2d_clamp01(g),
                                canvas2d_clamp01(b), canvas2d_clamp01(a));
    }

    // Reduce the input to linear sRGB (r,g,b) first; OKLAB is one convert away.
    canvas2d_rgb lin = { .r = r, .g = g, .b = b };
    if (space == CANVAS2D_CS_OKLAB) {
        lin = canvas2d_oklab_to_linear_srgb((canvas2d_oklab){ .L = r, .a = g, .b = b });
    }
    float const ca = canvas2d_clamp01(a);
    if (cv->space == CANVAS2D_CS_LINEAR_SRGB) {
        // Linear working canvas: store the linear values directly, no rgb clamp.
        return canvas2d_unpremul_of(lin.r, lin.g, lin.b, ca);
    }
    // sRGB working canvas: encode linear->sRGB, then clamp into [0,1].
    canvas2d_rgb const enc = canvas2d_rgb_linear_to_srgb(lin);
    return canvas2d_unpremul_of(canvas2d_clamp01(enc.r), canvas2d_clamp01(enc.g),
                            canvas2d_clamp01(enc.b), ca);
}

// Convert one sampled RGB triple from `src` space into the canvas working space,
// in place (s[3], the alpha, is untouched).  The sampling counterpart to
// intern_color: the same pivot through linear sRGB and the same working-space
// encoding (linear canvas stores linear with no rgb clamp so extended values
// propagate; sRGB canvas encodes then clamps), but applied to a colour read out
// of an image rather than to an API input.  Callers skip this when src equals
// cv->space, so the matched path stays bit-exact.  The transfer is nonlinear, so
// a premultiplied sample is unpremultiplied before the call and re-premultiplied
// after.
static void sample_to_working(struct canvas2d_context const *__single cv,
                              enum canvas2d_color_space src,
                              float *__counted_by(4) s) {
    canvas2d_rgb lin = { .r = s[0], .g = s[1], .b = s[2] };
    switch (src) {
        case CANVAS2D_CS_SRGB:        lin = canvas2d_rgb_srgb_to_linear(lin); break;
        case CANVAS2D_CS_LINEAR_SRGB: break;  // already linear sRGB
        case CANVAS2D_CS_OKLAB:
            lin = canvas2d_oklab_to_linear_srgb(
                (canvas2d_oklab){ .L = s[0], .a = s[1], .b = s[2] });
            break;
    }
    if (cv->space == CANVAS2D_CS_LINEAR_SRGB) {
        s[0] = lin.r;
        s[1] = lin.g;
        s[2] = lin.b;
        return;
    }
    canvas2d_rgb const enc = canvas2d_rgb_linear_to_srgb(lin);
    s[0] = canvas2d_clamp01(enc.r);
    s[1] = canvas2d_clamp01(enc.g);
    s[2] = canvas2d_clamp01(enc.b);
}

void canvas2d_set_fill_rgba(struct canvas2d_context *__single cv, enum canvas2d_color_space space,
                          float r, float g, float b, float a) {
    if (cv->rec) { canvas2d_rec_floats_cs(cv->rec, "set_fill_rgba", (float[]){ r, g, b, a }, 4, space); }
    cv->cur.fill = intern_color(cv, space, r, g, b, a);
    cv->cur.fill_kind = CANVAS2D_PAINT_SOLID;
}

// Average CTM scale, used to bake user-space radii into device space.
static float ctm_scale(canvas2d_mat m) {
    float const det = m.a * m.d - m.b * m.c;
    return sqrtf(fabsf(det));
}

// Initialise a gradient struct in device space (the CTM is baked in now); the
// caller sets the matching paint kind to GRADIENT.
// Every grad_set_* stamps the canvas's (immutable) working space onto the
// gradient so its interp path can take stops working<->linear at eval time, and
// stores the caller-chosen interpolation knobs (space + alpha) directly -- the
// interpolation is required at creation, with no favoured default among the
// peer spaces or alpha modes.
// Stamp the perspective fields shared by every gradient kind: the user-space
// def (raw args, no CTM baked in) and the device->user inverse, plus the persp
// flag set when the creating CTM is non-affine.  On the affine path these go
// unread (the device-space p0/p1/r0/r1/angle solver runs unchanged).
static void grad_set_persp(struct canvas2d_context *__single cv, struct canvas2d_gradient *gr,
                           canvas2d_vec2 up0, canvas2d_vec2 up1, float ur0, float ur1,
                           float uangle) {
    gr->persp = !canvas2d_mat_is_affine(cv->cur.ctm);
    gr->up0 = up0;
    gr->up1 = up1;
    gr->ur0 = ur0;
    gr->ur1 = ur1;
    gr->uangle = uangle;
    gr->to_user = canvas2d_mat_invert(cv->cur.ctm);
}

static void grad_set_linear(struct canvas2d_context *__single cv, struct canvas2d_gradient *gr,
                            enum canvas2d_color_space interp_space,
                            enum canvas2d_alpha_type interp_alpha,
                            float x0, float y0, float x1, float y1) {
    gr->kind = CANVAS2D_GRAD_LINEAR;
    gr->p0 = xf(cv, x0, y0);
    gr->p1 = xf(cv, x1, y1);
    gr->r0 = 0.0f;
    gr->r1 = 0.0f;
    gr->angle = 0.0f;
    gr->stop_count = 0;
    gr->interp = interp_space;
    gr->interp_alpha = interp_alpha;
    gr->space = cv->space;
    grad_set_persp(cv, gr, (canvas2d_vec2){ x0, y0 }, (canvas2d_vec2){ x1, y1 },
                   0.0f, 0.0f, 0.0f);
}

static void grad_set_radial(struct canvas2d_context *__single cv, struct canvas2d_gradient *gr,
                            enum canvas2d_color_space interp_space,
                            enum canvas2d_alpha_type interp_alpha, float x0,
                            float y0, float r0, float x1, float y1, float r1) {
    float const s = ctm_scale(cv->cur.ctm);
    gr->kind = CANVAS2D_GRAD_RADIAL;
    gr->p0 = xf(cv, x0, y0);
    gr->p1 = xf(cv, x1, y1);
    gr->r0 = r0 * s;
    gr->r1 = r1 * s;
    gr->angle = 0.0f;
    gr->stop_count = 0;
    gr->interp = interp_space;
    gr->interp_alpha = interp_alpha;
    gr->space = cv->space;
    grad_set_persp(cv, gr, (canvas2d_vec2){ x0, y0 }, (canvas2d_vec2){ x1, y1 },
                   r0, r1, 0.0f);
}

// Rotation angle (radians) of the CTM's x-axis basis, for baking a conic
// gradient's start angle into device space.  Exact for similarity transforms;
// skew / non-uniform scale distort the angles (as they do the radial circles).
static float ctm_rotation(canvas2d_mat m) {
    return atan2f(m.b, m.a);
}

static void grad_set_conic(struct canvas2d_context *__single cv, struct canvas2d_gradient *gr,
                           enum canvas2d_color_space interp_space,
                           enum canvas2d_alpha_type interp_alpha,
                           float start_angle, float cx, float cy) {
    gr->kind = CANVAS2D_GRAD_CONIC;
    gr->p0 = xf(cv, cx, cy);  // centre in device space
    gr->p1 = gr->p0;
    gr->r0 = 0.0f;
    gr->r1 = 0.0f;
    gr->angle = start_angle + ctm_rotation(cv->cur.ctm);
    gr->stop_count = 0;
    gr->interp = interp_space;
    gr->interp_alpha = interp_alpha;
    gr->space = cv->space;
    grad_set_persp(cv, gr, (canvas2d_vec2){ cx, cy }, (canvas2d_vec2){ cx, cy },
                   0.0f, 0.0f, start_angle);
}

// Configure `p` to tile `src` (borrowed) with `repeat`, pinned in device space
// via the inverse of the current CTM.  The (data, len) pair is set together so
// -fbounds-safety can verify the __counted_by(len) invariant: src is itself
// __counted_by(w*h*4), exactly the new len.
static void pattern_set(struct canvas2d_context *__single cv, struct canvas2d_pattern *p,
                        uint8_t const *__counted_by(w * h * 4) src, int w, int h,
                        enum canvas2d_pattern_repeat repeat,
                        enum canvas2d_color_space space) {
    p->data = src;
    p->len = w * h * 4;
    p->w = w;
    p->h = h;
    p->repeat = repeat;
    p->space = space;
    p->to_pattern = canvas2d_mat_invert(cv->cur.ctm);  // device -> pattern image space
}

void canvas2d_set_fill_linear_gradient(struct canvas2d_context *__single cv,
                                     enum canvas2d_color_space interp_space,
                                     enum canvas2d_alpha_type interp_alpha,
                                     float x0, float y0, float x1, float y1) {
    if (cv->rec) { canvas2d_rec_gradient(cv->rec, "set_fill_linear_gradient", interp_space, interp_alpha, (float[]){ x0, y0, x1, y1 }, 4); }
    grad_set_linear(cv, &cv->cur.fill_grad, interp_space, interp_alpha, x0, y0, x1, y1);
    cv->cur.fill_kind = CANVAS2D_PAINT_GRADIENT;
}

void canvas2d_set_fill_radial_gradient(struct canvas2d_context *__single cv,
                                     enum canvas2d_color_space interp_space,
                                     enum canvas2d_alpha_type interp_alpha,
                                     float x0, float y0,
                                     float r0, float x1, float y1, float r1) {
    if (cv->rec) { canvas2d_rec_gradient(cv->rec, "set_fill_radial_gradient", interp_space, interp_alpha, (float[]){ x0, y0, r0, x1, y1, r1 }, 6); }
    grad_set_radial(cv, &cv->cur.fill_grad, interp_space, interp_alpha, x0, y0, r0, x1, y1, r1);
    cv->cur.fill_kind = CANVAS2D_PAINT_GRADIENT;
}

void canvas2d_set_fill_conic_gradient(struct canvas2d_context *__single cv,
                                    enum canvas2d_color_space interp_space,
                                    enum canvas2d_alpha_type interp_alpha,
                                    float start_angle, float x, float y) {
    if (cv->rec) { canvas2d_rec_gradient(cv->rec, "set_fill_conic_gradient", interp_space, interp_alpha, (float[]){ start_angle, x, y }, 3); }
    grad_set_conic(cv, &cv->cur.fill_grad, interp_space, interp_alpha, start_angle, x, y);
    cv->cur.fill_kind = CANVAS2D_PAINT_GRADIENT;
}

void canvas2d_add_fill_color_stop(struct canvas2d_context *__single cv,
                                enum canvas2d_color_space space, float offset,
                                float r, float g, float b, float a) {
    if (cv->rec) { canvas2d_rec_floats_cs(cv->rec, "add_fill_color_stop", (float[]){ offset, r, g, b, a }, 5, space); }
    canvas2d_gradient_add_stop(&cv->cur.fill_grad, canvas2d_clamp01(offset),
                           intern_color(cv, space, r, g, b, a));
}

void canvas2d_set_fill_pattern(struct canvas2d_context *__single cv, enum canvas2d_color_space space,
                             uint8_t const *__counted_by(w * h * 4) src,
                             int w, int h, enum canvas2d_pattern_repeat repeat) {
    if (!canvas2d_rgba8_dims_ok(w, h)) {
        return;  // invalid dimensions: leave the fill paint unchanged
    }
    if (cv->rec) {
        // The pattern pixels ride a content-deduped image block (its colour
        // space included); when the block can't be carried (caps/OOM) the op
        // line is skipped with it.
        int const id = canvas2d_rec_image(cv->rec, src, w * h * 4, w, h,
                                      CANVAS2D_COLOR_UNORM8, CANVAS2D_ALPHA_UNPREMUL,
                                      space);
        if (id >= 0) { canvas2d_rec_pattern(cv->rec, "set_fill_pattern", id, repeat); }
    }
    // The source pixels are sampled in `space` and converted to the working
    // space on deposit (paint_tile_pattern); the tag rides the pattern state.
    pattern_set(cv, &cv->cur.fill_pattern, src, w, h, repeat, space);
    cv->cur.fill_kind = CANVAS2D_PAINT_PATTERN;
}

void canvas2d_set_stroke_linear_gradient(struct canvas2d_context *__single cv,
                                       enum canvas2d_color_space interp_space,
                                       enum canvas2d_alpha_type interp_alpha,
                                       float x0, float y0, float x1, float y1) {
    if (cv->rec) { canvas2d_rec_gradient(cv->rec, "set_stroke_linear_gradient", interp_space, interp_alpha, (float[]){ x0, y0, x1, y1 }, 4); }
    grad_set_linear(cv, &cv->cur.stroke_grad, interp_space, interp_alpha, x0, y0, x1, y1);
    cv->cur.stroke_kind = CANVAS2D_PAINT_GRADIENT;
}

void canvas2d_set_stroke_radial_gradient(struct canvas2d_context *__single cv,
                                       enum canvas2d_color_space interp_space,
                                       enum canvas2d_alpha_type interp_alpha,
                                       float x0, float y0,
                                       float r0, float x1, float y1, float r1) {
    if (cv->rec) { canvas2d_rec_gradient(cv->rec, "set_stroke_radial_gradient", interp_space, interp_alpha, (float[]){ x0, y0, r0, x1, y1, r1 }, 6); }
    grad_set_radial(cv, &cv->cur.stroke_grad, interp_space, interp_alpha, x0, y0, r0, x1, y1, r1);
    cv->cur.stroke_kind = CANVAS2D_PAINT_GRADIENT;
}

void canvas2d_set_stroke_conic_gradient(struct canvas2d_context *__single cv,
                                      enum canvas2d_color_space interp_space,
                                      enum canvas2d_alpha_type interp_alpha,
                                      float start_angle, float x, float y) {
    if (cv->rec) { canvas2d_rec_gradient(cv->rec, "set_stroke_conic_gradient", interp_space, interp_alpha, (float[]){ start_angle, x, y }, 3); }
    grad_set_conic(cv, &cv->cur.stroke_grad, interp_space, interp_alpha, start_angle, x, y);
    cv->cur.stroke_kind = CANVAS2D_PAINT_GRADIENT;
}

void canvas2d_add_stroke_color_stop(struct canvas2d_context *__single cv,
                                  enum canvas2d_color_space space, float offset,
                                  float r, float g, float b, float a) {
    if (cv->rec) { canvas2d_rec_floats_cs(cv->rec, "add_stroke_color_stop", (float[]){ offset, r, g, b, a }, 5, space); }
    canvas2d_gradient_add_stop(&cv->cur.stroke_grad, canvas2d_clamp01(offset),
                           intern_color(cv, space, r, g, b, a));
}

void canvas2d_set_stroke_pattern(struct canvas2d_context *__single cv, enum canvas2d_color_space space,
                               uint8_t const *__counted_by(w * h * 4) src,
                               int w, int h, enum canvas2d_pattern_repeat repeat) {
    if (!canvas2d_rgba8_dims_ok(w, h)) {
        return;
    }
    if (cv->rec) {
        int const id = canvas2d_rec_image(cv->rec, src, w * h * 4, w, h,
                                      CANVAS2D_COLOR_UNORM8, CANVAS2D_ALPHA_UNPREMUL,
                                      space);
        if (id >= 0) { canvas2d_rec_pattern(cv->rec, "set_stroke_pattern", id, repeat); }
    }
    // Sampled in `space`, converted to the working space on deposit (the fill
    // twin's note).
    pattern_set(cv, &cv->cur.stroke_pattern, src, w, h, repeat, space);
    cv->cur.stroke_kind = CANVAS2D_PAINT_PATTERN;
}

void canvas2d_set_global_alpha(struct canvas2d_context *__single cv, float alpha) {
    cv->cur.global_alpha = canvas2d_clamp01(alpha);  // NaN/out-of-range -> [0,1]
    // Record the stored (sanitized, finite) value, not the raw argument, so the
    // recorded op always replays and matches the applied state.
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "set_global_alpha", (float[]){ cv->cur.global_alpha }, 1); }
}

void canvas2d_set_global_composite_operation(struct canvas2d_context *__single cv,
                                           enum canvas2d_composite_op op) {
    if ((int)op < 0 || (int)op >= CANVAS2D_BLEND_MODE_COUNT) {
        return;
    }
    if (cv->rec) { canvas2d_rec_composite(cv->rec, op); }
    cv->cur.composite = op;
}

void canvas2d_set_shadow_color_rgba(struct canvas2d_context *__single cv,
                                  enum canvas2d_color_space space,
                                  float r, float g, float b, float a) {
    if (cv->rec) { canvas2d_rec_floats_cs(cv->rec, "set_shadow_color_rgba", (float[]){ r, g, b, a }, 4, space); }
    cv->cur.shadow_color = intern_color(cv, space, r, g, b, a);
}

void canvas2d_set_shadow_blur(struct canvas2d_context *__single cv, float blur) {
    if (isfinite(blur) && blur >= 0.0f) {  // spec: ignore negative / non-finite
        // The hook sits inside the guard: an ignored call records nothing
        // (and %.9g of a non-finite would not reparse anyway).
        if (cv->rec) { canvas2d_rec_floats(cv->rec, "set_shadow_blur", (float[]){ blur }, 1); }
        cv->cur.shadow_blur = blur;
    }
}

void canvas2d_set_shadow_offset_x(struct canvas2d_context *__single cv, float offset) {
    if (isfinite(offset)) {
        if (cv->rec) { canvas2d_rec_floats(cv->rec, "set_shadow_offset_x", (float[]){ offset }, 1); }
        cv->cur.shadow_offset_x = offset;
    }
}

void canvas2d_set_shadow_offset_y(struct canvas2d_context *__single cv, float offset) {
    if (isfinite(offset)) {
        if (cv->rec) { canvas2d_rec_floats(cv->rec, "set_shadow_offset_y", (float[]){ offset }, 1); }
        cv->cur.shadow_offset_y = offset;
    }
}

void canvas2d_set_filter_none(struct canvas2d_context *__single cv) {
    if (cv->rec) { canvas2d_rec_op(cv->rec, "set_filter_none"); }
    free(cv->cur.filters);
    cv->cur.filter_count = 0;
    cv->cur.filters = NULL;
}

// Append one compiled function to the state's filter list.  The list grows by
// exactly one (filter lists stay a few entries long); on allocation failure the
// call is dropped, leaving the list as it was (best-effort, like the path
// builders).  Non-finite amounts never reach here -- each canvas2d_add_filter_*
// ignores those calls outright, per spec.
static void filter_append(struct canvas2d_context *__single cv, canvas2d_filter f) {
    int const n = cv->cur.filter_count;
    canvas2d_filter *nf = realloc(cv->cur.filters, ((size_t)n + 1) * sizeof *nf);
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

void canvas2d_add_filter_blur(struct canvas2d_context *__single cv, float px) {
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
        if (cv->rec) { canvas2d_rec_floats(cv->rec, "add_filter_blur", (float[]){ px }, 1); }
        filter_append(cv, canvas2d_filter_blur(r));
    }
}

void canvas2d_add_filter_brightness(struct canvas2d_context *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { canvas2d_rec_floats(cv->rec, "add_filter_brightness", (float[]){ amount }, 1); }
        filter_append(cv, canvas2d_filter_brightness(clamp_lo(amount)));
    }
}

void canvas2d_add_filter_contrast(struct canvas2d_context *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { canvas2d_rec_floats(cv->rec, "add_filter_contrast", (float[]){ amount }, 1); }
        filter_append(cv, canvas2d_filter_contrast(clamp_lo(amount)));
    }
}

void canvas2d_add_filter_drop_shadow(struct canvas2d_context *__single cv,
                                   enum canvas2d_color_space space,
                                   float dx, float dy,
                                   float blur, float r, float g, float b,
                                   float a) {
    if (!isfinite(dx) || !isfinite(dy) || !isfinite(blur) || blur < 0.0f) {
        return;  // spec: ignore an unparseable (or negative-blur) drop-shadow
    }
    if (!(canvas2d_clamp01(a) > 0.0f)) {
        return;  // a fully transparent shadow composites as nothing
    }
    if (cv->rec) {
        // dx/dy/blur are guarded finite; the colour rides raw, like set_fill_rgba
        // and set_shadow_color -- so an extended (HDR / wide-gamut) shadow colour
        // on a linear canvas round-trips (in-range values record identically).
        canvas2d_rec_floats_cs(cv->rec, "add_filter_drop_shadow",
                           (float[]){ dx, dy, blur, r, g, b, a }, 7, space);
    }
    // The offsets split onto the 1/256th-px grid (shadow_offset_split, as for
    // shadowOffset{X,Y}); blur IS the Gaussian's stdDev, like blur() -- but
    // unlike blur(), radius 0 is a real entry (a sharp shadow, not identity).
    int dxw, dxk, dyw, dyk;
    shadow_offset_split(dx, &dxw, &dxk);
    shadow_offset_split(dy, &dyw, &dyk);
    // The shadow tint composites into the tile in the working space, so it
    // interns through the same gate as every other boundary colour, in whatever
    // space the caller named.  The recorder above logged the raw input.
    canvas2d_unpremul const tint = intern_color(cv, space, r, g, b, a);
    filter_append(cv, canvas2d_filter_drop_shadow(
                          dxw, dxk, dyw, dyk,
                          sigma_box_radius(blur), (float)tint.r, (float)tint.g,
                          (float)tint.b, (float)tint.a));
}

void canvas2d_add_filter_grayscale(struct canvas2d_context *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { canvas2d_rec_floats(cv->rec, "add_filter_grayscale", (float[]){ amount }, 1); }
        filter_append(cv, canvas2d_filter_grayscale(canvas2d_clamp01(amount)));
    }
}

void canvas2d_add_filter_hue_rotate(struct canvas2d_context *__single cv, float radians) {
    if (isfinite(radians)) {
        if (cv->rec) { canvas2d_rec_floats(cv->rec, "add_filter_hue_rotate", (float[]){ radians }, 1); }
        filter_append(cv, canvas2d_filter_hue_rotate(radians));
    }
}

void canvas2d_add_filter_invert(struct canvas2d_context *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { canvas2d_rec_floats(cv->rec, "add_filter_invert", (float[]){ amount }, 1); }
        filter_append(cv, canvas2d_filter_invert(canvas2d_clamp01(amount)));
    }
}

void canvas2d_add_filter_opacity(struct canvas2d_context *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { canvas2d_rec_floats(cv->rec, "add_filter_opacity", (float[]){ amount }, 1); }
        filter_append(cv, canvas2d_filter_opacity(canvas2d_clamp01(amount)));
    }
}

void canvas2d_add_filter_saturate(struct canvas2d_context *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { canvas2d_rec_floats(cv->rec, "add_filter_saturate", (float[]){ amount }, 1); }
        filter_append(cv, canvas2d_filter_saturate(clamp_lo(amount)));
    }
}

void canvas2d_add_filter_sepia(struct canvas2d_context *__single cv, float amount) {
    if (isfinite(amount)) {
        if (cv->rec) { canvas2d_rec_floats(cv->rec, "add_filter_sepia", (float[]){ amount }, 1); }
        filter_append(cv, canvas2d_filter_sepia(canvas2d_clamp01(amount)));
    }
}

static canvas2d_vec2 xf(struct canvas2d_context *__single cv, float x, float y) {
    return canvas2d_mat_apply(cv->cur.ctm, (canvas2d_vec2){ .x = x, .y = y });
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
static void emit_shadow(struct canvas2d_context *__single cv, cbbox b,
                        bool from_tile, bool with_cov, _Float16 base);

static cbbox points_bbox(struct canvas2d_context *__single cv,
                         canvas2d_vec2 const *__counted_by(n) pts, int n, int margin) {
    if (n <= 0) {
        return (cbbox){ .x = 0, .y = 0, .w = 0, .h = 0 };
    }
    float minx = pts[0].x, maxx = pts[0].x, miny = pts[0].y, maxy = pts[0].y;
    for (int i = 1; i < n; i++) {
        canvas2d_vec2 const p = pts[i];
        minx = p.x < minx ? p.x : minx;
        maxx = p.x > maxx ? p.x : maxx;
        miny = p.y < miny ? p.y : miny;
        maxy = p.y > maxy ? p.y : maxy;
    }
    float const m = (float)margin;
    float fx0 = floorf(minx) - m, fy0 = floorf(miny) - m;
    float fx1 = ceilf(maxx) + m, fy1 = ceilf(maxy) + m;
    int x0 = canvas2d_f2i(fx0), y0 = canvas2d_f2i(fy0), x1 = canvas2d_f2i(fx1), y1 = canvas2d_f2i(fy1);
    if (x0 < 0)          { x0 = 0; }
    if (y0 < 0)          { y0 = 0; }
    if (x1 > cv->width)  { x1 = cv->width; }
    if (y1 > cv->height) { y1 = cv->height; }
    cbbox b = { .x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0 };
    if (b.w < 0) { b.w = 0; }
    if (b.h < 0) { b.h = 0; }
    return b;
}

bool canvas2d_ensure_tile(struct canvas2d_context *__single cv, int npix) {
    if (npix > cv->cov_cap) {
        uint8_t *nc = realloc(cv->cov, (size_t)npix);
        if (!nc) {
            return false;
        }
        cv->cov = nc;
        cv->cov_cap = npix;
    }
    if (npix > cv->tile_cap) {  // one premultiplied pixel per cell
        canvas2d_premul *nt = realloc(cv->tile, (size_t)npix * sizeof *nt);
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
static bool ensure_grad_rows(struct canvas2d_context *__single cv, int w) {
    if (w > cv->trow_cap) {
        float *nr = realloc(cv->trow, (size_t)w * sizeof *nr);
        if (!nr) {
            return false;
        }
        cv->trow = nr;
        cv->trow_cap = w;
    }
    if (w > cv->crow_cap) {
        canvas2d_unpremul *nc = realloc(cv->crow, (size_t)w * sizeof *nc);
        if (!nc) {
            return false;
        }
        cv->crow = nc;
        cv->crow_cap = w;
    }
    return true;
}

// Grow the filter-blur scratch tile to at least npix pixels.
static bool ensure_blur_tmp(struct canvas2d_context *__single cv, int npix) {
    if (npix > cv->blur_tmp_cap) {
        canvas2d_premul *nt = realloc(cv->blur_tmp, (size_t)npix * sizeof *nt);
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
static int filter_margin(struct canvas2d_context const *__single cv) {
    int m = 0;
    for (int i = 0; i < cv->cur.filter_count; i++) {
        canvas2d_filter const f = cv->cur.filters[i];
        m += 3 * f.blur;
        if (f.shadow) {
            // A fractional offset spills one pixel past its floor.
            int const ax = (f.dx < 0 ? -f.dx : f.dx) + (f.kx ? 1 : 0);
            int const ay = (f.dy < 0 ? -f.dy : f.dy) + (f.ky ? 1 : 0);
            m += ax > ay ? ax : ay;
        }
        if (m > CANVAS2D_DIM_MAX) {
            return CANVAS2D_DIM_MAX;
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
static void apply_drop_shadow(struct canvas2d_context *__single cv, canvas2d_filter f, int w, int h) {
    int const npix = w * h;
    if (!ensure_blur_tmp(cv, 2 * npix)) {
        return;
    }
    // Premultiply the tint without clamp: on a linear canvas an HDR or
    // wide-gamut shadow colour (f.color is interned extended) carries through,
    // the composite's space-aware output clamp being the bound, as for shade8.
    // On an sRGB canvas f.color is already [0,1], so this stays byte-identical.
    float const ta = f.color[3];
    canvas2d_premul const tint = { .r = (_Float16)(f.color[0] * ta),
                               .g = (_Float16)(f.color[1] * ta),
                               .b = (_Float16)(f.color[2] * ta),
                               .a = (_Float16)ta };
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
                cv->blur_tmp[y * w + x] = (canvas2d_premul){
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
                cv->blur_tmp[y * w + x] = (canvas2d_premul){
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
            canvas2d_blur_box_h_f16(cv->blur_tmp + npix, cv->blur_tmp, w, h, f.blur);
            canvas2d_blur_box_v_f16(cv->blur_tmp, cv->blur_tmp + npix, w, h, f.blur);
        }
    }
    for (int i = 0; i < npix; i++) {  // premultiplied source-over: tile OVER shadow
        canvas2d_premul t = cv->tile[i], s = cv->blur_tmp[i];
        float const k = 1.0f - (float)t.a;
        cv->tile[i] = (canvas2d_premul){
            .r = (_Float16)((float)t.r + (float)s.r * k),
            .g = (_Float16)((float)t.g + (float)s.g * k),
            .b = (_Float16)((float)t.b + (float)s.b * k),
            .a = (_Float16)((float)t.a + (float)s.a * k),
        };
    }
}

// Run the state's filter list (if any) over the freshly painted w*h tile in
// place, just before it composites.  Colour entries are 1:1 per pixel (maximal
// runs of them go through canvas2d_filter_apply together); a blur() entry runs the
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
static void apply_filters(struct canvas2d_context *__single cv, int w, int h) {
    int const count = cv->cur.filter_count;
    int const npix = w * h;
    int i = 0;
    while (i < count) {
        canvas2d_filter const f = cv->cur.filters[i];
        if (f.shadow) {
            apply_drop_shadow(cv, f, w, h);
            i += 1;
        } else if (f.blur > 0) {
            if (ensure_blur_tmp(cv, npix)) {
                for (int pass = 0; pass < 3; pass++) {
                    canvas2d_blur_box_h_f16(cv->blur_tmp, cv->tile, w, h, f.blur);
                    canvas2d_blur_box_v_f16(cv->tile, cv->blur_tmp, w, h, f.blur);
                }
            }
            i += 1;
        } else {
            int j = i + 1;
            while (j < count && cv->cur.filters[j].blur == 0 &&
                   !cv->cur.filters[j].shadow) {
                j += 1;
            }
            canvas2d_filter_apply(cv->cur.filters + i, j - i, cv->tile, npix);
            i = j;
        }
    }
}

// Add a path edge to the coverage rasterizer, translated into the tile's frame.
static void cover_edge(struct canvas2d_context *__single cv, cbbox b, canvas2d_vec2 p0, canvas2d_vec2 p1) {
    canvas2d_cover_add_edge(&cv->cover, b.w, b.h, p0.x - (float)b.x, p0.y - (float)b.y,
                        p1.x - (float)b.x, p1.y - (float)b.y);
}

static void cover_path_edges(struct canvas2d_context *__single cv, cbbox b, struct canvas2d_path const *p) {
    for (int s = 0; s < p->nsubs; s++) {
        canvas2d_subpath const sp = p->subs[s];
        if (sp.count < 2) {
            continue;
        }
        for (int k = 0; k < sp.count; k++) {
            canvas2d_vec2 const a = p->pts[sp.start + k];
            canvas2d_vec2 const c = p->pts[sp.start + (k + 1) % sp.count];
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
// (canvas2d_planar.h); per-pixel branches are bitwise lane selects (f16x8_if_then_else)
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

static_assert(CANVAS2D_OP_COPY == 10 && CANVAS2D_OP_MULTIPLY == 11 &&
              CANVAS2D_OP_EXCLUSION == 21 && CANVAS2D_OP_HUE == 22 &&
              CANVAS2D_BLEND_MODE_COUNT == 26,
              "blend8 dispatches on these mode bands");

// Coverage semantics (docs/rasterization.md): partial
// coverage applies in principle as a lerp between the destination and the
// full-strength blend, out = lerp(dst, blend(src, dst), cov) -- a pixel the
// shape doesn't cover keeps its destination.  Folding coverage into source
// alpha instead (src *= cov, premultiplied) is identical math only where the
// Porter-Duff form co = Fa*s + Fb*d has Fa free of sa and Fb affine in sa
// with Fb(0) = 1, AND the result never trips the output clamp: the modes
// below.  Those fold; every other mode takes the lerp in canvas2d_blend.
// 'lighter' passes the Fa/Fb criterion (Fa = Fb = 1) but its co = s + d
// exceeds 1 exactly where it saturates, and clamp(c*s + d) != lerp(d,
// clamp(s + d), c) there -- the supersampled truth clamps per subsample, so
// lighter lerps (test_coverage_lerp measures the difference).
static bool coverage_folds(enum canvas2d_composite_op m) {
    switch ((int)m) {
        case CANVAS2D_OP_SOURCE_OVER:      // Fa = 1,      Fb = 1 - sa
        case CANVAS2D_OP_SOURCE_ATOP:      // Fa = da,     Fb = 1 - sa
        case CANVAS2D_OP_DESTINATION_OVER: // Fa = 1 - da, Fb = 1
        case CANVAS2D_OP_DESTINATION_OUT:  // Fa = 0,      Fb = 1 - sa
        case CANVAS2D_OP_XOR:              // Fa = 1 - da, Fb = 1 - sa
            return true;
        default:  // copy, the in/out family, dst-atop, lighter, blends
            return false;
    }
}

// Separable blend B(cb, cs), unpremultiplied; only the non-linear modes need it.
static f16x8 blend_sep8(enum canvas2d_composite_op mode, f16x8 cb, f16x8 cs) {
    f16x8 const zero = (f16x8)(_Float16)0.0f, one = (f16x8)(_Float16)1.0f;
    switch ((int)mode) {
        case CANVAS2D_OP_COLOR_DODGE:
            return f16x8_if_then_else(cb <= zero, zero,
                   f16x8_if_then_else(cs >= one,  one,
                       __builtin_elementwise_min(one, cb / (one - cs))));
        case CANVAS2D_OP_COLOR_BURN:
            return f16x8_if_then_else(cb >= one,  one,
                   f16x8_if_then_else(cs <= zero, zero,
                       one - __builtin_elementwise_min(one, (one - cb) / cs)));
        case CANVAS2D_OP_SOFT_LIGHT: {
            f16x8 dd = f16x8_if_then_else(cb <= (f16x8)(_Float16)0.25f,
                (((_Float16)16.0f * cb - (_Float16)12.0f) * cb + (_Float16)4.0f) * cb,
                __builtin_elementwise_sqrt(cb));
            return f16x8_if_then_else(cs <= (f16x8)(_Float16)0.5f,
                cb - (one - (_Float16)2.0f * cs) * cb * (one - cb),
                cb + ((_Float16)2.0f * cs - one) * (dd - cb));
        }
        default:
            return cs;
    }
}

// Premultiplied separable term T = sa*da*B per channel plane (s, d premultiplied).
static f16x8 blend_term8(enum canvas2d_composite_op mode,
                           f16x8 s, f16x8 d, f16x8 sa, f16x8 da) {
    switch ((int)mode) {
        case CANVAS2D_OP_MULTIPLY:    return s * d;
        case CANVAS2D_OP_SCREEN:      return sa * d + da * s - s * d;
        case CANVAS2D_OP_OVERLAY:
            return f16x8_if_then_else((_Float16)2.0f * d <= da,
                                      (_Float16)2.0f * s * d,
                                      sa * da - (_Float16)2.0f * (da - d) * (sa - s));
        case CANVAS2D_OP_DARKEN:      return __builtin_elementwise_min(s * da, d * sa);
        case CANVAS2D_OP_LIGHTEN:     return __builtin_elementwise_max(s * da, d * sa);
        case CANVAS2D_OP_HARD_LIGHT:
            return f16x8_if_then_else((_Float16)2.0f * s <= sa,
                                      (_Float16)2.0f * s * d,
                                      sa * da - (_Float16)2.0f * (da - d) * (sa - s));
        case CANVAS2D_OP_DIFFERENCE:
            return __builtin_elementwise_abs(s * da - d * sa);
        case CANVAS2D_OP_EXCLUSION:   return sa * d + da * s - (_Float16)2.0f * s * d;
        default: {  // color-dodge / color-burn / soft-light
            f16x8 const zero = (f16x8)(_Float16)0.0f;
            f16x8 const cs = f16x8_if_then_else(sa > zero, s / sa, zero);
            f16x8 const cb = f16x8_if_then_else(da > zero, d / da, zero);
            return sa * da * blend_sep8(mode, cb, cs);
        }
    }
}

// --- the linear working-space seam -------------------------------------------
//
// On a LINEAR canvas (cv->space == CANVAS2D_CS_LINEAR_SRGB) the target holds extended
// linear sRGB.  Two families of blend mode:
//
//   * range-preserving (the over/Porter-Duff family, multiply, screen,
//     difference, exclusion) -- the blend expression is a valid operation in
//     any space, so it runs DIRECTLY on the linear premul slabs, finished with
//     the no-upper-clamp variant (canvas2d_px8_clamp_premul_lin) so extended
//     colours survive to the output boundary;
//   * spec-defined-in-encoded-[0,1] (lighter, overlay, darken, lighten,
//     color-dodge, color-burn, hard-light, soft-light, and the non-separable
//     hue/saturation/color/luminosity) -- clip_color / the HSL math / the
//     dodge-burn-soft-light divides all need an encoded [0,1] precondition, so
//     these run through an encode->sRGB / blend / decode->linear wrapper.
//
// linear_direct() splits the two.  An sRGB canvas never reaches any of this --
// every linear-only branch is gated on cv->space upstream.
static bool linear_direct(enum canvas2d_composite_op mode) {
    switch ((int)mode) {
        case CANVAS2D_OP_SOURCE_OVER:      case CANVAS2D_OP_SOURCE_IN:
        case CANVAS2D_OP_SOURCE_OUT:       case CANVAS2D_OP_SOURCE_ATOP:
        case CANVAS2D_OP_DESTINATION_OVER: case CANVAS2D_OP_DESTINATION_IN:
        case CANVAS2D_OP_DESTINATION_OUT:  case CANVAS2D_OP_DESTINATION_ATOP:
        case CANVAS2D_OP_XOR:              case CANVAS2D_OP_COPY:
        case CANVAS2D_OP_MULTIPLY:         case CANVAS2D_OP_SCREEN:
        case CANVAS2D_OP_DIFFERENCE:       case CANVAS2D_OP_EXCLUSION:
            return true;
        default:  // lighter, overlay, darken, lighten, dodge/burn, hard/soft
            return false;  // light, and the non-separable HSL quartet
    }
}

// Per-lane sRGB transfer over a premultiplied slab.  The transfer is nonlinear,
// so it must act on STRAIGHT (unpremultiplied) colour: unpremultiply (guarded;
// a == 0 stays all-zero), transfer each RGB lane through the f32 scalar kernel
// (canvas2d_color.c -- precision-sensitive pow runs in f32, narrowing once), then
// repremultiply.  Alpha rides through untouched -- it is never a colour.  Scalar
// per lane (libm has no f16x8 pow; the deferral note in canvas2d_color.c applies):
// these wrap only the spec-in-sRGB blend modes, which are not the profiled hot
// path -- the over-family fast path never enters here.
typedef float (*chan_xfer)(float);
static canvas2d_px8 px8_transfer(canvas2d_px8 p, chan_xfer xf_chan) {
    canvas2d_px8 o = p;
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
// linear no-upper-clamp finish; an sRGB canvas clamps each channel to [0,ao]
// (its encoded values live in [0,1]).  Source-over is range-preserving, so on a
// linear canvas the fold runs directly on the linear slabs -- no encode wrapper.
static canvas2d_px8 src_over8(canvas2d_px8 s, canvas2d_px8 d, bool lin) {
    f16x8 const fb = (f16x8)(_Float16)1.0f - s.a;
    canvas2d_px8 co = { s.r + fb * d.r, s.g + fb * d.g,
                    s.b + fb * d.b, s.a + fb * d.a };
    return lin ? canvas2d_px8_clamp_premul_lin(co) : canvas2d_px8_clamp_premul(co);
}

// Eight pixels' unpremultiplied colour as three channel planes.
typedef struct {
    f16x8 r, g, b;
} rgb8;

static f16x8 lum8(rgb8 c) {
    return (_Float16)0.3f * c.r + (_Float16)0.59f * c.g + (_Float16)0.11f * c.b;
}

static rgb8 clip_color8(rgb8 c) {
    f16x8 const zero = (f16x8)(_Float16)0.0f, one = (f16x8)(_Float16)1.0f;
    f16x8 const l = lum8(c);
    f16x8 const n = __builtin_elementwise_min(c.r, __builtin_elementwise_min(c.g, c.b));
    f16x8 const x = __builtin_elementwise_max(c.r, __builtin_elementwise_max(c.g, c.b));
    i16x8 const lo = n < zero;  // lanes with a channel below 0: scale about l
    f16x8 const kn = l / (l - n);
    c.r = f16x8_if_then_else(lo, l + (c.r - l) * kn, c.r);
    c.g = f16x8_if_then_else(lo, l + (c.g - l) * kn, c.g);
    c.b = f16x8_if_then_else(lo, l + (c.b - l) * kn, c.b);
    // The W3C ClipColor computes n and x ONCE, before either fix: the x > 1
    // test and the kx denominator both read the pre-fix maximum even though
    // the channels they rescale may have just been pulled toward l.
    i16x8 const hi = x > one;   // lanes with a channel above 1
    f16x8 const kx = (one - l) / (x - l);
    c.r = f16x8_if_then_else(hi, l + (c.r - l) * kx, c.r);
    c.g = f16x8_if_then_else(hi, l + (c.g - l) * kx, c.g);
    c.b = f16x8_if_then_else(hi, l + (c.b - l) * kx, c.b);
    return c;
}

static rgb8 set_lum8(rgb8 c, f16x8 l) {
    f16x8 const dl = l - lum8(c);
    c.r += dl;
    c.g += dl;
    c.b += dl;
    return clip_color8(c);
}

static f16x8 saturation8(rgb8 c) {
    return __builtin_elementwise_max(c.r, __builtin_elementwise_max(c.g, c.b))
         - __builtin_elementwise_min(c.r, __builtin_elementwise_min(c.g, c.b));
}

// Set saturation: max channel -> s, min -> 0, mid proportional; an all-equal
// lane (mx <= mn) has no saturation axis and goes to black.
static rgb8 set_saturation8(rgb8 c, f16x8 s) {
    f16x8 const zero = (f16x8)(_Float16)0.0f;
    f16x8 const mn = __builtin_elementwise_min(c.r, __builtin_elementwise_min(c.g, c.b));
    f16x8 const mx = __builtin_elementwise_max(c.r, __builtin_elementwise_max(c.g, c.b));
    i16x8 const flat = mx <= mn;
    f16x8 const k = s / (mx - mn);
    return (rgb8){ .r = f16x8_if_then_else(flat, zero, (c.r - mn) * k),
                   .g = f16x8_if_then_else(flat, zero, (c.g - mn) * k),
                   .b = f16x8_if_then_else(flat, zero, (c.b - mn) * k) };
}

static rgb8 blend_nonsep8(enum canvas2d_composite_op mode, rgb8 cb, rgb8 cs) {
    switch ((int)mode) {
        case CANVAS2D_OP_HUE:        return set_lum8(set_saturation8(cs, saturation8(cb)), lum8(cb));
        case CANVAS2D_OP_SATURATION: return set_lum8(set_saturation8(cb, saturation8(cs)), lum8(cb));
        case CANVAS2D_OP_COLOR:      return set_lum8(cs, lum8(cb));
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
static canvas2d_px8 blend8(canvas2d_px8 s, canvas2d_px8 d, enum canvas2d_composite_op mode,
                       bool lin) {
    if (lin && !linear_direct(mode)) {
        canvas2d_px8 const se = px8_transfer(s, canvas2d_linear_to_srgb);
        canvas2d_px8 const de = px8_transfer(d, canvas2d_linear_to_srgb);
        return px8_transfer(blend8(se, de, mode, false), canvas2d_srgb_to_linear);
    }
    if (mode == CANVAS2D_OP_SOURCE_OVER) {
        // Delegate to the fast path's kernel: the Porter-Duff arm below would
        // spell source-over as fa*s + fb*d with fa = 1, and the contraction
        // shape differs -- fa*s rounds fb*d's product separately where
        // src_over8's s + fb*d fuses it -- so the explicit kernel keeps every
        // source-over bit-identical no matter which loop reached it.
        return src_over8(s, d, lin);
    }
    f16x8 const zero = (f16x8)(_Float16)0.0f, one = (f16x8)(_Float16)1.0f;
    f16x8 sa = s.a, da = d.a;
    canvas2d_px8 co;

    if ((int)mode <= CANVAS2D_OP_COPY) {
        // Porter-Duff: co = Fa*s + Fb*d, ao = Fa*sa + Fb*da.
        f16x8 fa, fb;
        switch ((int)mode) {
            case CANVAS2D_OP_SOURCE_IN:        fa = da;       fb = zero;      break;
            case CANVAS2D_OP_SOURCE_OUT:       fa = one - da; fb = zero;      break;
            case CANVAS2D_OP_SOURCE_ATOP:      fa = da;       fb = one - sa;  break;
            case CANVAS2D_OP_DESTINATION_OVER: fa = one - da; fb = one;       break;
            case CANVAS2D_OP_DESTINATION_IN:   fa = zero;     fb = sa;        break;
            case CANVAS2D_OP_DESTINATION_OUT:  fa = zero;     fb = one - sa;  break;
            case CANVAS2D_OP_DESTINATION_ATOP: fa = one - da; fb = sa;        break;
            case CANVAS2D_OP_XOR:              fa = one - da; fb = one - sa;  break;
            case CANVAS2D_OP_LIGHTER:          fa = one;      fb = one;       break;
            case CANVAS2D_OP_COPY:             fa = one;      fb = zero;      break;
            default:                         fa = one;      fb = one - sa;  break;  // source-over
        }
        co.r = fa * s.r + fb * d.r;
        co.g = fa * s.g + fb * d.g;
        co.b = fa * s.b + fb * d.b;
        co.a = fa * sa  + fb * da ;
    } else {
        canvas2d_px8 t;
        if ((int)mode >= CANVAS2D_OP_HUE) {
            i16x8 sm = sa > zero, dm = da > zero;  // a == 0 un-premultiplies to 0
            rgb8 cs = { f16x8_if_then_else(sm, s.r / sa, zero),
                        f16x8_if_then_else(sm, s.g / sa, zero),
                        f16x8_if_then_else(sm, s.b / sa, zero) };
            rgb8 cb = { f16x8_if_then_else(dm, d.r / da, zero),
                        f16x8_if_then_else(dm, d.g / da, zero),
                        f16x8_if_then_else(dm, d.b / da, zero) };
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
    return lin ? canvas2d_px8_clamp_premul_lin(co)
               : canvas2d_px8_clamp_premul(co);  // additive 'lighter' can exceed 1
}

// The coverage lerp: out = blend*cov + dst*(1-cov) per plane.  Two products,
// not dst + (blend-dst)*cov, so cov == 1 returns the blend bit-exactly and
// cov == 0 returns dst bit-exactly (full coverage must not perturb the blend;
// zero coverage must not perturb the destination).  The clamp restores the
// premultiplied invariant against the one-ULP drift of cov + (1-cov) in f16.
static canvas2d_px8 cov_lerp8(canvas2d_px8 d, canvas2d_px8 co, f16x8 cov, bool lin) {
    f16x8 const icov = (f16x8)(_Float16)1.0f - cov;
    canvas2d_px8 o = { co.r * cov + d.r * icov, co.g * cov + d.g * icov,
                   co.b * cov + d.b * icov, co.a * cov + d.a * icov };
    // Coverage is a geometric fraction (antialiasing), so the lerp between two
    // working-space premul colours is range-preserving: the linear no-upper
    // clamp on a linear canvas, the [0,ao] clamp on an sRGB one.
    return lin ? canvas2d_px8_clamp_premul_lin(o) : canvas2d_px8_clamp_premul(o);
}

// The generic modes, shared by the tile and solid-colour entry points: the
// same planar slab walk as the source-over fast path, with the 26-mode
// kernel in place of the source-over fold.  `tile` may be NULL, in which case
// every slab's source is `splat` -- one solid colour broadcast across the
// lanes.  Effective coverage is the op plane x the clip mask (each absent
// factor is 1); the over-family folds it into the source exactly as the fast
// path does, every other mode blends at full strength and lerps toward the
// destination (cov_lerp8).
static void blend_region(struct canvas2d_context *__single cv, int x, int y, int w, int h,
                         canvas2d_premul const *__counted_by_or_null(w * h) tile,
                         canvas2d_px8 splat,
                         uint8_t const *__counted_by_or_null(w * h) cov,
                         uint8_t const *__counted_by_or_null(clip_len) clip,
                         int clip_len, enum canvas2d_composite_op mode) {
    (void)clip_len;
    bool const folds = coverage_folds(mode);
    bool const atten = cov || clip;  // any coverage to apply?
    bool const lin = cv->space == CANVAS2D_CS_LINEAR_SRGB;  // false -> sRGB bypass
    _Float16 const inv255 = (_Float16)(1.0f / 255.0f);
    f16x8 const one = (f16x8)(_Float16)1.0f;
    for (int row = 0; row < h; row++) {
        int col = 0;
        for (; col + 8 <= w; col += 8) {
            int ti = row * w + col;
            int di = (y + row) * cv->width + (x + col);
            canvas2d_px8 s = tile ? canvas2d_px8_load(tile + ti) : splat;
            canvas2d_px8 d = canvas2d_px8_load(cv->target + di);
            canvas2d_px8 o;
            if (!atten) {
                o = blend8(s, d, mode, lin);
            } else if (folds) {
                // Attenuate the source by each factor in turn, exactly as the
                // fast path does (combining the factors first re-rounds).
                if (cov) {
                    s = canvas2d_px8_scale(s, f16x8_from_u8(cov + ti) * inv255);
                }
                if (clip) {
                    s = canvas2d_px8_scale(s, f16x8_from_u8(clip + di) * inv255);
                }
                o = blend8(s, d, mode, lin);
            } else {
                f16x8 cv8 = one;  // 1*x is exact: a lone factor passes through
                if (cov) {
                    cv8 = cv8 * (f16x8_from_u8(cov + ti) * inv255);
                }
                if (clip) {
                    cv8 = cv8 * (f16x8_from_u8(clip + di) * inv255);
                }
                o = cov_lerp8(d, blend8(s, d, mode, lin), cv8, lin);
            }
            canvas2d_px8_store(cv->target + di, o);
        }
        if (col < w) {
            int const n = w - col;
            int const ti = row * w + col;
            int const di = (y + row) * cv->width + (x + col);
            canvas2d_px8 s = tile ? canvas2d_px8_load_k(tile + ti, n) : splat;
            canvas2d_px8 const d = canvas2d_px8_load_k(cv->target + di, n);
            canvas2d_px8 o;
            if (!atten) {
                o = blend8(s, d, mode, lin);
            } else if (folds) {
                if (cov) {
                    s = canvas2d_px8_scale(s, f16x8_from_u8_k(cov + ti, n) * inv255);
                }
                if (clip) {
                    s = canvas2d_px8_scale(s, f16x8_from_u8_k(clip + di, n) * inv255);
                }
                o = blend8(s, d, mode, lin);
            } else {
                f16x8 cv8 = one;
                if (cov) {
                    cv8 = cv8 * (f16x8_from_u8_k(cov + ti, n) * inv255);
                }
                if (clip) {
                    cv8 = cv8 * (f16x8_from_u8_k(clip + di, n) * inv255);
                }
                o = cov_lerp8(d, blend8(s, d, mode, lin), cv8, lin);
            }
            canvas2d_px8_store_k(cv->target + di, n, o);
        }
    }
}

void canvas2d_blend(struct canvas2d_context *__single cv, int x, int y, int w, int h,
                canvas2d_premul const *__counted_by(w * h) tile,
                uint8_t const *__counted_by_or_null(w * h) cov,
                uint8_t const *__counted_by_or_null(clip_len) clip, int clip_len,
                enum canvas2d_composite_op mode) {
    if (!tile || w <= 0 || h <= 0) {
        return;
    }
    if (x < 0 || y < 0 || x + w > cv->width || y + h > cv->height) {
        return;
    }
    if (clip && clip_len < cv->width * cv->height) {
        return;
    }
    if (mode == CANVAS2D_OP_SOURCE_OVER) {
        // The fast path.  Source-over folds: op coverage (normally already
        // folded by the shade stage, so cov is NULL here) and clip
        // attenuation both scale the premultiplied source, in f16.  Source-over
        // is range-preserving, so `lin` only swaps src_over8's final clamp; the
        // sRGB and linear paths share the identical fold (lin == false on sRGB).
        bool const lin = cv->space == CANVAS2D_CS_LINEAR_SRGB;
        _Float16 const inv255 = (_Float16)(1.0f / 255.0f);
        for (int row = 0; row < h; row++) {
            int col = 0;
            for (; col + 8 <= w; col += 8) {
                int ti = row * w + col;
                int di = (y + row) * cv->width + (x + col);
                canvas2d_px8 s = canvas2d_px8_load(tile + ti);
                if (cov) {
                    s = canvas2d_px8_scale(s, f16x8_from_u8(cov + ti) * inv255);
                }
                if (clip) {
                    s = canvas2d_px8_scale(s, f16x8_from_u8(clip + di) * inv255);
                }
                canvas2d_px8 d = canvas2d_px8_load(cv->target + di);
                canvas2d_px8_store(cv->target + di, src_over8(s, d, lin));
            }
            if (col < w) {
                int const k = w - col;
                int const ti = row * w + col;
                int const di = (y + row) * cv->width + (x + col);
                canvas2d_px8 s = canvas2d_px8_load_k(tile + ti, k);
                if (cov) {
                    s = canvas2d_px8_scale(s, f16x8_from_u8_k(cov + ti, k) * inv255);
                }
                if (clip) {
                    s = canvas2d_px8_scale(s, f16x8_from_u8_k(clip + di, k) * inv255);
                }
                canvas2d_px8 const d = canvas2d_px8_load_k(cv->target + di, k);
                canvas2d_px8_store_k(cv->target + di, k, src_over8(s, d, lin));
            }
        }
        return;
    }
    // The generic modes: the shared region walk (splat unused, tile present).
    canvas2d_px8 const zero = { (f16x8)(_Float16)0.0f, (f16x8)(_Float16)0.0f,
                            (f16x8)(_Float16)0.0f, (f16x8)(_Float16)0.0f };
    blend_region(cv, x, y, w, h, tile, zero, cov, clip, clip_len, mode);
}

void canvas2d_blend_solid(struct canvas2d_context *__single cv, int x, int y, int w, int h,
                      canvas2d_premul color,
                      uint8_t const *__counted_by_or_null(w * h) cov,
                      uint8_t const *__counted_by_or_null(clip_len) clip, int clip_len,
                      enum canvas2d_composite_op mode) {
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
    canvas2d_px8 const splat = { (f16x8)color.r, (f16x8)color.g,
                             (f16x8)color.b, (f16x8)color.a };
    blend_region(cv, x, y, w, h, NULL, splat, cov, clip, clip_len, mode);
}

void canvas2d_blend_read(struct canvas2d_context *__single cv,
                     canvas2d_premul *__counted_by(pixels) out, int pixels) {
    if (!out || pixels < cv->target_len) {
        return;
    }
    memcpy(out, cv->target, (size_t)cv->target_len * sizeof *out);
}

// --- the planar shade stage --------------------------------------------------
//
// The coverage -> premultiplied-tile fold, eight pixels per step over channel
// planes (canvas2d_planar.h).  The fold runs in _Float16, the pipeline's compute
// type (docs/decisions/color-axis.md): coverage normalizes as one f16
// multiply by RN16(1/255) -- the blend stage's exact idiom, and 255 * that
// rounds back to exactly 1.0, so full coverage passes the paint's alpha
// through untouched -- and each paint's alpha factors fold in the type the
// colour data is born in (f16 for solid and gradient paint, f32 for image
// and pattern samples), with one narrowing convert.

// Eight coverage bytes as an f16 plane in [0, 1]: exact widen (every u8 value
// is exact in _Float16), one multiply by RN16(1/255).
static f16x8 cover8(uint8_t const *__counted_by(8) cov) {
    return f16x8_from_u8(cov) * (_Float16)(1.0f / 255.0f);
}

static f16x8 cover8_k(uint8_t const *__counted_by(k) cov, int k) {
    return f16x8_from_u8_k(cov, k) * (_Float16)(1.0f / 255.0f);
}

// Premultiply the colour planes under the folded alpha plane.  No clamp: alpha
// is already in [0,1] (coverage x global x sample), and the colour planes' bound
// belongs to the blend's space-aware output clamp -- the sRGB path re-clamps to
// [0,ao] (so this stays byte-identical there), the linear path keeps extended
// (HDR above 1, wide-gamut below 0) colour.
static canvas2d_px8 shade8(f16x8 r, f16x8 g, f16x8 b, f16x8 alpha) {
    return (canvas2d_px8){ r * alpha, g * alpha, b * alpha, alpha };
}

// mat_apply8 / mat_apply8_persp (8-wide canvas2d_mat_apply over a row) and their
// foldv8 result live in canvas2d_geom.h: the gradient and pattern paint loops below
// and the image sampler both apply a matrix to eight pixel centres at once.

// Does the shade stage fold the op's coverage into the tile's alpha?  The
// over-family folds exactly (coverage_folds); for every other
// mode the tile carries the source at full strength and the
// coverage plane rides to canvas2d_blend separately, which lerps.  Filters
// force the fold regardless: blur()/drop-shadow() consume the op's silhouette
// from the tile's alpha, so coverage must be materialized before they run --
// after a filter the coverage genuinely is source alpha.
static bool shade_folds_coverage(struct canvas2d_context const *__single cv) {
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
static void blend_tile(struct canvas2d_context *__single cv, cbbox b, bool fold) {
    apply_filters(cv, b.w, b.h);
    emit_shadow(cv, b, true, !fold, (_Float16)1.0f);
    canvas2d_blend(cv, b.x, b.y, b.w, b.h, cv->tile, fold ? NULL : cv->cov,
               cv->cur.clip_mask, cv->cur.clip_len, cv->cur.composite);
}

// Paint the resolved coverage in cv->cov with a solid colour.  Each pixel's
// alpha is paint_alpha * global_alpha * coverage when the composite mode folds
// coverage (shade_folds_coverage); otherwise paint_alpha * global_alpha, with
// cv->cov handed to the blend's lerp.
static void paint_tile_solid(struct canvas2d_context *__single cv, cbbox b, canvas2d_unpremul solid) {
    float const ga = cv->cur.global_alpha;
    bool const fold = shade_folds_coverage(cv);
    // The colour planes are splats and every alpha factor but coverage is one
    // constant, so the loop is a coverage widen, two multiplies, and the
    // premultiply -> st4.  Coverage and tile are both dense over the bbox, so
    // one flat loop covers all rows.
    f16x8 const base = (f16x8)(_Float16)((float)solid.a * ga);
    f16x8 const cr = (f16x8)solid.r, cg = (f16x8)solid.g,
                cb = (f16x8)solid.b;
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
        canvas2d_premul px;
        canvas2d_px8_store_k(&px, 1, shade8(cr, cg, cb, base));
        canvas2d_blend_solid(cv, b.x, b.y, b.w, b.h, px, cv->cov,
                         cv->cur.clip_mask, cv->cur.clip_len,
                         cv->cur.composite);
        return;
    }
    int i = 0;
    for (; i + 8 <= npix; i += 8) {
        canvas2d_px8_store(cv->tile + i,
                       shade8(cr, cg, cb, base * cover8(cv->cov + i)));
    }
    if (i < npix) {
        int const k = npix - i;
        canvas2d_px8_store_k(cv->tile + i, k,
                         shade8(cr, cg, cb,
                                base * cover8_k(cv->cov + i, k)));
    }
    blend_tile(cv, b, fold);
}

// Paint the resolved coverage with a gradient; the same alpha fold as
// paint_tile_solid, with the colour solved per pixel instead of splatted.
// Fill t_out with the gradient parameter for a row of pixel centres under a
// PERSPECTIVE CTM: map each device centre back to user space (perspective-
// correct, the 8-wide u/w v/w divide), then solve the parameter in user space.
// The affine row solver (canvas2d_gradient_param_row) is divide-free and linear in
// x; this is its projective twin -- same -1 "outside" sentinel for the radial
// miss, consumed by the unchanged canvas2d_gradient_color_row.
static void grad_param_row_persp(struct canvas2d_gradient const *gr, int x0, float y,
                                 int n, float *__counted_by(n) t_out) {
    f32x8 const lane = { 0, 1, 2, 3, 4, 5, 6, 7 };
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        f32x8 const xs = (float)x0 + ((float)i + lane) + 0.5f;
        foldv8 const uv = mat_apply8_persp(gr->to_user, xs, y);
        for (int l = 0; l < 8; l++) {
            float t;
            t_out[i + l] = canvas2d_gradient_param_user(
                               gr, (canvas2d_vec2){ uv.x[l], uv.y[l] }, &t)
                               ? t : -1.0f;
        }
    }
    for (; i < n; i++) {
        canvas2d_vec2 const p = canvas2d_mat_apply(gr->to_user,
            (canvas2d_vec2){ .x = (float)x0 + (float)i + 0.5f, .y = y });
        float t;
        t_out[i] = canvas2d_gradient_param_user(gr, p, &t) ? t : -1.0f;
    }
}

// Paint the resolved coverage with a gradient; the same alpha fold as
// paint_tile_solid, with the colour solved per pixel instead of splatted.
static void paint_tile_gradient(struct canvas2d_context *__single cv, cbbox b,
                                struct canvas2d_gradient const *gr) {
    float const ga = cv->cur.global_alpha;
    bool const fold = shade_folds_coverage(cv);
    if (gr->persp && ensure_grad_rows(cv, b.w)) {
        // Perspective CTM: solve the parameter in user space (the row filler
        // does the per-pixel device->user divide), then reuse the unchanged
        // colour + fold stages.  Affine gradients never reach here -- persp is
        // false -- so the divide-free row solver below stays byte-identical.
        f16x8 const gah = (f16x8)(_Float16)ga;
        for (int py = 0; py < b.h; py++) {
            grad_param_row_persp(gr, b.x, (float)b.y + (float)py + 0.5f, b.w,
                                 cv->trow);
            canvas2d_gradient_color_row(gr, cv->trow, b.w, cv->crow);
            int const row = py * b.w;
            int px = 0;
            for (; px + 8 <= b.w; px += 8) {
                canvas2d_px8 const col = canvas2d_px8_load_unpremul(cv->crow + px);
                f16x8 alpha = col.a * gah;
                if (fold) {
                    alpha = alpha * cover8(cv->cov + row + px);
                }
                canvas2d_px8_store(cv->tile + row + px,
                               shade8(col.r, col.g, col.b, alpha));
            }
            if (px < b.w) {
                int const k = b.w - px;
                canvas2d_px8 const col = canvas2d_px8_load_unpremul_k(cv->crow + px, k);
                f16x8 alpha = col.a * gah;
                if (fold) {
                    alpha = alpha * cover8_k(cv->cov + row + px, k);
                }
                canvas2d_px8_store_k(cv->tile + row + px, k,
                                 shade8(col.r, col.g, col.b, alpha));
            }
        }
        blend_tile(cv, b, fold);
        return;
    }
    if (ensure_grad_rows(cv, b.w)) {
        // Evaluate the gradient a row at a time, all three stages vectorized:
        // solve the parameters (canvas2d_gradient_param_row), lerp the stop
        // colours from them (canvas2d_gradient_color_row) -- the exact
        // piecewise-linear colour, no precomputed ramp
        // (docs/decisions/gradient-eval.md) -- then pick the colours back up
        // as planes (ld4 over the row buffer) for the fold.
        f16x8 const gah = (f16x8)(_Float16)ga;
        for (int py = 0; py < b.h; py++) {
            canvas2d_gradient_param_row(gr, b.x, (float)b.y + (float)py + 0.5f, b.w,
                                    cv->trow);
            canvas2d_gradient_color_row(gr, cv->trow, b.w, cv->crow);
            int const row = py * b.w;
            int px = 0;
            for (; px + 8 <= b.w; px += 8) {
                canvas2d_px8 const col = canvas2d_px8_load_unpremul(cv->crow + px);
                f16x8 alpha = col.a * gah;
                if (fold) {
                    alpha = alpha * cover8(cv->cov + row + px);
                }
                canvas2d_px8_store(cv->tile + row + px,
                               shade8(col.r, col.g, col.b, alpha));
            }
            if (px < b.w) {
                int const k = b.w - px;
                canvas2d_px8 const col = canvas2d_px8_load_unpremul_k(cv->crow + px, k);
                f16x8 alpha = col.a * gah;
                if (fold) {
                    alpha = alpha * cover8_k(cv->cov + row + px, k);
                }
                canvas2d_px8_store_k(cv->tile + row + px, k,
                                 shade8(col.r, col.g, col.b, alpha));
            }
        }
    } else {
        // OOM fallback: the row buffers couldn't grow, so run the scalar
        // per-pixel parameter solve + stop lerp.  Under perspective the device
        // centre maps back to user space (perspective-correct) and the
        // parameter solves there; affine solves directly in device space.
        for (int py = 0; py < b.h; py++) {
            for (int px = 0; px < b.w; px++) {
                int const i = py * b.w + px;
                float t;
                canvas2d_vec2 const dp = { .x = (float)b.x + (float)px + 0.5f,
                                       .y = (float)b.y + (float)py + 0.5f };
                bool got;
                if (gr->persp) {
                    canvas2d_vec2 const up = canvas2d_mat_apply(gr->to_user, dp);
                    got = canvas2d_gradient_param_user(gr, up, &t);
                } else {
                    got = canvas2d_gradient_param(gr, dp, &t);
                }
                canvas2d_unpremul col = got
                                        ? canvas2d_gradient_color_at(gr, t)
                                        : canvas2d_unpremul_of(0.0f, 0.0f, 0.0f, 0.0f);
                // Fold global alpha (and, for folding modes, coverage) into
                // the paint's alpha, then premultiply -- the row kernel's f16
                // fold, one pixel at a time.
                _Float16 alpha = col.a * (_Float16)ga;
                if (fold) {
                    alpha = alpha * ((_Float16)cv->cov[i] * (_Float16)(1.0f / 255.0f));
                }
                // Premultiply without clamp, matching the vectorized arm's
                // shade8: the colour bound is the blend's space-aware output
                // clamp, so an extended linear gradient stop survives here too.
                cv->tile[i] = (canvas2d_premul){ .r = (_Float16)(col.r * alpha),
                                            .g = (_Float16)(col.g * alpha),
                                            .b = (_Float16)(col.b * alpha),
                                            .a = alpha };
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
static void pattern_sample(struct canvas2d_pattern const *p, float u, float v, bool smooth,
                           float *__counted_by(4) out) {
    bool const rx = p->repeat == CANVAS2D_REPEAT || p->repeat == CANVAS2D_REPEAT_X;
    bool const ry = p->repeat == CANVAS2D_REPEAT || p->repeat == CANVAS2D_REPEAT_Y;
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
        int u0 = wrap_idx(canvas2d_f2i(fu), w, rx), u1 = wrap_idx(canvas2d_f2i(fu) + 1, w, rx);
        int v0 = wrap_idx(canvas2d_f2i(fv), h, ry), v1 = wrap_idx(canvas2d_f2i(fv) + 1, h, ry);
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
        int const iu = wrap_idx(canvas2d_f2i(floorf(u)), w, rx);
        int const iv = wrap_idx(canvas2d_f2i(floorf(v)), h, ry);
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
static void paint_tile_pattern(struct canvas2d_context *__single cv, cbbox b, struct canvas2d_pattern const *p) {
    float const ga = cv->cur.global_alpha;
    bool const fold = shade_folds_coverage(cv);
    bool const smooth = cv->cur.image_smoothing_enabled;
    // to_pattern is the device->pattern inverse of the CTM in force at creation;
    // it is non-affine exactly when that CTM was perspective, which is the per-
    // pixel-divide branch.  Affine patterns keep mat_apply8's linear DDA.
    bool const persp = !canvas2d_mat_is_affine(p->to_pattern);
    f32x8 const lane = { 0, 1, 2, 3, 4, 5, 6, 7 };
    for (int py = 0; py < b.h; py++) {
        float const dy = (float)b.y + (float)py + 0.5f;
        for (int px = 0; px < b.w; px += 8) {
            int const i = py * b.w + px;
            int const k = b.w - px < 8 ? b.w - px : 8;
            // Pixel-centre x per lane: integer-exact f32 sums, so the grouping
            // can't differ from the scalar (float)b.x + (float)(px+l) + 0.5f.
            f32x8 const xs = (float)b.x + ((float)px + lane) + 0.5f;
            foldv8 const uv = persp ? mat_apply8_persp(p->to_pattern, xs, dy)
                                    : mat_apply8(p->to_pattern, xs, dy);
            f32x8 sr = (f32x8)0.0f, sg = (f32x8)0.0f, sb = (f32x8)0.0f,
                   sa = (f32x8)0.0f;
            for (int l = 0; l < k; l++) {
                if (cv->cov[i + l] == 0) {  // a zero-coverage lane skips its taps
                    continue;
                }
                float s[4];
                pattern_sample(p, uv.x[l], uv.y[l], smooth, s);
                if (p->space != cv->space) {  // straight samples: convert in place
                    sample_to_working(cv, p->space, s);
                }
                sr[l] = s[0];
                sg[l] = s[1];
                sb[l] = s[2];
                sa[l] = s[3];
            }
            f16x8 alpha = __builtin_convertvector(sa * ga, f16x8);
            if (fold) {
                alpha = alpha * (k < 8 ? cover8_k(cv->cov + i, k)
                                       : cover8(cv->cov + i));
            }
            canvas2d_px8 const out = shade8(__builtin_convertvector(sr, f16x8),
                                        __builtin_convertvector(sg, f16x8),
                                        __builtin_convertvector(sb, f16x8),
                                        alpha);
            if (k < 8) {
                canvas2d_px8_store_k(cv->tile + i, k, out);
            } else {
                canvas2d_px8_store(cv->tile + i, out);
            }
        }
    }
    blend_tile(cv, b, fold);
}

// Grow the two shadow ping-pong masks to at least n bytes each.
static bool ensure_shadow(struct canvas2d_context *__single cv, int n) {
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
static bool shadow_active(struct canvas2d_context const *__single cv) {
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
// (canvas2d_add_filter_blur).
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
// lerp on (canvas2d_blur_shift_h/v); finer than 1/256 px is invisible.  Clamped to a
// sane range first (a larger offset just pushes the shadow off-canvas).  The
// fraction is always shifted rightward/downward of the floor, so a negative
// offset splits the same way: -2.5 is whole -3 plus 128/256.
static void shadow_offset_split(float v, int *__single whole, int *__single k256) {
    float const m = (float)(2 * CANVAS2D_DIM_MAX);
    v = v > m ? m : (v < -m ? -m : v);
    int const t = canvas2d_f2i(floorf(v * 256.0f + 0.5f));  // round to the 1/256 grid
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
static void emit_shadow(struct canvas2d_context *__single cv, cbbox b,
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
            canvas2d_blur_box_h(cv->shadow_dst, cv->shadow_src, sw, sh, radius);
            canvas2d_blur_box_v(cv->shadow_src, cv->shadow_dst, sw, sh, radius);
        }
    }
    if (kx || ky) {  // the offsets' subpixel fractions: one 2-tap lerp per
                     // axis (canvas2d_blur_shift_h/v), commuting with the box passes
                     // above, so the whole-pixel stamp plus these IS the
                     // fractional translate.  A zero fraction passes through
                     // as an exact copy, keeping the src/dst ping-pong even.
        canvas2d_blur_shift_h(cv->shadow_dst, cv->shadow_src, sw, sh, kx);
        canvas2d_blur_shift_v(cv->shadow_src, cv->shadow_dst, sw, sh, ky);
    }
    // Composite the shadow colour through the blurred mask: one splat, the
    // mask as the blend's coverage plane -- blend_region's fold arm scales
    // the splat by the mask for folding modes, its lerp arm bounds the blend
    // by it for the rest, so one call serves both strategies.  Global alpha
    // is NOT folded here: it already rides in the mask (the op alpha the
    // stamp loop quantized), per the spec's B = shadow(A) x globalAlpha.
    // The tile stays untouched -- it holds the op this shadow lands under.
    canvas2d_unpremul const sc = cv->cur.shadow_color;
    canvas2d_premul px;
    canvas2d_px8_store_k(&px, 1, shade8((f16x8)sc.r, (f16x8)sc.g, (f16x8)sc.b,
                                    (f16x8)sc.a));
    canvas2d_blend_solid(cv, sx0, sy0, sw, sh, px, cv->shadow_src,
                     cv->cur.clip_mask, cv->cur.clip_len,
                     cv->cur.composite);
}

// Paint the resolved coverage with the current fill / stroke paint, dispatching
// on its kind.  Each paint path casts the shadow itself, from the painted
// alpha, just before its blend -- so it lands under the shape.
static void paint_fill(struct canvas2d_context *__single cv, cbbox b) {
    if (cv->cur.fill_kind == CANVAS2D_PAINT_GRADIENT &&
        canvas2d_gradient_paints_nothing(&cv->cur.fill_grad)) {
        return;
    }
    switch (cv->cur.fill_kind) {
        case CANVAS2D_PAINT_SOLID:    paint_tile_solid(cv, b, cv->cur.fill);            break;
        case CANVAS2D_PAINT_GRADIENT: paint_tile_gradient(cv, b, &cv->cur.fill_grad);   break;
        case CANVAS2D_PAINT_PATTERN:  paint_tile_pattern(cv, b, &cv->cur.fill_pattern); break;
    }
}

static void paint_stroke(struct canvas2d_context *__single cv, cbbox b) {
    if (cv->cur.stroke_kind == CANVAS2D_PAINT_GRADIENT &&
        canvas2d_gradient_paints_nothing(&cv->cur.stroke_grad)) {
        return;
    }
    switch (cv->cur.stroke_kind) {
        case CANVAS2D_PAINT_SOLID:    paint_tile_solid(cv, b, cv->cur.stroke);            break;
        case CANVAS2D_PAINT_GRADIENT: paint_tile_gradient(cv, b, &cv->cur.stroke_grad);   break;
        case CANVAS2D_PAINT_PATTERN:  paint_tile_pattern(cv, b, &cv->cur.stroke_pattern); break;
    }
}

void canvas2d_clear_rect(struct canvas2d_context *__single cv, float x, float y, float w, float h) {
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "clear_rect", (float[]){ x, y, w, h }, 4); }
    canvas2d_vec2 q[4] = { xf(cv, x, y), xf(cv, x + w, y),
                       xf(cv, x + w, y + h), xf(cv, x, y + h) };
    cbbox b = points_bbox(cv, q, 4, 0);  // clear_rect bypasses filters: no margin
    if (b.w <= 0 || b.h <= 0) {
        return;
    }
    // Erase = destination-out of a unit-alpha solid: out = dst*(1 - alpha), and
    // the clip attenuates alpha to the coverage, so a clip leaves dst*(1 - clip).
    // The unit-alpha source is one splat colour -- no tile (and no allocation).
    canvas2d_blend_solid(cv, b.x, b.y, b.w, b.h,
                     (canvas2d_premul){ .r = 0, .g = 0, .b = 0,
                                    .a = (_Float16)1.0f },
                     NULL, cv->cur.clip_mask, cv->cur.clip_len,
                     CANVAS2D_OP_DESTINATION_OUT);
}

// A user-space point as a vec2 (no transform), the input to cv->upath.
static canvas2d_vec2 uv(float x, float y) {
    return (canvas2d_vec2){ .x = x, .y = y };
}

void canvas2d_fill_rect(struct canvas2d_context *__single cv, float x, float y, float w, float h) {
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "fill_rect", (float[]){ x, y, w, h }, 4); }
    if (!canvas2d_mat_is_affine(cv->cur.ctm)) {
        // Perspective: w-clip + project the user-space quad (a corner may sit at
        // or behind the projection plane), then fill the survivor.
        canvas2d_vec2 const uq[4] = { uv(x, y), uv(x + w, y), uv(x + w, y + h), uv(x, y + h) };
        canvas2d_path_reset(&cv->pclip);
        clip_project_contour(cv->cur.ctm, uq, 4, true, &cv->pclip);
        fill_device_path(cv, &cv->pclip, CANVAS2D_NONZERO);
        return;
    }
    canvas2d_vec2 q[4] = { xf(cv, x, y), xf(cv, x + w, y),
                       xf(cv, x + w, y + h), xf(cv, x, y + h) };
    cbbox const b = points_bbox(cv, q, 4, filter_margin(cv));
    if (b.w <= 0 || b.h <= 0 || !canvas2d_ensure_tile(cv, b.w * b.h) ||
        !canvas2d_cover_reset(&cv->cover, b.w, b.h)) {
        return;
    }
    cover_edge(cv, b, q[0], q[1]);
    cover_edge(cv, b, q[1], q[2]);
    cover_edge(cv, b, q[2], q[3]);
    cover_edge(cv, b, q[3], q[0]);
    canvas2d_cover_resolve(&cv->cover, b.w, b.h, CANVAS2D_NONZERO, cv->cov);
    paint_fill(cv, b);
}

void canvas2d_begin_path(struct canvas2d_context *__single cv) {
    if (cv->rec) { canvas2d_rec_op(cv->rec, "begin_path"); }
    canvas2d_path_reset(&cv->path);
    canvas2d_path_reset(&cv->upath);
}

void canvas2d_move_to(struct canvas2d_context *__single cv, float x, float y) {
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "move_to", (float[]){ x, y }, 2); }
    canvas2d_path_move_to(&cv->path, xf(cv, x, y));
    canvas2d_path_move_to(&cv->upath, uv(x, y));
    cv->cur_user = (canvas2d_vec2){ .x = x, .y = y };
}

void canvas2d_line_to(struct canvas2d_context *__single cv, float x, float y) {
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "line_to", (float[]){ x, y }, 2); }
    canvas2d_path_line_to(&cv->path, xf(cv, x, y));
    canvas2d_path_line_to(&cv->upath, uv(x, y));
    cv->cur_user = (canvas2d_vec2){ .x = x, .y = y };
}

void canvas2d_rect(struct canvas2d_context *__single cv, float x, float y, float w, float h) {
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "rect", (float[]){ x, y, w, h }, 4); }
    canvas2d_path_rect(&cv->path, xf(cv, x, y), xf(cv, x + w, y),
                   xf(cv, x + w, y + h), xf(cv, x, y + h));
    canvas2d_path_rect(&cv->upath, uv(x, y), uv(x + w, y), uv(x + w, y + h), uv(x, y + h));
    cv->cur_user = (canvas2d_vec2){ .x = x, .y = y };
}

void canvas2d_quadratic_curve_to(struct canvas2d_context *__single cv,
                               float cpx, float cpy, float x, float y) {
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "quadratic_curve_to", (float[]){ cpx, cpy, x, y }, 4); }
    canvas2d_path_quad_to(&cv->path, xf(cv, cpx, cpy), xf(cv, x, y),
                      CANVAS2D_FLATTEN_TOL);
    // Flatten in user space too (correct foreshortening once projected).
    canvas2d_path_quad_to(&cv->upath, uv(cpx, cpy), uv(x, y), CANVAS2D_FLATTEN_TOL);
    cv->cur_user = (canvas2d_vec2){ .x = x, .y = y };
}

void canvas2d_bezier_curve_to(struct canvas2d_context *__single cv, float c1x, float c1y,
                            float c2x, float c2y, float x, float y) {
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "bezier_curve_to", (float[]){ c1x, c1y, c2x, c2y, x, y }, 6); }
    canvas2d_path_cubic_to(&cv->path, xf(cv, c1x, c1y), xf(cv, c2x, c2y),
                       xf(cv, x, y), CANVAS2D_FLATTEN_TOL);
    canvas2d_path_cubic_to(&cv->upath, uv(c1x, c1y), uv(c2x, c2y), uv(x, y),
                       CANVAS2D_FLATTEN_TOL);
    cv->cur_user = (canvas2d_vec2){ .x = x, .y = y };
}

void canvas2d_ellipse(struct canvas2d_context *__single cv, float x, float y, float rx, float ry,
                    float rotation, float start_angle, float end_angle,
                    bool anticlockwise) {
    if (cv->rec) {
        canvas2d_rec_floats_bool(cv->rec, "ellipse",
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
    float const rr = rmax > CANVAS2D_FLATTEN_TOL ? rmax : CANVAS2D_FLATTEN_TOL;
    float dstep = 2.0f * acosf(fmaxf(-1.0f, 1.0f - CANVAS2D_FLATTEN_TOL / rr));
    if (!(dstep > 1e-4f)) {
        dstep = 1e-4f;  // guard against tiny/NaN step
    }
    float const fsegs = ceilf(fabsf(sweep) / dstep);
    int segs = canvas2d_f2i(fsegs);
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
        float const ux = x + ex * cosr - ey * sinr;
        float const uy = y + ex * sinr + ey * cosr;
        canvas2d_vec2 const p = xf(cv, ux, uy);
        if (i == 0 && !cv->path.has_cur) {
            canvas2d_path_move_to(&cv->path, p);
            canvas2d_path_move_to(&cv->upath, uv(ux, uy));
        } else {
            canvas2d_path_line_to(&cv->path, p);
            canvas2d_path_line_to(&cv->upath, uv(ux, uy));
        }
    }
    float const te = start_angle + sweep;
    cv->cur_user = (canvas2d_vec2){
        .x = x + rx * cosf(te) * cosr - ry * sinf(te) * sinr,
        .y = y + rx * cosf(te) * sinr + ry * sinf(te) * cosr,
    };
}

void canvas2d_arc(struct canvas2d_context *__single cv, float x, float y, float radius,
                float start_angle, float end_angle, bool anticlockwise) {
    // Record `arc` as itself, then swallow the canvas2d_ellipse it expands to.
    if (cv->rec) {
        canvas2d_rec_floats_bool(cv->rec, "arc",
                             (float[]){ x, y, radius, start_angle, end_angle },
                             5, anticlockwise);
        canvas2d_rec_enter(cv->rec);
    }
    canvas2d_ellipse(cv, x, y, radius, radius, 0.0f, start_angle, end_angle,
                   anticlockwise);
    canvas2d_rec_leave(cv->rec);
}

void canvas2d_round_rect(struct canvas2d_context *__single cv, float x, float y, float w, float h,
                       float radius) {
    // Record `round_rect` as itself, then swallow the move_to/arc/close_path it
    // expands to (no early returns between enter and leave).
    if (cv->rec) {
        canvas2d_rec_floats(cv->rec, "round_rect", (float[]){ x, y, w, h, radius }, 5);
        canvas2d_rec_enter(cv->rec);
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
    canvas2d_move_to(cv, x + r, y);
    canvas2d_arc(cv, x + w - r, y + r,     r,   -q, 0.0f,   false);  // top-right
    canvas2d_arc(cv, x + w - r, y + h - r, r, 0.0f,    q,   false);  // bottom-right
    canvas2d_arc(cv, x + r,     y + h - r, r,    q,   pi,   false);  // bottom-left
    canvas2d_arc(cv, x + r,     y + r,     r,   pi, pi + q, false);  // top-left
    canvas2d_close_path(cv);
    canvas2d_rec_leave(cv->rec);
}

// CSS border-radius overlap rule: reduce the scale factor `f` so that two radii
// summing to `sum` fit within an edge of length `len`.  `sum` 0 (no radii on the
// edge) divides to inf or NaN, which never passes g < f.
static float radii_fit(float f, float len, float sum) {
    float const g = len / sum;
    return g < f ? g : f;
}

static void round_rect_radii_impl(struct canvas2d_context *__single cv, float x, float y,
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
    // scalar canvas2d_round_rect's convention and keeping the scaling math finite.
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
    canvas2d_move_to(cv, x + r[0], y);
    canvas2d_ellipse(cv, x + w - r[2], y + r[3],     r[2], r[3], 0.0f,   -q, 0.0f,   false);
    canvas2d_ellipse(cv, x + w - r[4], y + h - r[5], r[4], r[5], 0.0f, 0.0f,    q,   false);
    canvas2d_ellipse(cv, x + r[6],     y + h - r[7], r[6], r[7], 0.0f,    q,   pi,   false);
    canvas2d_ellipse(cv, x + r[0],     y + r[1],     r[0], r[1], 0.0f,   pi, pi + q, false);
    canvas2d_close_path(cv);
}

void canvas2d_round_rect_radii(struct canvas2d_context *__single cv, float x, float y,
                             float w, float h,
                             float tl_x, float tl_y, float tr_x, float tr_y,
                             float br_x, float br_y, float bl_x, float bl_y) {
    // Record as itself, then swallow the move_to/ellipse/close_path the impl
    // expands to (the impl's early return keeps this wrapper single-exit, the
    // arc_to pattern).
    if (cv->rec) {
        canvas2d_rec_floats(cv->rec, "round_rect_radii",
                        (float[]){ x, y, w, h, tl_x, tl_y, tr_x, tr_y,
                                   br_x, br_y, bl_x, bl_y }, 12);
        canvas2d_rec_enter(cv->rec);
    }
    round_rect_radii_impl(cv, x, y, w, h, tl_x, tl_y, tr_x, tr_y,
                          br_x, br_y, bl_x, bl_y);
    canvas2d_rec_leave(cv->rec);
}

static void arc_to_impl(struct canvas2d_context *__single cv, float x1, float y1, float x2, float y2,
                        float radius) {
    if (!cv->path.has_cur) {
        canvas2d_move_to(cv, x1, y1);
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
        canvas2d_line_to(cv, x1, y1);
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
        canvas2d_line_to(cv, x1, y1);  // collinear: no arc
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
    canvas2d_line_to(cv, t1x, t1y);
    canvas2d_arc(cv, cx, cy, radius, sa, ea, ccw);
    cv->cur_user = (canvas2d_vec2){ .x = t2x, .y = t2y };
}

void canvas2d_arc_to(struct canvas2d_context *__single cv, float x1, float y1, float x2, float y2,
                   float radius) {
    // Record `arc_to` as itself, then swallow the line_to/arc its impl issues.
    // The wrapper is single-exit, so leave always balances enter even though the
    // impl has several early returns.
    if (cv->rec) {
        canvas2d_rec_floats(cv->rec, "arc_to", (float[]){ x1, y1, x2, y2, radius }, 5);
        canvas2d_rec_enter(cv->rec);
    }
    arc_to_impl(cv, x1, y1, x2, y2, radius);
    canvas2d_rec_leave(cv->rec);
}

void canvas2d_close_path(struct canvas2d_context *__single cv) {
    if (cv->rec) { canvas2d_rec_op(cv->rec, "close_path"); }
    canvas2d_path_close(&cv->path);
    canvas2d_path_close(&cv->upath);
}

// Homogeneous w of a user-space point under the CTM (the projection plane's
// distance term; affine has w == i == 1 everywhere).
static float ctm_w(canvas2d_mat m, canvas2d_vec2 u) {
    return m.g * u.x + m.h * u.y + m.i;
}

// Project one user-space point to device space (full divide; the caller has
// guaranteed w > 0 by clipping).
static canvas2d_vec2 project(canvas2d_mat m, canvas2d_vec2 u) {
    float const w = m.g * u.x + m.h * u.y + m.i;
    float const inv = 1.0f / w;
    return (canvas2d_vec2){ .x = (m.a * u.x + m.c * u.y + m.e) * inv,
                        .y = (m.b * u.x + m.d * u.y + m.f) * inv };
}

// w-clip threshold: a vertex with w at or below this projects to garbage (it is
// at or behind the projection plane), so contours are clipped against w > W_EPS
// in homogeneous space BEFORE the divide.
#define CANVAS2D_W_EPS 1e-4f

// Sutherland-Hodgman clip of one USER-space contour against the half-space
// w > W_EPS, then project the survivors into `out` as one device subpath.  At an
// edge that crosses the plane, the crossing vertex is found by interpolating x,y
// linearly in the parameter where w == W_EPS (homogeneous interpolation: the
// projected result is exact for a straight edge).  A contour fully behind the
// plane contributes nothing.  `closed` controls whether the last->first edge is
// also tested (fills and stroke outlines are closed; an open polyline is not).
static void clip_project_contour(canvas2d_mat m, canvas2d_vec2 const *__counted_by(n) src,
                                 int n, bool closed, struct canvas2d_path *__single out) {
    if (n < 1) {
        return;
    }
    bool first = true;
    int const last = closed ? n : n - 1;
    for (int k = 0; k < (closed ? n : last); k++) {
        canvas2d_vec2 const a = src[k];
        canvas2d_vec2 const b = src[(k + 1) % n];
        float const wa = ctm_w(m, a);
        float const wb = ctm_w(m, b);
        bool const ina = wa > CANVAS2D_W_EPS;
        bool const inb = wb > CANVAS2D_W_EPS;
        // Emit `a` if inside (the start vertex of each edge; a closed walk covers
        // every vertex once this way).
        if (ina) {
            canvas2d_vec2 const d = project(m, a);
            if (first) { canvas2d_path_move_to(out, d); first = false; }
            else       { canvas2d_path_line_to(out, d); }
        }
        // Emit the crossing vertex when the edge straddles the plane.
        if (ina != inb) {
            float const t = (CANVAS2D_W_EPS - wa) / (wb - wa);
            canvas2d_vec2 const x = { .x = a.x + t * (b.x - a.x),
                                  .y = a.y + t * (b.y - a.y) };
            canvas2d_vec2 const d = project(m, x);
            if (first) { canvas2d_path_move_to(out, d); first = false; }
            else       { canvas2d_path_line_to(out, d); }
        }
    }
    if (!first) {
        canvas2d_path_close(out);
    }
}

// Build the device-space path for a perspective fill/clip: w-clip + project every
// subpath of the USER-space path `up` into cv->pclip (each subpath implicitly
// closed, as the fill rasterizer treats them).  Returns the device path.
static struct canvas2d_path const *perspective_fill_path(struct canvas2d_context *__single cv,
                                                     struct canvas2d_path const *up) {
    canvas2d_path_reset(&cv->pclip);
    canvas2d_mat const m = cv->cur.ctm;
    for (int s = 0; s < up->nsubs; s++) {
        canvas2d_subpath const sp = up->subs[s];
        if (sp.count < 2) {
            continue;
        }
        clip_project_contour(m, up->pts + sp.start, sp.count, true, &cv->pclip);
    }
    return &cv->pclip;
}

// Rasterize a device-space path under `rule` and paint it with the fill paint
// over its clamped bbox.
static void fill_device_path(struct canvas2d_context *__single cv, struct canvas2d_path const *p,
                             enum canvas2d_fill_rule rule) {
    cbbox const b = points_bbox(cv, p->pts, p->npts, filter_margin(cv));
    if (b.w <= 0 || b.h <= 0 || !canvas2d_ensure_tile(cv, b.w * b.h) ||
        !canvas2d_cover_reset(&cv->cover, b.w, b.h)) {
        return;
    }
    cover_path_edges(cv, b, p);
    canvas2d_cover_resolve(&cv->cover, b.w, b.h, rule, cv->cov);
    paint_fill(cv, b);
}

void canvas2d_fill(struct canvas2d_context *__single cv, enum canvas2d_fill_rule rule) {
    if (cv->rec) { canvas2d_rec_rule(cv->rec, "fill", rule); }
    // Affine: the device path is correct and rasterizes bit-identically.
    // Perspective: w-clip + project the user-space path first.
    struct canvas2d_path const *p = canvas2d_mat_is_affine(cv->cur.ctm)
                                    ? &cv->path
                                    : perspective_fill_path(cv, &cv->upath);
    fill_device_path(cv, p, rule);
}

// Point-in-path for hit testing.  Each subpath is treated as implicitly closed
// (as the fill rasterizer does).  Casts a ray in +x from `q` and counts edge
// crossings: the signed count is the winding number (nonzero rule) and the raw
// count is the crossing number (even-odd rule).  The half-open vertical test
// (a.y <= q.y < b.y for an upward edge, and the reverse for downward) counts each
// shared vertex exactly once.
static bool path_contains(struct canvas2d_path const *p, canvas2d_vec2 q, enum canvas2d_fill_rule rule) {
    int wn = 0;   // winding number  (nonzero rule)
    int cn = 0;   // crossing number (even-odd rule)
    for (int s = 0; s < p->nsubs; s++) {
        canvas2d_subpath const sp = p->subs[s];
        if (sp.count < 2) {
            continue;
        }
        for (int k = 0; k < sp.count; k++) {
            canvas2d_vec2 const a = p->pts[sp.start + k];
            canvas2d_vec2 const b = p->pts[sp.start + (k + 1) % sp.count];
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
    return rule == CANVAS2D_EVENODD ? (cn & 1) != 0 : wn != 0;
}

bool canvas2d_is_point_in_path(struct canvas2d_context *__single cv, float x, float y,
                             enum canvas2d_fill_rule rule) {
    if (!isfinite(x) || !isfinite(y)) {
        return false;
    }
    return path_contains(&cv->path, xf(cv, x, y), rule);
}

void canvas2d_clip(struct canvas2d_context *__single cv, enum canvas2d_fill_rule rule) {
    if (cv->rec) { canvas2d_rec_rule(cv->rec, "clip", rule); }
    int const n = cv->width * cv->height;
    uint8_t *nm = malloc((size_t)n);
    if (!nm) {
        return;
    }
    // Affine clips the device path directly; perspective w-clips + projects the
    // user path first (its subpaths are implicitly closed, like a fill).
    struct canvas2d_path const *p = canvas2d_mat_is_affine(cv->cur.ctm)
                                    ? &cv->path
                                    : perspective_fill_path(cv, &cv->upath);
    // Rasterize the path's coverage into cv->cov over its (clamped) bbox.
    cbbox b = points_bbox(cv, p->pts, p->npts, 0);  // the clip is unfiltered
    if (b.w > 0 && b.h > 0 && canvas2d_ensure_tile(cv, b.w * b.h) &&
        canvas2d_cover_reset(&cv->cover, b.w, b.h)) {
        cover_path_edges(cv, b, p);
        canvas2d_cover_resolve(&cv->cover, b.w, b.h, rule, cv->cov);
    } else {
        b.w = 0;
        b.h = 0;  // empty path: clip to nothing
    }
    // new_clip = old_clip * path_coverage, zero outside the path's bbox.  Outside
    // the bbox pc == 0, so that region is all zero: memset it once and only
    // multiply within the bbox (clamped to the canvas) -- the common small clip
    // path then skips most of the canvas instead of multiplying it by zero.
    memset(nm, 0, (size_t)n);
    int const x0 = b.x < 0 ? 0 : b.x;
    int const y0 = b.y < 0 ? 0 : b.y;
    int const x1 = b.x + b.w > cv->width ? cv->width : b.x + b.w;
    int const y1 = b.y + b.h > cv->height ? cv->height : b.y + b.h;
    for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
            int const i = yy * cv->width + xx;
            int const pc = cv->cov[(yy - b.y) * b.w + (xx - b.x)];
            int const old = cv->cur.clip_mask ? cv->cur.clip_mask[i] : 255;
            nm[i] = (uint8_t)(old * pc / 255);
        }
    }
    free(cv->cur.clip_mask);
    cv->cur.clip_mask = nm;
    cv->cur.clip_len = n;
}

void canvas2d_set_stroke_rgba(struct canvas2d_context *__single cv, enum canvas2d_color_space space,
                            float r, float g, float b, float a) {
    if (cv->rec) { canvas2d_rec_floats_cs(cv->rec, "set_stroke_rgba", (float[]){ r, g, b, a }, 4, space); }
    cv->cur.stroke = intern_color(cv, space, r, g, b, a);
    cv->cur.stroke_kind = CANVAS2D_PAINT_SOLID;
}

void canvas2d_set_line_width(struct canvas2d_context *__single cv, float width) {
    if (isfinite(width)) {  // a non-finite width would not reparse; ignore it
        if (cv->rec) { canvas2d_rec_floats(cv->rec, "set_line_width", (float[]){ width }, 1); }
        cv->cur.line_width = width;
    }
}

void canvas2d_set_line_join(struct canvas2d_context *__single cv, enum canvas2d_line_join join) {
    if (cv->rec) { canvas2d_rec_line_join(cv->rec, join); }
    cv->cur.line_join = join;
}

void canvas2d_set_line_cap(struct canvas2d_context *__single cv, enum canvas2d_line_cap cap) {
    if (cv->rec) { canvas2d_rec_line_cap(cv->rec, cap); }
    cv->cur.line_cap = cap;
}

void canvas2d_set_miter_limit(struct canvas2d_context *__single cv, float limit) {
    if (isfinite(limit)) {  // a non-finite limit would not reparse; ignore it
        if (cv->rec) { canvas2d_rec_floats(cv->rec, "set_miter_limit", (float[]){ limit }, 1); }
        cv->cur.miter_limit = limit;
    }
}

void canvas2d_set_line_dash(struct canvas2d_context *__single cv,
                          float const *__counted_by(count) pattern, int count) {
    // A non-finite dash length would not reparse, so ignore the whole call.
    for (int i = 0; i < count; i++) {
        if (!isfinite(pattern[i])) {
            return;
        }
    }
    // Clamp into a separate variable: mutating `count` would desync the
    // __counted_by(count) bound on `pattern`.
    int m = count < 0 ? 0 : count;
    if (m > CANVAS2D_DASH_MAX) {
        m = CANVAS2D_DASH_MAX;
    }
    for (int i = 0; i < m; i++) {
        cv->cur.dash[i] = pattern[i];
    }
    cv->cur.dash_count = m;
    // Record the effective (clamped) pattern, so the line never exceeds the
    // parser's per-line dash cap and re-clamps to the same state on replay.
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "set_line_dash", cv->cur.dash, m); }
}

int canvas2d_get_line_dash(struct canvas2d_context *__single cv,
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

void canvas2d_set_line_dash_offset(struct canvas2d_context *__single cv, float offset) {
    if (isfinite(offset)) {  // a non-finite offset would not reparse; ignore it
        if (cv->rec) { canvas2d_rec_floats(cv->rec, "set_line_dash_offset", (float[]){ offset }, 1); }
        cv->cur.dash_offset = offset;
    }
}

// Build the stroke triangles for `p` into cv->scratch_verts under the current
// line styles (width/join/cap/dash, CTM scale baked in).  False on alloc failure.
static bool build_stroke_verts(struct canvas2d_context *__single cv, struct canvas2d_path const *p) {
    canvas2d_verts_reset(&cv->scratch_verts);
    // Line width and dash lengths are in user units; bake the CTM scale in.
    float const scale = ctm_scale(cv->cur.ctm);
    float const half_width = cv->cur.line_width * 0.5f * scale;

    bool const dashed = cv->cur.dash_count > 0;
    float sdash[CANVAS2D_DASH_MAX];
    for (int i = 0; i < cv->cur.dash_count; i++) {
        sdash[i] = cv->cur.dash[i] * scale;
    }
    float const soff = cv->cur.dash_offset * scale;

    for (int s = 0; s < p->nsubs; s++) {
        canvas2d_subpath const sp = p->subs[s];
        if (sp.count < 2) {
            continue;
        }
        canvas2d_vec2 *poly = p->pts + sp.start;
        bool ok = dashed
                      ? canvas2d_stroke_dashed(poly, sp.count, sp.closed, half_width, sdash,
                                           cv->cur.dash_count, soff,
                                           &cv->scratch_verts)
                      : canvas2d_stroke_polyline(poly, sp.count, sp.closed, half_width,
                                             cv->cur.line_join, cv->cur.line_cap,
                                             cv->cur.miter_limit,
                                             &cv->scratch_verts);
        if (!ok) {
            return false;
        }
    }
    return true;
}

static void stroke_device_path(struct canvas2d_context *__single cv, struct canvas2d_path const *p) {
    if (!build_stroke_verts(cv, p) || cv->scratch_verts.nverts < 3) {
        return;
    }
    cbbox b = points_bbox(cv, cv->scratch_verts.data, cv->scratch_verts.nverts,
                           filter_margin(cv));
    if (b.w <= 0 || b.h <= 0 || !canvas2d_ensure_tile(cv, b.w * b.h) ||
        !canvas2d_cover_reset(&cv->cover, b.w, b.h)) {
        return;
    }
    // Feed each stroke triangle as edges, forced to a consistent winding so the
    // overlapping join/cap triangles union (nonzero) instead of cancelling.
    for (int i = 0; i + 2 < cv->scratch_verts.nverts; i += 3) {
        canvas2d_vec2 const p0 = cv->scratch_verts.data[i];
        canvas2d_vec2 p1 = cv->scratch_verts.data[i + 1];
        canvas2d_vec2 p2 = cv->scratch_verts.data[i + 2];
        float const area = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
        if (area < 0.0f) {
            canvas2d_vec2 const t = p1;
            p1 = p2;
            p2 = t;
        }
        cover_edge(cv, b, p0, p1);
        cover_edge(cv, b, p1, p2);
        cover_edge(cv, b, p2, p0);
    }
    canvas2d_cover_resolve(&cv->cover, b.w, b.h, CANVAS2D_NONZERO, cv->cov);
    paint_stroke(cv, b);
}

// Build the stroke triangles for the USER-space path `up` into cv->scratch_verts,
// width and dash lengths in USER units (NO CTM scale -- the outline is projected
// afterward, so foreshortening falls out of the projection).  The perspective
// twin of build_stroke_verts.
static bool build_stroke_verts_user(struct canvas2d_context *__single cv, struct canvas2d_path const *up) {
    canvas2d_verts_reset(&cv->scratch_verts);
    float const half_width = cv->cur.line_width * 0.5f;
    bool const dashed = cv->cur.dash_count > 0;
    float const soff = cv->cur.dash_offset;
    for (int s = 0; s < up->nsubs; s++) {
        canvas2d_subpath const sp = up->subs[s];
        if (sp.count < 2) {
            continue;
        }
        canvas2d_vec2 *poly = up->pts + sp.start;
        bool ok = dashed
                      ? canvas2d_stroke_dashed(poly, sp.count, sp.closed, half_width,
                                           cv->cur.dash, cv->cur.dash_count, soff,
                                           &cv->scratch_verts)
                      : canvas2d_stroke_polyline(poly, sp.count, sp.closed, half_width,
                                             cv->cur.line_join, cv->cur.line_cap,
                                             cv->cur.miter_limit, &cv->scratch_verts);
        if (!ok) {
            return false;
        }
    }
    return true;
}

// Stroke under a perspective CTM: build the outline triangles in USER space, then
// w-clip + project each one and rasterize the survivors.  Each clipped triangle
// becomes a small convex polygon; feeding its edges (forced winding) unions them
// with nonzero coverage exactly as the affine stroker does with raw triangles.
static void stroke_perspective_path(struct canvas2d_context *__single cv, struct canvas2d_path const *up) {
    if (!build_stroke_verts_user(cv, up) || cv->scratch_verts.nverts < 3) {
        return;
    }
    canvas2d_mat const m = cv->cur.ctm;
    // Project the user-space triangles (clipped) into cv->pclip for both the bbox
    // and the rasterization (one pass, no per-triangle re-clip).
    canvas2d_path_reset(&cv->pclip);
    for (int i = 0; i + 2 < cv->scratch_verts.nverts; i += 3) {
        canvas2d_vec2 const tri[3] = { cv->scratch_verts.data[i],
                                   cv->scratch_verts.data[i + 1],
                                   cv->scratch_verts.data[i + 2] };
        clip_project_contour(m, tri, 3, true, &cv->pclip);
    }
    if (cv->pclip.npts < 3) {
        return;
    }
    cbbox b = points_bbox(cv, cv->pclip.pts, cv->pclip.npts, filter_margin(cv));
    if (b.w <= 0 || b.h <= 0 || !canvas2d_ensure_tile(cv, b.w * b.h) ||
        !canvas2d_cover_reset(&cv->cover, b.w, b.h)) {
        return;
    }
    // Each projected polygon is a subpath; force a consistent winding (its signed
    // area) so the overlapping join/cap shapes union under nonzero.
    for (int s = 0; s < cv->pclip.nsubs; s++) {
        canvas2d_subpath const sp = cv->pclip.subs[s];
        if (sp.count < 3) {
            continue;
        }
        canvas2d_vec2 *poly = cv->pclip.pts + sp.start;
        // Signed area; reverse edge order for a clockwise polygon so every
        // polygon contributes the same winding sign.
        float area2 = 0.0f;
        for (int k = 0; k < sp.count; k++) {
            canvas2d_vec2 const a = poly[k];
            canvas2d_vec2 const c = poly[(k + 1) % sp.count];
            area2 += a.x * c.y - c.x * a.y;
        }
        for (int k = 0; k < sp.count; k++) {
            canvas2d_vec2 const a = poly[k];
            canvas2d_vec2 const c = poly[(k + 1) % sp.count];
            if (area2 < 0.0f) { cover_edge(cv, b, c, a); }
            else              { cover_edge(cv, b, a, c); }
        }
    }
    canvas2d_cover_resolve(&cv->cover, b.w, b.h, CANVAS2D_NONZERO, cv->cov);
    paint_stroke(cv, b);
}

void canvas2d_stroke(struct canvas2d_context *__single cv) {
    if (cv->rec) { canvas2d_rec_op(cv->rec, "stroke"); }
    if (canvas2d_mat_is_affine(cv->cur.ctm)) {
        stroke_device_path(cv, &cv->path);
    } else {
        stroke_perspective_path(cv, &cv->upath);
    }
}

// Twice the signed area of triangle (a,b,c); its sign is the winding, zero means
// the three points are collinear (a degenerate triangle).
static float orient(canvas2d_vec2 a, canvas2d_vec2 b, canvas2d_vec2 c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Whether q lies in triangle (a,b,c) -- on the same side of all three edges,
// winding-agnostic, boundary counts as inside.  A degenerate triangle has no
// interior, so it never reports a hit (guards against the stroker's zero-area
// triangles swallowing every query).
static bool point_in_tri(canvas2d_vec2 q, canvas2d_vec2 a, canvas2d_vec2 b, canvas2d_vec2 c) {
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
static bool stroke_verts_contain(struct canvas2d_context *__single cv, canvas2d_vec2 q) {
    for (int i = 0; i + 2 < cv->scratch_verts.nverts; i += 3) {
        if (point_in_tri(q, cv->scratch_verts.data[i], cv->scratch_verts.data[i + 1],
                         cv->scratch_verts.data[i + 2])) {
            return true;
        }
    }
    return false;
}

bool canvas2d_is_point_in_stroke(struct canvas2d_context *__single cv, float x, float y) {
    if (!isfinite(x) || !isfinite(y)) {
        return false;
    }
    // Build the same stroke triangles canvas2d_stroke would paint, then test the
    // (transformed) query point against their union.
    if (!build_stroke_verts(cv, &cv->path)) {
        return false;
    }
    return stroke_verts_contain(cv, xf(cv, x, y));
}

void canvas2d_stroke_rect(struct canvas2d_context *__single cv, float x, float y, float w, float h) {
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "stroke_rect", (float[]){ x, y, w, h }, 4); }
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h)) {
        return;  // Canvas spec: non-finite args paint nothing.
    }
    // strokeRect builds and strokes its own rectangle without touching the
    // current path; the corners go through the CTM exactly as fill_rect's quad.
    // Perspective strokes from a USER-space rect (the perspective stroker projects
    // the outline); affine bakes the CTM into the device-space rect as before.
    bool const persp = !canvas2d_mat_is_affine(cv->cur.ctm);
    struct canvas2d_path rp;
    canvas2d_path_init(&rp);
    if (w == 0.0f && h == 0.0f) {
        // Spec: a single-point subpath.  The stroker emits nothing for a
        // zero-length subpath (no caps on a bare point) -- a deviation here.
        canvas2d_path_move_to(&rp, persp ? uv(x, y) : xf(cv, x, y));
    } else if (w == 0.0f || h == 0.0f) {
        // A degenerate rect is a hairline: an open two-point subpath, so caps
        // (not joins) bracket it.  The far corner coincides for both axes.
        canvas2d_path_move_to(&rp, persp ? uv(x, y) : xf(cv, x, y));
        canvas2d_path_line_to(&rp, persp ? uv(x + w, y + h) : xf(cv, x + w, y + h));
    } else if (persp) {
        canvas2d_path_rect(&rp, uv(x, y), uv(x + w, y), uv(x + w, y + h), uv(x, y + h));
    } else {
        canvas2d_path_rect(&rp, xf(cv, x, y), xf(cv, x + w, y),
                       xf(cv, x + w, y + h), xf(cv, x, y + h));
    }
    if (persp) {
        stroke_perspective_path(cv, &rp);
    } else {
        stroke_device_path(cv, &rp);
    }
    canvas2d_path_free(&rp);
}

// The current fontStyle as the boundary's bool italic.
static bool cur_italic(struct canvas2d_context const *__single cv) {
    return cv->cur.font_style == CANVAS2D_FONT_STYLE_ITALIC;
}

// Rebuild the cached font when the requested size, family, weight, style, OR
// stretch changes; NULL on failure.  The family is matched by (length, bytes) --
// the sized model -- so a switch between two families of the same size still
// rebuilds, and weight/style/stretch changes (a different real or synthesized
// face) rebuild too.  fontVariantCaps is deliberately NOT a trigger: it is
// glyph SELECTION within the shaped runs, not the primary metrics/reference
// face this handle is (its vmetrics and font-wide box are caps-independent).
static struct canvas2d_font *__single ensure_font(struct canvas2d_context *__single cv) {
    bool const family_changed =
        cv->font_built_family_len != cv->cur.font_family_len ||
        memcmp(cv->font_built_family, cv->cur.font_family,
               (size_t)cv->cur.font_family_len) != 0;
    if (!cv->font || fabsf(cv->font_built_size - cv->cur.font_size) > 1e-6f ||
        family_changed || cv->font_built_weight != cv->cur.font_weight ||
        cv->font_built_style != cv->cur.font_style ||
        cv->font_built_stretch != cv->cur.font_stretch) {
        canvas2d_font_free(cv->font);
        cv->font = canvas2d_font(cv->cur.font_family, cv->cur.font_family_len,
                                    cv->cur.font_size, cv->cur.font_weight,
                                    cur_italic(cv), (int)cv->cur.font_stretch);
        cv->font_built_size = cv->cur.font_size;
        memcpy(cv->font_built_family, cv->cur.font_family,
               (size_t)cv->cur.font_family_len);
        cv->font_built_family_len = cv->cur.font_family_len;
        cv->font_built_weight = cv->cur.font_weight;
        cv->font_built_style = cv->cur.font_style;
        cv->font_built_stretch = cv->cur.font_stretch;
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
static bool canvas2d_vmetrics(struct canvas2d_context *__single cv, float *__single ascent,
                            float *__single descent) {
    float const size = cv->cur.font_size;
    int fid = canvas2d_text_cache_intern(&cv->text_cache, cv->cur.font_family,
                                     cv->cur.font_family_len, cv->cur.font_weight,
                                     cur_italic(cv));
    float a1 = 0.0f, d1 = 0.0f;
    if (canvas2d_text_cache_get_vmetrics(&cv->text_cache, fid, &a1, &d1)) {
        *ascent = a1 * size;
        *descent = d1 * size;
        return true;
    }
    struct canvas2d_font *__single f = ensure_font(cv);
    if (!f) {
        return false;
    }
    float a = 0.0f, d = 0.0f;
    canvas2d_font_vmetrics(f, &a, &d);
    if (fid >= 0 && size > 0.0f) {
        canvas2d_text_cache_set_vmetrics(&cv->text_cache, fid, a / size, d / size);
        if (canvas2d_text_cache_get_vmetrics(&cv->text_cache, fid, &a1, &d1)) {
            *ascent = a1 * size;  // re-derive through the stored record, so the
            *descent = d1 * size; // live values match a future replay's exactly
            return true;
        }
    }
    *ascent = a;
    *descent = d;
    return true;
}

void canvas2d_set_font_size(struct canvas2d_context *__single cv, float px) {
    cv->cur.font_size = px > 0.0f ? px : 0.0f;  // non-positive / NaN -> 0
    // Record the stored (sanitized, finite) value, not the raw argument.
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "set_font_size", (float[]){ cv->cur.font_size }, 1); }
}

// fontFamily is canvas state, like fontSize: it rides save/restore and resets to
// the default.  The name is copied into the fixed state buffer, truncated to its
// capacity; an empty name is ignored (the "ignore invalid" setter pattern),
// keeping the current family.  Recorded as set_font_family with the stored
// (possibly truncated) bytes, so replay restores the same family.  The
// length-counted core: the replay parser stays in the indexable world (no
// __null_terminated seam), exactly as canvas2d_fill_text_n serves canvas2d_fill_text.
void canvas2d_set_font_family_n(struct canvas2d_context *__single cv,
                              char const *__counted_by(len) name, int len) {
    if (len <= 0) {
        return;  // empty/invalid: ignore, keep current
    }
    // Truncate to the state buffer's capacity (the copy reads only `keep` bytes,
    // which `len` still bounds).
    int const keep = len > CANVAS2D_FONT_FAMILY_MAX ? CANVAS2D_FONT_FAMILY_MAX : len;
    memcpy(cv->cur.font_family, name, (size_t)keep);
    cv->cur.font_family_len = keep;
    if (cv->rec) { canvas2d_rec_font_family(cv->rec, cv->cur.font_family, keep); }
}

void canvas2d_set_font_family(struct canvas2d_context *__single cv, char const *__null_terminated name) {
    if (!name) {
        return;  // NULL: ignore, keep current
    }
    int const len = (int)strlen(name);
    canvas2d_set_font_family_n(cv, __null_terminated_to_indexable(name), len);
}

// fontWeight/fontStyle are canvas state like fontFamily: they ride save/restore
// and reset to 400 / NORMAL.  Weight is clamped to the CSS [100, 900] axis;
// an unrecognized style enum is ignored (the "ignore invalid" setter pattern).
// Both record their sanitized value -- set_font_weight an int, set_font_style a
// token -- so replay restores the same identity (and the shaping/font blocks
// carry weight/style too, since they are the glyph-cache key for synthesized
// faces).
void canvas2d_set_font_weight(struct canvas2d_context *__single cv, int weight) {
    int const w = weight < 100 ? 100 : (weight > 900 ? 900 : weight);
    cv->cur.font_weight = w;
    if (cv->rec) { canvas2d_rec_ints(cv->rec, "set_font_weight", (int[]){ w }, 1); }
}

void canvas2d_set_font_style(struct canvas2d_context *__single cv, enum canvas2d_font_style style) {
    switch (style) {
        case CANVAS2D_FONT_STYLE_NORMAL:
        case CANVAS2D_FONT_STYLE_ITALIC:
            cv->cur.font_style = style;
            if (cv->rec) { canvas2d_rec_font_style(cv->rec, style); }
            break;
    }
}

// fontKerning / textRendering / lang are canvas state like the font setters
// above: they ride save/restore and reset to AUTO / AUTO / "".  An unrecognized
// kerning/rendering enum is ignored (the "ignore invalid" setter pattern); the
// lang tag is copied into the fixed state buffer truncated to capacity, and a
// NULL tag is ignored (pass "" to clear).  Each records a state op -- the enums
// by token, lang length-prefixed like set_font_family -- so replay restores the
// same shaping inputs (the shaping block carries them too, as cache-key parts).
void canvas2d_set_font_kerning(struct canvas2d_context *__single cv, enum canvas2d_font_kerning kerning) {
    switch (kerning) {
        case CANVAS2D_FONT_KERNING_AUTO:
        case CANVAS2D_FONT_KERNING_NORMAL:
        case CANVAS2D_FONT_KERNING_NONE:
            cv->cur.font_kerning = kerning;
            if (cv->rec) { canvas2d_rec_font_kerning(cv->rec, kerning); }
            break;
    }
}

void canvas2d_set_text_rendering(struct canvas2d_context *__single cv, enum canvas2d_text_rendering rendering) {
    switch (rendering) {
        case CANVAS2D_TEXT_RENDERING_AUTO:
        case CANVAS2D_TEXT_RENDERING_OPTIMIZE_SPEED:
        case CANVAS2D_TEXT_RENDERING_OPTIMIZE_LEGIBILITY:
        case CANVAS2D_TEXT_RENDERING_GEOMETRIC_PRECISION:
            cv->cur.text_rendering = rendering;
            if (cv->rec) { canvas2d_rec_text_rendering(cv->rec, rendering); }
            break;
    }
}

// The length-counted core (the replay parser stays in the indexable world, no
// __null_terminated seam, exactly as canvas2d_set_font_family_n serves the family).
// Unlike the family, an empty tag is the valid "no language" value, not an
// ignored call: len == 0 clears, len < 0 (only an internal misuse) is ignored.
void canvas2d_set_lang_n(struct canvas2d_context *__single cv,
                       char const *__counted_by(len) tag, int len) {
    if (len < 0) {
        return;
    }
    // Truncate to the state buffer's capacity (the copy reads only `keep` bytes,
    // which `len` still bounds).
    int const keep = len > CANVAS2D_LANG_MAX ? CANVAS2D_LANG_MAX : len;
    if (keep > 0) {
        memcpy(cv->cur.lang, tag, (size_t)keep);
    }
    cv->cur.lang_len = keep;
    if (cv->rec) { canvas2d_rec_lang(cv->rec, cv->cur.lang, keep); }
}

void canvas2d_set_lang(struct canvas2d_context *__single cv, char const *__null_terminated tag) {
    if (!tag) {
        return;  // NULL: ignore, keep current (pass "" to clear)
    }
    int const len = (int)strlen(tag);
    canvas2d_set_lang_n(cv, __null_terminated_to_indexable(tag), len);
}

// fontVariantCaps / fontStretch are canvas state like the toggles above: they
// ride save/restore and reset to NORMAL / NORMAL.  An unrecognized enum is
// ignored (the "ignore invalid" setter pattern).  Each records a state op (by
// token) so replay restores the same shaping inputs (the shaping block carries
// them too, as cache-key parts).  variantCaps changes glyph selection within the
// shaped runs; stretch resolves a different width face (and so rebuilds cv->font
// on the next ensure_font, the stretch check there).
void canvas2d_set_font_variant_caps(struct canvas2d_context *__single cv,
                                  enum canvas2d_font_variant_caps variant_caps) {
    switch (variant_caps) {
        case CANVAS2D_FONT_VARIANT_CAPS_NORMAL:
        case CANVAS2D_FONT_VARIANT_CAPS_SMALL_CAPS:
        case CANVAS2D_FONT_VARIANT_CAPS_ALL_SMALL_CAPS:
            cv->cur.font_variant_caps = variant_caps;
            if (cv->rec) { canvas2d_rec_font_variant_caps(cv->rec, variant_caps); }
            break;
    }
}

void canvas2d_set_font_stretch(struct canvas2d_context *__single cv, enum canvas2d_font_stretch stretch) {
    switch (stretch) {
        case CANVAS2D_FONT_STRETCH_ULTRA_CONDENSED:
        case CANVAS2D_FONT_STRETCH_EXTRA_CONDENSED:
        case CANVAS2D_FONT_STRETCH_CONDENSED:
        case CANVAS2D_FONT_STRETCH_SEMI_CONDENSED:
        case CANVAS2D_FONT_STRETCH_NORMAL:
        case CANVAS2D_FONT_STRETCH_SEMI_EXPANDED:
        case CANVAS2D_FONT_STRETCH_EXPANDED:
        case CANVAS2D_FONT_STRETCH_EXTRA_EXPANDED:
        case CANVAS2D_FONT_STRETCH_ULTRA_EXPANDED:
            cv->cur.font_stretch = stretch;
            if (cv->rec) { canvas2d_rec_font_stretch(cv->rec, stretch); }
            break;
    }
}

// letterSpacing/wordSpacing are canvas state, recorded as ordinary state ops
// exactly like font_size and direction.  The spacing is also baked into a shaped
// line's advances and is part of the cache key, so the shaping block a fill_text
// emits carries ls/ws too -- but the canvas's spacing state, the value a
// fill_text keys its lookup by, is owned by these ops.
void canvas2d_set_letter_spacing(struct canvas2d_context *__single cv, float px) {
    cv->cur.letter_spacing = isfinite(px) ? px : 0.0f;  // NaN/inf -> 0
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "set_letter_spacing",
                                   (float[]){ cv->cur.letter_spacing }, 1); }
}

void canvas2d_set_word_spacing(struct canvas2d_context *__single cv, float px) {
    cv->cur.word_spacing = isfinite(px) ? px : 0.0f;  // NaN/inf -> 0
    if (cv->rec) { canvas2d_rec_floats(cv->rec, "set_word_spacing",
                                   (float[]){ cv->cur.word_spacing }, 1); }
}

void canvas2d_set_text_align(struct canvas2d_context *__single cv, enum canvas2d_text_align align) {
    switch (align) {
        case CANVAS2D_ALIGN_START:
        case CANVAS2D_ALIGN_END:
        case CANVAS2D_ALIGN_LEFT:
        case CANVAS2D_ALIGN_RIGHT:
        case CANVAS2D_ALIGN_CENTER:
            if (cv->rec) { canvas2d_rec_text_align(cv->rec, align); }
            cv->cur.text_align = align;
            break;
    }
}

void canvas2d_set_direction(struct canvas2d_context *__single cv, enum canvas2d_direction dir) {
    switch (dir) {
        case CANVAS2D_DIRECTION_LTR:
        case CANVAS2D_DIRECTION_RTL:
            if (cv->rec) { canvas2d_rec_direction(cv->rec, dir); }
            cv->cur.direction = dir;
            break;
    }
}

void canvas2d_set_text_baseline(struct canvas2d_context *__single cv, enum canvas2d_text_baseline baseline) {
    switch (baseline) {
        case CANVAS2D_BASELINE_ALPHABETIC:
        case CANVAS2D_BASELINE_TOP:
        case CANVAS2D_BASELINE_HANGING:
        case CANVAS2D_BASELINE_MIDDLE:
        case CANVAS2D_BASELINE_IDEOGRAPHIC:
        case CANVAS2D_BASELINE_BOTTOM:
            if (cv->rec) { canvas2d_rec_text_baseline(cv->rec, baseline); }
            cv->cur.text_baseline = baseline;
            break;
    }
}

// Fraction of the advance the textAlign anchor sits from the text's left edge:
// left 0, center 0.5, right 1.  start/end resolve through the direction
// attribute -- start is the edge the text flows from (left under ltr, right
// under rtl), end the edge it flows toward.
static float text_align_frac(enum canvas2d_text_align a, enum canvas2d_direction dir) {
    bool const rtl = dir == CANVAS2D_DIRECTION_RTL;
    switch (a) {
        case CANVAS2D_ALIGN_START:  return rtl ? 1.0f : 0.0f;
        case CANVAS2D_ALIGN_END:    return rtl ? 0.0f : 1.0f;
        case CANVAS2D_ALIGN_LEFT:   return 0.0f;
        case CANVAS2D_ALIGN_CENTER: return 0.5f;
        case CANVAS2D_ALIGN_RIGHT:  return 1.0f;
    }
    return 0.0f;  // unreachable for a valid enum
}

// Offset added to the pen y to place the requested textBaseline at y, derived
// from the font's ascent/descent (no BASE table: hanging ~ top, ideographic ~
// bottom).  The metrics come through the cached vmetrics record, so a replayed
// program needs no font handle to place a baseline.
static float text_baseline_offset(struct canvas2d_context *__single cv) {
    if (cv->cur.text_baseline == CANVAS2D_BASELINE_ALPHABETIC) {
        return 0.0f;
    }
    float a = 0.0f, d = 0.0f;
    if (!canvas2d_vmetrics(cv, &a, &d)) {
        return 0.0f;
    }
    switch (cv->cur.text_baseline) {
        case CANVAS2D_BASELINE_ALPHABETIC:  return 0.0f;
        case CANVAS2D_BASELINE_TOP:         return a;
        case CANVAS2D_BASELINE_HANGING:     return a;
        case CANVAS2D_BASELINE_MIDDLE:      return (a - d) * 0.5f;
        case CANVAS2D_BASELINE_IDEOGRAPHIC: return -d;
        case CANVAS2D_BASELINE_BOTTOM:      return -d;
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
static struct canvas2d_shaped const *__single shape_text(struct canvas2d_context *__single cv,
                                              char const *__counted_by(len) text,
                                              int len) {
    return canvas2d_text_cache_shaping(&cv->text_cache,
                                 cv->cur.font_family, cv->cur.font_family_len,
                                 cv->cur.font_size,
                                 cv->cur.direction == CANVAS2D_DIRECTION_RTL,
                                 cv->cur.letter_spacing, cv->cur.word_spacing,
                                 cv->cur.font_weight, cur_italic(cv),
                                 (int)cv->cur.font_kerning, (int)cv->cur.text_rendering,
                                 (int)cv->cur.font_variant_caps, (int)cv->cur.font_stretch,
                                 cv->cur.lang, cv->cur.lang_len,
                                 text, len);
}

// Render one color (emoji) glyph from its canonical capture: pick the mip
// level pair around the glyph quad's device footprint and sample it through
// the same transform-aware trilinear path drawImage minification takes -- so
// the emoji takes the transform, clip, global alpha, and shadow like any
// other image, and no boundary call (indeed, no CTFontRef at all) is needed
// once the capture exists.  The capture covers the glyph-space rect
// [ink_x0, ink_x0 + capture_w] x [ink_y0, ink_y0 + capture_h] in capture px (y up,
// baseline-relative); scaling by size_px / CANVAS2D_CAPTURE_EM and pinning to the
// pen maps it to user space.
static void draw_glyph_capture(struct canvas2d_context *__single cv, struct canvas2d_glyph_slot *__single slot,
                               float pen_x, float baseline_y, float size_px) {
    float const k = size_px / (float)CANVAS2D_CAPTURE_EM;
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
    canvas2d_mat const m = cv->cur.ctm;
    float const ex = hypotf(m.a * dw, m.b * dw);
    float const ey = hypotf(m.c * dh, m.d * dh);
    canvas2d_mip hi, lo;
    float const lt = canvas2d_glyph_mip_pair(slot, ex > ey ? ex : ey, &hi, &lo);
    if (!hi.px) {
        return;
    }
    draw_image_quad(cv, hi.px, hi.len, hi.w, hi.h, 0.0f, 0.0f,
                    (float)hi.w, (float)hi.h, dx, dy, dw, dh, true,
                    CANVAS2D_COLOR_UNORM8, CANVAS2D_CS_SRGB, false, false, NULL,
                    hi, lo, lt);
}

// The degraded path when the capture cache can't serve (full table, OOM) but
// the run still has its boundary handle: ask the boundary for the ink box,
// draw into a checked RGBA8 buffer at device size, unpremultiply, then
// composite through the CTM with canvas2d_draw_bitmap_subrect.
static void draw_color_glyph(struct canvas2d_context *__single cv, void *__single font,
                             uint16_t glyph, float pen_x, float baseline_y) {
    float x0, y0, x1, y1;
    canvas2d_glyph_bounds(font, glyph, &x0, &y0, &x1, &y1);
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
    // buffer (canvas2d_glyph_draw is bitmap space: y up from the bottom).
    canvas2d_glyph_draw(font, glyph, (float)margin - x0, (float)margin - y0, buf, gw, gh);
    // CGBitmapContext gives premultiplied RGBA, top row first (the orientation
    // canvas2d_draw_bitmap_subrect wants); just unpremultiply to straight alpha.
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
    // The decoded glyph capture is sRGB RGBA8 -- tag it so it converts to the
    // working space on deposit like any other source.
    canvas2d_draw_bitmap_subrect(cv, CANVAS2D_CS_SRGB, buf, gw, gh, 0.0f, 0.0f,
                              (float)gw, (float)gh,
                              dest_x, dest_y, (float)gw, (float)gh);
    free(buf);
}

// canvas2d_shaped_outline's color-glyph callback context: the canvas plus the size
// the line was shaped at (the capture px scale's numerator).
struct color_glyph_ctx {
    struct canvas2d_context *__single cv;
    float size_px;
};

// canvas2d_shaped_outline's color-glyph callback: the context rides along untyped
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
    struct canvas2d_glyph_slot *__single slot =
        canvas2d_text_cache_color(&cc->cv->text_cache, fid, font, glyph);
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
// canvas2d_shaped_outline does the layout and hands color glyphs back through the
// callback above.
// Paint a shaped line.  `place_to` flattens glyph outlines into `cv->text_path`:
// affine, it is the full device transform (CTM, with any max-width condense) and
// the path lands in device space, rasterized byte-identically; perspective, it is
// only the user-space condense, so the path lands in USER space and is then
// w-clipped + projected through the CTM.  `persp` selects the two tails.
static void paint_shaped(struct canvas2d_context *__single cv, struct canvas2d_shaped const *__single s,
                         float ox, float oy, canvas2d_mat place_to, bool persp, bool stroke) {
    canvas2d_path_reset(&cv->text_path);
    struct color_glyph_ctx cc = { .cv = cv, .size_px = s->size_px };
    canvas2d_shaped_outline(&cv->text_cache, s, ox, oy, place_to, CANVAS2D_FLATTEN_TOL,
                        &cv->text_path, paint_color_glyph, &cc);
    if (!persp) {
        if (stroke) { stroke_device_path(cv, &cv->text_path); }
        else        { fill_device_path(cv, &cv->text_path, CANVAS2D_NONZERO); }
        return;
    }
    if (stroke) {
        stroke_perspective_path(cv, &cv->text_path);
    } else {
        fill_device_path(cv, perspective_fill_path(cv, &cv->text_path), CANVAS2D_NONZERO);
    }
}

// Shape once, place by textAlign/textBaseline (condensing to max_width if finite and
// positive), and paint.  One shaped line drives both the alignment advance and the
// glyphs, so emoji and fallback runs are measured the same way they are drawn.
static void do_text(struct canvas2d_context *__single cv, char const *__counted_by(len) text, int len,
                    float x, float y, float max_width, bool stroke) {
    if (!isfinite(x) || !isfinite(y)) {
        return;  // spec: fillText/strokeText with non-finite coordinates draw
    }            // nothing (and an inf pen would poison every glyph point)
    struct canvas2d_shaped const *__single s = shape_text(cv, text, len);
    if (!s) {
        return;
    }
    float const advance = canvas2d_shaped_width(s);
    float sx = 1.0f;
    if (isfinite(max_width) && max_width > 0.0f && advance > max_width) {
        sx = max_width / advance;  // condense in x to fit
    }
    float ox = x - text_align_frac(cv->cur.text_align, cv->cur.direction)
                       * advance * sx;
    float const oy = y + text_baseline_offset(cv);
    bool const persp = !canvas2d_mat_is_affine(cv->cur.ctm);
    // The max-width condense: scale x by sx about the anchor (X' = sx*X +
    // ox*(1-sx), Y' = Y), an affine in USER space.  Identity when sx == 1.
    canvas2d_mat cond = canvas2d_mat_identity();
    if (sx != 1.0f) {
        cond = (canvas2d_mat){ .a = sx, .b = 0.0f, .c = 0.0f, .d = 1.0f,
                           .e = ox * (1.0f - sx), .f = 0.0f,
                           .g = 0.0f, .h = 0.0f, .i = 1.0f };
    }
    if (persp) {
        // Flatten glyph outlines into user space (cond only); project later.
        paint_shaped(cv, s, ox, oy, cond, true, stroke);
        return;
    }
    // Affine: flatten straight to device space, byte-identical to before (the CTM
    // alone when sx == 1, else CTM . cond as the old code built it).
    canvas2d_mat const td = sx != 1.0f ? canvas2d_mat_mul(cv->cur.ctm, cond) : cv->cur.ctm;
    paint_shaped(cv, s, ox, oy, td, false, stroke);
}

float canvas2d_measure_text(struct canvas2d_context *__single cv, char const *__null_terminated text) {
    int const len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    struct canvas2d_shaped const *__single s = shape_text(cv, t, len);
    if (!s) {
        return 0.0f;
    }
    return canvas2d_shaped_width(s);
}

canvas2d_text_metrics canvas2d_measure_text_full(struct canvas2d_context *__single cv,
                                             char const *__null_terminated text) {
    canvas2d_text_metrics m;
    memset(&m, 0, sizeof m);  // all-zero if the font/shaping can't be built
    float a = 0.0f, d = 0.0f;
    bool const have_vm = canvas2d_vmetrics(cv, &a, &d);  // cached: no font handle needed
    int const len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    struct canvas2d_shaped const *__single s = shape_text(cv, t, len);
    if (have_vm && s) {
        canvas2d_shaped_metrics tm;  // fallback-aware: each glyph in its run's font
        canvas2d_measure_shaped(&cv->text_cache, s, cv->cur.font_size, a, d, &tm);
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

// Selection/caret queries: each shapes through the cache exactly like
// canvas2d_measure_text (so it sees the same font size, direction, and
// letterSpacing/wordSpacing baked into the advances) and forwards to the matching
// canvas2d_shaped_* query.  Read-only: no recording, no pixels.
int canvas2d_text_index_at_x(struct canvas2d_context *__single cv, char const *__null_terminated text,
                           float x) {
    int const len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    struct canvas2d_shaped const *__single s = shape_text(cv, t, len);
    if (!s) {
        return -1;
    }
    return canvas2d_shaped_index_at_x(s, x);
}

float canvas2d_text_x_at_index(struct canvas2d_context *__single cv, char const *__null_terminated text,
                             int index) {
    int const len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    struct canvas2d_shaped const *__single s = shape_text(cv, t, len);
    if (!s) {
        return 0.0f;
    }
    return canvas2d_shaped_x_at_index(s, index);
}

int canvas2d_text_selection(struct canvas2d_context *__single cv, char const *__null_terminated text,
                          int lo, int hi, canvas2d_text_span *__counted_by(max) out, int max) {
    if (!out || max <= 0) {
        return 0;
    }
    int const len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    struct canvas2d_shaped const *__single s = shape_text(cv, t, len);
    if (!s) {
        return 0;
    }
    // The public canvas2d_text_span and the internal canvas2d_xspan are layout-identical
    // ({float x0, x1;}), so the caller's buffer is reinterpreted in place: same
    // element size, same `max` count, same byte extent.  The cast feeds the call
    // directly -- canvas2d_shaped_selection's own __counted_by(max) parameter carries
    // the same bound, so no copy and no allocation, and it writes within `max`
    // exactly as for an internal canvas2d_xspan caller.
    return canvas2d_shaped_selection(s, lo, hi, (canvas2d_xspan *)out, max);
}

// The per-canvas text cache, for tests and stats -- declared in canvas2d_text.h so
// it stays off the public canvas.h surface (tests include internal headers).
struct canvas2d_text_cache *__single canvas2d_canvas_text_cache(struct canvas2d_context *__single cv) {
    return &cv->text_cache;
}

// Recording a text op: first make sure the cache holds everything the op is
// about to use (the family's vmetrics record and the shaped line -- the same
// lookups the draw takes, so this adds no boundary traffic), then serialize
// the not-yet-emitted font/glyph/shape blocks ahead of the op line.  The
// recorded program is self-contained: replay rebuilds the cache from the
// blocks and draws with no text boundary at all.
static void record_text_blocks(struct canvas2d_context *__single cv,
                               char const *__counted_by(len) text, int len) {
    float a = 0.0f, d = 0.0f;
    (void)canvas2d_vmetrics(cv, &a, &d);  // intern the family + its vmetrics
    (void)shape_text(cv, text, len);    // ensure the line is cached
    canvas2d_rec_text_blocks(cv->rec, &cv->text_cache,
                         cv->cur.font_family, cv->cur.font_family_len,
                         cv->cur.font_size,
                         cv->cur.direction == CANVAS2D_DIRECTION_RTL,
                         cv->cur.letter_spacing, cv->cur.word_spacing,
                         cv->cur.font_weight, cur_italic(cv),
                         (int)cv->cur.font_kerning, (int)cv->cur.text_rendering,
                         (int)cv->cur.font_variant_caps, (int)cv->cur.font_stretch,
                         cv->cur.lang, cv->cur.lang_len, text, len);
}

void canvas2d_fill_text_n(struct canvas2d_context *__single cv, char const *__counted_by(len) text,
                        int len, float x, float y) {
    if (cv->rec) {
        record_text_blocks(cv, text, len);
        canvas2d_rec_text(cv->rec, "fill_text", x, y, text, len);
    }
    do_text(cv, text, len, x, y, -1.0f, false);
}

void canvas2d_fill_text(struct canvas2d_context *__single cv, char const *__null_terminated text,
                      float x, float y) {
    int const len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    canvas2d_fill_text_n(cv, t, len, x, y);
}

void canvas2d_fill_text_max_n(struct canvas2d_context *__single cv, char const *__counted_by(len) text,
                            int len, float x, float y, float max_width) {
    if (cv->rec) {
        record_text_blocks(cv, text, len);
        canvas2d_rec_text_max(cv->rec, "fill_text_max", x, y, max_width, text, len);
    }
    do_text(cv, text, len, x, y, max_width, false);
}

void canvas2d_fill_text_max(struct canvas2d_context *__single cv, char const *__null_terminated text,
                          float x, float y, float max_width) {
    int const len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    canvas2d_fill_text_max_n(cv, t, len, x, y, max_width);
}

void canvas2d_stroke_text_n(struct canvas2d_context *__single cv, char const *__counted_by(len) text,
                          int len, float x, float y) {
    if (cv->rec) {
        record_text_blocks(cv, text, len);
        canvas2d_rec_text(cv->rec, "stroke_text", x, y, text, len);
    }
    do_text(cv, text, len, x, y, -1.0f, true);
}

void canvas2d_stroke_text(struct canvas2d_context *__single cv, char const *__null_terminated text,
                        float x, float y) {
    int const len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    canvas2d_stroke_text_n(cv, t, len, x, y);
}

void canvas2d_stroke_text_max_n(struct canvas2d_context *__single cv,
                              char const *__counted_by(len) text,
                              int len, float x, float y, float max_width) {
    if (cv->rec) {
        record_text_blocks(cv, text, len);
        canvas2d_rec_text_max(cv->rec, "stroke_text_max", x, y, max_width, text, len);
    }
    do_text(cv, text, len, x, y, max_width, true);
}

void canvas2d_stroke_text_max(struct canvas2d_context *__single cv, char const *__null_terminated text,
                            float x, float y, float max_width) {
    int const len = (int)strlen(text);
    char const *__counted_by(len) t = __null_terminated_to_indexable(text);
    canvas2d_stroke_text_max_n(cv, t, len, x, y, max_width);
}

// Bilinear sample of an RGBA8 source at source-pixel (fx, fy), unpremultiplied,
// clamp-to-edge.
static void sample_src(uint8_t const *__counted_by(slen) src, int slen,
                       int sw, int sh, float fx, float fy,
                       float *__counted_by(4) out) {
    (void)slen;
    float gx = fx - 0.5f, gy = fy - 0.5f;
    float fxx = floorf(gx), fyy = floorf(gy);
    int x0 = canvas2d_f2i(fxx), y0 = canvas2d_f2i(fyy);
    // canvas2d_f2i saturates a huge source coordinate (an inf/1e30 subrect maps
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
    int x = canvas2d_f2i(floorf(fx));
    int y = canvas2d_f2i(floorf(fy));
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
    // (canvas2d_f2i saturates huge coords to INT_MAX); the per-tap edge clamp
    // below does the real work.
    int bx = canvas2d_f2i(fxx), by = canvas2d_f2i(fyy);
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
    int x0 = canvas2d_f2i(fxx), y0 = canvas2d_f2i(fyy);
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
    int x = canvas2d_f2i(floorf(fx));
    int y = canvas2d_f2i(floorf(fy));
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
    int bx = canvas2d_f2i(fxx), by = canvas2d_f2i(fyy);
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
static int px_bpp(enum canvas2d_color_type ct) {
    return ct == CANVAS2D_COLOR_F16 ? 8 : 4;
}

enum { MIP_MAX_LEVELS = 16 };  // 1 << 15 > CANVAS2D_DIM_MAX: a chain always fits

static bool ensure_mips(struct canvas2d_context *__single cv, int n) {
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
// canvas2d_mip_halve (the emoji captures' kernel), the f16 arm through
// mip_halve_f16.  The four image formats are peers here: each colour type
// has its own halve, each alpha type its own level-0 entry.
static void fill_mips(uint8_t *__counted_by(total) dst, int total,
                      uint8_t const *__counted_by(slen) src, int slen,
                      int sw, int sh, enum canvas2d_color_type ct, bool premul,
                      mip_level const *__counted_by(MIP_MAX_LEVELS) lv, int n) {
    (void)slen;
    (void)total;
    if (premul) {
        memcpy(dst, src, (size_t)(sw * sh * px_bpp(ct)));
    } else if (ct == CANVAS2D_COLOR_F16) {
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
        if (ct == CANVAS2D_COLOR_F16) {
            mip_halve_f16(dst + lv[i - 1].off, lv[i - 1].w, lv[i - 1].h,
                          dst + lv[i].off, lv[i].w, lv[i].h);
        } else {
            canvas2d_mip_halve(dst + lv[i - 1].off, lv[i - 1].w, lv[i - 1].h,
                           dst + lv[i].off, lv[i].w, lv[i].h);
        }
    }
}

// The per-draw chain for a borrowed bitmap source, built into the canvas's
// reused scratch: a borrowed buffer has no identity to cache a pyramid
// against (a canvas2d_image does -- canvas2d_image_build_mips).  Returns the
// count built, or 0 when the scratch can't grow -- the caller falls back to
// bilinear, best-effort like the other OOM paths.
static int build_src_mips(struct canvas2d_context *__single cv,
                          uint8_t const *__counted_by(slen) src, int slen,
                          int sw, int sh, int need,
                          enum canvas2d_color_type ct, bool premul,
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
// A canvas2d_image is a thing you draw FROM; the canvas is the surface you draw
// TO; both are bitmaps (RGBA8 memory) underneath.  Reifying the pixels gives
// them identity, which is what lets derived data live with them:
// canvas2d_image_build_mips caches the premultiplied pyramid the bitmap entry
// points must otherwise rebuild per minifying draw.
struct canvas2d_image {
    int w, h;
    enum canvas2d_color_type ct;  // unorm8 or f16 channels...
    enum canvas2d_alpha_type at;  // ...straight or premultiplied -- all four
                                // format combinations are peers
    enum canvas2d_color_space cs; // the pixels' space; sampled in it, the resolved
                                // sample converts to the working space on deposit
    uint8_t *__counted_by(len) px;  // raw bytes, w * h * px_bpp(ct)
    int len;
    // The explicit mip chain (canvas2d_image_build_mips): fill_mips' layout in
    // one buffer, each level a self-slice in lv.  nlevels == 0 until built.
    uint8_t *__counted_by(mips_len) mips;
    int mips_len;
    canvas2d_mip lv[MIP_MAX_LEVELS];
    int nlevels;
};

// The shared constructor body: copy `len` bytes of pixels into a fresh image
// of the given format.  NULL on bad dims or OOM.
static struct canvas2d_image *__single image_make(
        uint8_t const *__counted_by(len) px, int len, int w, int h,
        enum canvas2d_color_type ct, enum canvas2d_alpha_type at,
        enum canvas2d_color_space cs) {
    if (!canvas2d_rgba8_dims_ok(w, h) || len != w * h * px_bpp(ct)) {
        return NULL;
    }
    struct canvas2d_image *img = calloc(1, sizeof *img);
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
    img->cs = cs;
    img->len = len;
    img->px = copy;
    return img;
}

struct canvas2d_image *__single canvas2d_image_unorm8(
        enum canvas2d_color_space space,
        uint8_t const *__counted_by(w * h * 4) px, int w, int h,
        enum canvas2d_alpha_type at) {
    if (!canvas2d_rgba8_dims_ok(w, h)) {
        return NULL;
    }
    return image_make(px, w * h * 4, w, h, CANVAS2D_COLOR_UNORM8, at, space);
}

struct canvas2d_image *__single canvas2d_image_f16(
        enum canvas2d_color_space space,
        _Float16 const *__counted_by(w * h * 4) px, int w, int h,
        enum canvas2d_alpha_type at) {
    if (!canvas2d_rgba8_dims_ok(w, h)) {
        return NULL;
    }
    int const len = w * h * 4 * (int)sizeof(_Float16);
    uint8_t const *bytes = (uint8_t const *)px;
    return image_make(bytes, len, w, h, CANVAS2D_COLOR_F16, at, space);
}

struct canvas2d_image *__single canvas2d_snapshot(struct canvas2d_context *__single cv) {
    int const w = cv->width, h = cv->height;
    if (!canvas2d_rgba8_dims_ok(w, h)) {
        return NULL;
    }
    // The surface is premultiplied f16 and so is the snapshot: one memcpy,
    // bit-lossless, no quantize, no unpremultiply.  The
    // snapshot's space IS the canvas's working space -- the tag for
    // pixels lifted straight off the surface.
    uint8_t const *bytes = (uint8_t const *)cv->target;
    return image_make(bytes, w * h * 8, w, h,
                      CANVAS2D_COLOR_F16, CANVAS2D_ALPHA_PREMUL, cv->space);
}

bool canvas2d_image_build_mips(struct canvas2d_image *__single img) {
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
              img->at == CANVAS2D_ALPHA_PREMUL, plan, n);
    img->mips_len = cap;
    img->mips = chain;
    for (int i = 0; i < n; i++) {
        img->lv[i] = (canvas2d_mip){ .px = img->mips + plan[i].off,
                                 .len = plan[i].w * plan[i].h * bpp,
                                 .w = plan[i].w, .h = plan[i].h };
    }
    img->nlevels = n;
    return true;
}

int canvas2d_image_width(struct canvas2d_image const *__single img) {
    return img->w;
}

int canvas2d_image_height(struct canvas2d_image const *__single img) {
    return img->h;
}

void canvas2d_image_free(struct canvas2d_image *__single img) {
    if (!img) {
        return;
    }
    free(img->px);
    free(img->mips);
    free(img);
}

void canvas2d_set_image_smoothing_enabled(struct canvas2d_context *__single cv, bool enabled) {
    if (cv->rec) { canvas2d_rec_floats_bool(cv->rec, "set_image_smoothing_enabled", NULL, 0, enabled); }
    cv->cur.image_smoothing_enabled = enabled;
}

void canvas2d_set_image_smoothing_quality(struct canvas2d_context *__single cv,
                                        enum canvas2d_image_smoothing_quality quality) {
    switch (quality) {
        case CANVAS2D_SMOOTHING_LOW:
        case CANVAS2D_SMOOTHING_MEDIUM:
        case CANVAS2D_SMOOTHING_HIGH:
            if (cv->rec) { canvas2d_rec_smoothing_quality(cv->rec, quality); }
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
// COLOUR-SPACE MODEL: an image/bitmap source carries a colour-space tag
// (src_space).  It is sampled -- mip box-halving and the bilinear/cubic taps --
// in its OWN tagged space; the resolved per-sample colour then converts to the
// canvas working space on deposit (sample_to_working, applied in the loop below;
// a no-op when the spaces match).  This is the design, not a shortcut: the
// image's format governs filtering, the canvas working space governs
// compositing.  Filtering an sRGB source therefore averages in sRGB even on a
// linear canvas -- to filter in linear, tag/store the source linear (f16).  Only
// the deposit crosses spaces.
static void draw_image_quad(struct canvas2d_context *__single cv,
                            uint8_t const *__counted_by(slen) src, int slen,
                            int sw, int sh, float sx, float sy,
                            float sww, float shh, float dx, float dy,
                            float dw, float dh, bool premul_src,
                            enum canvas2d_color_type src_ct,
                            enum canvas2d_color_space src_space,
                            bool quality_tiers, bool chain_on_demand,
                            struct canvas2d_image const *__single img,
                            canvas2d_mip hi, canvas2d_mip lo, float lt) {
    if (!canvas2d_rgba8_dims_ok(sw, sh) || slen < sw * sh * px_bpp(src_ct) ||
        dw <= 0.0f || dh <= 0.0f) {
        return;
    }
    // The dest rect transforms to a (possibly rotated) device-space quad.
    canvas2d_vec2 q[4] = { xf(cv, dx, dy), xf(cv, dx + dw, dy),
                       xf(cv, dx + dw, dy + dh), xf(cv, dx, dy + dh) };
    cbbox const b = points_bbox(cv, q, 4, filter_margin(cv));
    if (b.w <= 0 || b.h <= 0 || !canvas2d_ensure_tile(cv, b.w * b.h) ||
        !canvas2d_cover_reset(&cv->cover, b.w, b.h)) {
        return;
    }
    cover_edge(cv, b, q[0], q[1]);
    cover_edge(cv, b, q[1], q[2]);
    cover_edge(cv, b, q[2], q[3]);
    cover_edge(cv, b, q[3], q[0]);
    canvas2d_cover_resolve(&cv->cover, b.w, b.h, CANVAS2D_NONZERO, cv->cov);

    // Blocks of eight pixels, the paint_tile_pattern shape: the SAMPLING stays
    // scalar per lane -- data-dependent taps at arbitrary source coords --
    // with the fold + premultiply around it run as planes: the sampled colours
    // are born f32, sample alpha x global alpha folds there, narrows once,
    // and the coverage fold finishes in f16.  Zero-coverage lanes skip their
    // sample and fold to transparent black.  The premultiplied-data arm
    // (emoji captures, mip and cubic samples) has no premultiply: every
    // channel narrows and scales by ga x coverage in f16.
    canvas2d_mat const inv = canvas2d_mat_invert(cv->cur.ctm);  // device -> user
    bool const persp = !canvas2d_mat_is_affine(cv->cur.ctm);  // per-pixel divide?
    float const ga = cv->cur.global_alpha;
    f16x8 const gah = (f16x8)(_Float16)ga;
    bool const fold = shade_folds_coverage(cv);
    bool const smooth = cv->cur.image_smoothing_enabled;
    // Sampler tier.  imageSmoothingQuality is live for the quality_tiers
    // paths (bitmaps, reified images, replayed blocks): a minifying draw
    // (source footprint past one source px per device px) at medium/high
    // samples the premultiplied mip chain with trilinear filtering -- an
    // image's cached chain when built, the per-draw scratch rebuild when
    // chain_on_demand (bitmaps), plain bilinear otherwise (a mip-less image's
    // documented fallback: canvas2d_image_build_mips is the explicit opt-in) --
    // and a magnifying draw at high runs the 4x4 BC-spline (CUBIC_B/CUBIC_C).
    // The emoji-capture path arrives with its level pair already picked from
    // the text cache's pyramid (canvas2d_glyph_mip_pair, the same doubling rule)
    // and joins at SAMP_TRILINEAR; a zero blend there is just bilinear on
    // `src`, which IS the finer level.  The footprint is one Jacobian for the
    // whole quad (the CTM is affine), the emoji rule's max mapped axis.
    enum { SAMP_NEAREST, SAMP_BILINEAR, SAMP_TRILINEAR, SAMP_CUBIC } samp =
        smooth ? SAMP_BILINEAR : SAMP_NEAREST;
    if (samp == SAMP_BILINEAR && premul_src && lt > 0.0f && lo.px) {
        samp = SAMP_TRILINEAR;
    }
    if (samp == SAMP_BILINEAR && quality_tiers &&
        cv->cur.image_smoothing_quality != CANVAS2D_SMOOTHING_LOW) {
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
                    hi = (canvas2d_mip){ .px = cv->mips + la.off,
                                     .len = la.w * la.h * bpp,
                                     .w = la.w, .h = la.h };
                    lo = (canvas2d_mip){ .px = cv->mips + lb.off,
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
                   cv->cur.image_smoothing_quality == CANVAS2D_SMOOTHING_HIGH) {
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
    f32x8 const lane = { 0, 1, 2, 3, 4, 5, 6, 7 };
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
            f32x8 const xs = (float)b.x + ((float)px + lane) + 0.5f;
            foldv8 const u = persp ? mat_apply8_persp(inv, xs, devy)
                                   : mat_apply8(inv, xs, devy);
            f32x8 const fsx = sx + ((u.x - dx) / dw) * sww;
            f32x8 const fsy = sy + ((u.y - dy) / dh) * shh;
            f32x8 sr = (f32x8)0.0f, sg = (f32x8)0.0f, sb = (f32x8)0.0f,
                   sa = (f32x8)0.0f;
            for (int l = 0; l < k; l++) {
                if (cv->cov[i + l] == 0) {  // a zero-coverage lane skips its taps
                    continue;
                }
                float s[4];
                bool const f16 = src_ct == CANVAS2D_COLOR_F16;
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
                // Sampled in the source's space; convert the resolved sample to
                // the working space before it composites.  Skipped (bit-exact)
                // when the spaces match.  The transfer is nonlinear, so a
                // premultiplied sample unpremultiplies around the convert.
                if (src_space != cv->space) {
                    if (premul_data) {
                        float const a = s[3];
                        if (a > 0.0f) {
                            s[0] /= a;
                            s[1] /= a;
                            s[2] /= a;
                            sample_to_working(cv, src_space, s);
                            s[0] *= a;
                            s[1] *= a;
                            s[2] *= a;
                        }  // a == 0: every channel is already 0
                    } else {
                        sample_to_working(cv, src_space, s);
                    }
                }
                sr[l] = s[0];
                sg[l] = s[1];
                sb[l] = s[2];
                sa[l] = s[3];
            }
            f16x8 const covh = fold ? (k < 8 ? cover8_k(cv->cov + i, k)
                                             : cover8(cv->cov + i))
                                    : (f16x8)(_Float16)1.0f;  // unused when !fold
            canvas2d_px8 out;
            if (premul_data) {
                f16x8 const m = fold ? gah * covh : gah;
                out = (canvas2d_px8){ __builtin_convertvector(sr, f16x8) * m,
                                  __builtin_convertvector(sg, f16x8) * m,
                                  __builtin_convertvector(sb, f16x8) * m,
                                  __builtin_convertvector(sa, f16x8) * m };
            } else {
                f16x8 alpha = __builtin_convertvector(sa * ga, f16x8);
                if (fold) {
                    alpha = alpha * covh;
                }
                out = shade8(__builtin_convertvector(sr, f16x8),
                             __builtin_convertvector(sg, f16x8),
                             __builtin_convertvector(sb, f16x8),
                             alpha);
            }
            if (k < 8) {
                canvas2d_px8_store_k(cv->tile + i, k, out);
            } else {
                canvas2d_px8_store(cv->tile + i, out);
            }
        }
    }
    blend_tile(cv, b, fold);
}

void canvas2d_draw_bitmap_subrect(struct canvas2d_context *__single cv, enum canvas2d_color_space space,
                               uint8_t const *__counted_by(sw * sh * 4) src,
                               int sw, int sh, float sx, float sy,
                               float sww, float shh, float dx, float dy,
                               float dw, float dh) {
    if (!canvas2d_rgba8_dims_ok(sw, sh)) {
        return;
    }
    if (cv->rec) {
        // The source's dims and colour space ride the image block; the op
        // line carries the two user-space rects.  Suspended when
        // draw_image/draw_image_scaled is the op the caller actually issued
        // (they record as themselves).
        int const id = canvas2d_rec_image(cv->rec, src, sw * sh * 4, sw, sh,
                                      CANVAS2D_COLOR_UNORM8, CANVAS2D_ALPHA_UNPREMUL,
                                      space);
        if (id >= 0) {
            canvas2d_rec_image_mips(cv->rec, id);  // bitmap draws: chain on demand
            canvas2d_rec_image_floats(cv->rec, "draw_image_subrect", id,
                                  (float[]){ sx, sy, sww, shh, dx, dy, dw, dh },
                                  8);
        }
    }
    // Sampled in `space`; the resolved sample converts to the working space in
    // draw_image_quad (a no-op when they match).
    draw_image_quad(cv, src, sw * sh * 4, sw, sh, sx, sy, sww, shh,
                    dx, dy, dw, dh, false, CANVAS2D_COLOR_UNORM8, space, true, true,
                    NULL,
                    (canvas2d_mip){ .px = NULL, .len = 0, .w = 0, .h = 0 },
                    (canvas2d_mip){ .px = NULL, .len = 0, .w = 0, .h = 0 }, 0.0f);
}

void canvas2d_draw_bitmap(struct canvas2d_context *__single cv, enum canvas2d_color_space space,
                       uint8_t const *__counted_by(sw * sh * 4) src,
                       int sw, int sh, float dx, float dy) {
    // Record `draw_image` as itself, then swallow the subrect form it
    // delegates to.  canvas2d_rgba8_dims_ok gates the w*h*4 the block needs (the same
    // predicate the delegate applies before painting).
    if (cv->rec && canvas2d_rgba8_dims_ok(sw, sh)) {
        int const id = canvas2d_rec_image(cv->rec, src, sw * sh * 4, sw, sh,
                                      CANVAS2D_COLOR_UNORM8, CANVAS2D_ALPHA_UNPREMUL,
                                      space);
        if (id >= 0) {
            canvas2d_rec_image_mips(cv->rec, id);  // bitmap draws: chain on demand
            canvas2d_rec_image_floats(cv->rec, "draw_image", id,
                                  (float[]){ dx, dy }, 2);
        }
    }
    canvas2d_rec_enter(cv->rec);
    canvas2d_draw_bitmap_subrect(cv, space, src, sw, sh, 0.0f, 0.0f, (float)sw, (float)sh,
                              dx, dy, (float)sw, (float)sh);
    canvas2d_rec_leave(cv->rec);
}

void canvas2d_draw_bitmap_scaled(struct canvas2d_context *__single cv, enum canvas2d_color_space space,
                              uint8_t const *__counted_by(sw * sh * 4) src,
                              int sw, int sh, float dx, float dy,
                              float dw, float dh) {
    if (cv->rec && canvas2d_rgba8_dims_ok(sw, sh)) {
        int const id = canvas2d_rec_image(cv->rec, src, sw * sh * 4, sw, sh,
                                      CANVAS2D_COLOR_UNORM8, CANVAS2D_ALPHA_UNPREMUL,
                                      space);
        if (id >= 0) {
            canvas2d_rec_image_mips(cv->rec, id);  // bitmap draws: chain on demand
            canvas2d_rec_image_floats(cv->rec, "draw_image_scaled", id,
                                  (float[]){ dx, dy, dw, dh }, 4);
        }
    }
    canvas2d_rec_enter(cv->rec);
    canvas2d_draw_bitmap_subrect(cv, space, src, sw, sh, 0.0f, 0.0f, (float)sw, (float)sh,
                              dx, dy, dw, dh);
    canvas2d_rec_leave(cv->rec);
}

// The reified-image draw trio.  Each records the image's pixels as an image
// block naming its format (deduplicated -- the object is a natural key, but
// content-dedup already covers it) plus the same draw_image op lines the
// bitmap trio writes: the format speaks in images either way.  An
// `image_mips` line rides along only once the image's chain is built, so a
// mip-less image's bilinear-fallback draws replay faithfully.
static int rec_image_obj(struct canvas2d_context *__single cv,
                         struct canvas2d_image const *__single img) {
    int const id = canvas2d_rec_image(cv->rec, img->px, img->len, img->w, img->h,
                                  img->ct, img->at, img->cs);
    if (id >= 0 && img->nlevels > 0) {
        canvas2d_rec_image_mips(cv->rec, id);
    }
    return id;
}

void canvas2d_draw_image_subrect(struct canvas2d_context *__single cv,
                               struct canvas2d_image const *__single img,
                               float sx, float sy, float sww, float shh,
                               float dx, float dy, float dw, float dh) {
    if (cv->rec) {
        int const id = rec_image_obj(cv, img);
        if (id >= 0) {
            canvas2d_rec_image_floats(cv->rec, "draw_image_subrect", id,
                                  (float[]){ sx, sy, sww, shh, dx, dy, dw, dh },
                                  8);
        }
    }
    draw_image_quad(cv, img->px, img->len, img->w, img->h, sx, sy, sww, shh,
                    dx, dy, dw, dh, img->at == CANVAS2D_ALPHA_PREMUL, img->ct,
                    img->cs, true, false, img,
                    (canvas2d_mip){ .px = NULL, .len = 0, .w = 0, .h = 0 },
                    (canvas2d_mip){ .px = NULL, .len = 0, .w = 0, .h = 0 }, 0.0f);
}

void canvas2d_draw_image(struct canvas2d_context *__single cv,
                       struct canvas2d_image const *__single img,
                       float dx, float dy) {
    if (cv->rec) {
        int const id = rec_image_obj(cv, img);
        if (id >= 0) {
            canvas2d_rec_image_floats(cv->rec, "draw_image", id,
                                  (float[]){ dx, dy }, 2);
        }
    }
    canvas2d_rec_enter(cv->rec);
    canvas2d_draw_image_subrect(cv, img, 0.0f, 0.0f, (float)img->w,
                              (float)img->h, dx, dy, (float)img->w,
                              (float)img->h);
    canvas2d_rec_leave(cv->rec);
}

void canvas2d_draw_image_scaled(struct canvas2d_context *__single cv,
                              struct canvas2d_image const *__single img,
                              float dx, float dy, float dw, float dh) {
    if (cv->rec) {
        int const id = rec_image_obj(cv, img);
        if (id >= 0) {
            canvas2d_rec_image_floats(cv->rec, "draw_image_scaled", id,
                                  (float[]){ dx, dy, dw, dh }, 4);
        }
    }
    canvas2d_rec_enter(cv->rec);
    canvas2d_draw_image_subrect(cv, img, 0.0f, 0.0f, (float)img->w,
                              (float)img->h, dx, dy, dw, dh);
    canvas2d_rec_leave(cv->rec);
}

// Replay's draw of one image block (canvas2d_replay.h): ct/at are the block's
// format as named on its line, mips whether the block's draws carry
// mip-chain semantics (an `image_mips` line) -- per-draw rebuild here, byte-
// identical to a cached chain.  Re-records in the replayed op's own spelling
// (`form`) when replaying onto a recording canvas, so the round trip is
// byte-idempotent.
void canvas2d_canvas_draw_block(struct canvas2d_context *__single cv,
                            uint8_t const *__counted_by(slen) px, int slen,
                            int w, int h, enum canvas2d_color_type ct,
                            enum canvas2d_alpha_type at, enum canvas2d_color_space cs,
                            bool mips, int form,
                            float sx, float sy, float sww, float shh,
                            float dx, float dy, float dw, float dh) {
    if (!canvas2d_rgba8_dims_ok(w, h) || slen < w * h * px_bpp(ct)) {
        return;
    }
    if (cv->rec) {
        // The colour-space tag carries through to the re-recorded block, so a
        // replay-onto-recording round trip is byte-idempotent.
        int const id = canvas2d_rec_image(cv->rec, px, w * h * px_bpp(ct), w, h,
                                      ct, at, cs);
        if (id >= 0) {
            if (mips) {
                canvas2d_rec_image_mips(cv->rec, id);
            }
            switch (form) {
                case 0:
                    canvas2d_rec_image_floats(cv->rec, "draw_image", id,
                                          (float[]){ dx, dy }, 2);
                    break;
                case 1:
                    canvas2d_rec_image_floats(cv->rec, "draw_image_scaled", id,
                                          (float[]){ dx, dy, dw, dh }, 4);
                    break;
                default:
                    canvas2d_rec_image_floats(cv->rec, "draw_image_subrect", id,
                                          (float[]){ sx, sy, sww, shh,
                                                     dx, dy, dw, dh }, 8);
                    break;
            }
        }
    }
    draw_image_quad(cv, px, slen, w, h, sx, sy, sww, shh, dx, dy, dw, dh,
                    at == CANVAS2D_ALPHA_PREMUL, ct, cs, true, mips, NULL,
                    (canvas2d_mip){ .px = NULL, .len = 0, .w = 0, .h = 0 },
                    (canvas2d_mip){ .px = NULL, .len = 0, .w = 0, .h = 0 }, 0.0f);
}

// --- Path2D -----------------------------------------------------------------
//
// A Path2D records its commands in user space and replays them through the
// canvas's path methods (reusing all the curve/arc/rounding logic) into a fresh
// device-space path at draw time, so it honours the current transform without
// disturbing the canvas's own current path.  The command-list storage lives in
// canvas2d_path2d.h so the recorder can serialize a path as a `path` block.

struct canvas2d_path2d *__single canvas2d_path2d(void) {
    return calloc(1, sizeof(struct canvas2d_path2d));  // cmds=NULL, ncmds=cap=0 (consistent)
}

void canvas2d_path2d_free(struct canvas2d_path2d *__single p) {
    if (!p) {
        return;
    }
    free(p->cmds);
    free(p);
}

static void p2d_push(struct canvas2d_path2d *__single p, p2d_cmd c) {
    if (p->ncmds >= p->cap) {
        int const nc = canvas2d_grow_cap(p->cap, p->ncmds + 1);
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

void canvas2d_path2d_move_to(struct canvas2d_path2d *__single p, float x, float y) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_MOVE, .a = { x, y } });
}

void canvas2d_path2d_line_to(struct canvas2d_path2d *__single p, float x, float y) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_LINE, .a = { x, y } });
}

void canvas2d_path2d_quadratic_curve_to(struct canvas2d_path2d *__single p,
                                      float cpx, float cpy, float x, float y) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_QUAD, .a = { cpx, cpy, x, y } });
}

void canvas2d_path2d_bezier_curve_to(struct canvas2d_path2d *__single p, float c1x, float c1y,
                                   float c2x, float c2y, float x, float y) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_CUBIC, .a = { c1x, c1y, c2x, c2y, x, y } });
}

void canvas2d_path2d_arc(struct canvas2d_path2d *__single p, float x, float y, float radius,
                       float start_angle, float end_angle, bool anticlockwise) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_ARC,
                           .a = { x, y, radius, start_angle, end_angle },
                           .ccw = anticlockwise });
}

void canvas2d_path2d_ellipse(struct canvas2d_path2d *__single p, float x, float y,
                           float rx, float ry, float rotation,
                           float start_angle, float end_angle, bool anticlockwise) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_ELLIPSE,
                           .a = { x, y, rx, ry, rotation, start_angle, end_angle },
                           .ccw = anticlockwise });
}

void canvas2d_path2d_arc_to(struct canvas2d_path2d *__single p, float x1, float y1,
                          float x2, float y2, float radius) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_ARC_TO, .a = { x1, y1, x2, y2, radius } });
}

void canvas2d_path2d_rect(struct canvas2d_path2d *__single p, float x, float y,
                        float w, float h) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_RECT, .a = { x, y, w, h } });
}

void canvas2d_path2d_round_rect(struct canvas2d_path2d *__single p, float x, float y,
                              float w, float h, float radius) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_ROUND_RECT, .a = { x, y, w, h, radius } });
}

void canvas2d_path2d_close_path(struct canvas2d_path2d *__single p) {
    p2d_push(p, (p2d_cmd){ .verb = P2D_CLOSE });
}

void canvas2d_path2d_add_path(struct canvas2d_path2d *__single dst,
                            struct canvas2d_path2d const *__single src) {
    for (int i = 0; i < src->ncmds; i++) {
        p2d_push(dst, src->cmds[i]);
    }
}

// Replay a Path2D's commands into cv->path through the canvas path methods (which
// transform each coordinate by the current CTM and flatten curves at device tol).
static void p2d_replay(struct canvas2d_context *__single cv, struct canvas2d_path2d const *__single p) {
    for (int i = 0; i < p->ncmds; i++) {
        p2d_cmd const c = p->cmds[i];
        float const *a = c.a;
        switch (c.verb) {
            case P2D_MOVE:       canvas2d_move_to(cv, a[0], a[1]); break;
            case P2D_LINE:       canvas2d_line_to(cv, a[0], a[1]); break;
            case P2D_QUAD:       canvas2d_quadratic_curve_to(cv, a[0], a[1], a[2], a[3]); break;
            case P2D_CUBIC:      canvas2d_bezier_curve_to(cv, a[0], a[1], a[2], a[3],
                                                        a[4], a[5]); break;
            case P2D_ARC:        canvas2d_arc(cv, a[0], a[1], a[2], a[3], a[4], c.ccw); break;
            case P2D_ELLIPSE:    canvas2d_ellipse(cv, a[0], a[1], a[2], a[3], a[4],
                                                a[5], a[6], c.ccw); break;
            case P2D_ARC_TO:     canvas2d_arc_to(cv, a[0], a[1], a[2], a[3], a[4]); break;
            case P2D_RECT:       canvas2d_rect(cv, a[0], a[1], a[2], a[3]); break;
            case P2D_ROUND_RECT: canvas2d_round_rect(cv, a[0], a[1], a[2], a[3], a[4]);
                                 break;
            case P2D_CLOSE:      canvas2d_close_path(cv); break;
        }
    }
}

// Borrow the current-path machinery for a Path2D without disturbing it: swap_in
// copies the current path (and its user-space pen) aside and builds `p` into a
// fresh one in its place; swap_out frees the scratch and restores the original.
// Every Path2D draw and hit-test runs between the two.
struct p2d_scratch {
    struct canvas2d_path path;
    struct canvas2d_path upath;  // the user-space twin (perspective fill/stroke/clip)
    canvas2d_vec2 user;
};

static void p2d_swap_in(struct canvas2d_context *__single cv, struct canvas2d_path2d const *__single p,
                        struct p2d_scratch *__single sv) {
    sv->path = cv->path;
    sv->upath = cv->upath;
    sv->user = cv->cur_user;
    canvas2d_path_init(&cv->path);
    canvas2d_path_init(&cv->upath);
    p2d_replay(cv, p);
}

static void p2d_swap_out(struct canvas2d_context *__single cv, struct p2d_scratch *__single sv) {
    canvas2d_path_free(&cv->path);
    canvas2d_path_free(&cv->upath);
    cv->path = sv->path;
    cv->upath = sv->upath;
    cv->cur_user = sv->user;
}

void canvas2d_fill_path(struct canvas2d_context *__single cv, struct canvas2d_path2d const *__single p,
                      enum canvas2d_fill_rule rule) {
    // Record `fill_path <id> <rule>` against the path's numbered block, then
    // swallow the public path methods p2d_replay drives -- the file keeps the
    // op the caller issued, not the path's expansion into the current path.
    if (cv->rec) {
        int const id = canvas2d_rec_path(cv->rec, p);
        if (id >= 0) { canvas2d_rec_path_rule(cv->rec, "fill_path", id, rule); }
    }
    canvas2d_rec_enter(cv->rec);
    struct p2d_scratch sv;
    p2d_swap_in(cv, p, &sv);
    if (canvas2d_mat_is_affine(cv->cur.ctm)) {
        fill_device_path(cv, &cv->path, rule);
    } else {
        fill_device_path(cv, perspective_fill_path(cv, &cv->upath), rule);
    }
    p2d_swap_out(cv, &sv);
    canvas2d_rec_leave(cv->rec);
}

void canvas2d_stroke_path(struct canvas2d_context *__single cv, struct canvas2d_path2d const *__single p) {
    if (cv->rec) {
        int const id = canvas2d_rec_path(cv->rec, p);
        if (id >= 0) { canvas2d_rec_path_op(cv->rec, "stroke_path", id); }
    }
    canvas2d_rec_enter(cv->rec);
    struct p2d_scratch sv;
    p2d_swap_in(cv, p, &sv);
    if (canvas2d_mat_is_affine(cv->cur.ctm)) {
        stroke_device_path(cv, &cv->path);
    } else {
        stroke_perspective_path(cv, &cv->upath);
    }
    p2d_swap_out(cv, &sv);
    canvas2d_rec_leave(cv->rec);
}

void canvas2d_clip_path(struct canvas2d_context *__single cv, struct canvas2d_path2d const *__single p,
                      enum canvas2d_fill_rule rule) {
    if (cv->rec) {
        int const id = canvas2d_rec_path(cv->rec, p);
        if (id >= 0) { canvas2d_rec_path_rule(cv->rec, "clip_path", id, rule); }
    }
    // Swallow both p2d_replay's path methods and the nested canvas2d_clip.
    canvas2d_rec_enter(cv->rec);
    struct p2d_scratch sv;
    p2d_swap_in(cv, p, &sv);
    canvas2d_clip(cv, rule);
    p2d_swap_out(cv, &sv);
    canvas2d_rec_leave(cv->rec);
}

bool canvas2d_is_point_in_path2d(struct canvas2d_context *__single cv, struct canvas2d_path2d const *__single p,
                               float x, float y, enum canvas2d_fill_rule rule) {
    if (!isfinite(x) || !isfinite(y)) {
        return false;
    }
    canvas2d_rec_enter(cv->rec);  // a query: p2d_replay's path methods record nothing
    struct p2d_scratch sv;
    p2d_swap_in(cv, p, &sv);
    bool inside = path_contains(&cv->path, xf(cv, x, y), rule);
    p2d_swap_out(cv, &sv);
    canvas2d_rec_leave(cv->rec);
    return inside;
}

bool canvas2d_is_point_in_stroke_path(struct canvas2d_context *__single cv,
                                    struct canvas2d_path2d const *__single p,
                                    float x, float y) {
    if (!isfinite(x) || !isfinite(y)) {
        return false;
    }
    canvas2d_rec_enter(cv->rec);  // a query: p2d_replay's path methods record nothing
    struct p2d_scratch sv;
    p2d_swap_in(cv, p, &sv);
    bool inside = build_stroke_verts(cv, &cv->path) &&
                  stroke_verts_contain(cv, xf(cv, x, y));
    p2d_swap_out(cv, &sv);
    canvas2d_rec_leave(cv->rec);
    return inside;
}
