#include "canvas.h"

#include "compositor.h"
#include "cnvs_cover.h"
#include "cnvs_font.h"
#include "cnvs_geom.h"
#include "cnvs_gradient.h"
#include "cnvs_image.h"
#include "cnvs_math.h"
#include "cnvs_mem.h"
#include "cnvs_path.h"
#include "cnvs_png.h"
#include "cnvs_stroke.h"

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

struct canvas_state {
    cnvs_mat ctm;
    cnvs_unpremul fill;
    int fill_is_gradient;  // 0 = solid `fill`; 1 = `fill_grad`
    cnvs_gradient fill_grad;
    cnvs_unpremul stroke;
    int stroke_is_gradient;  // 0 = solid `stroke`; 1 = `stroke_grad`
    cnvs_gradient stroke_grad;
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
    bool image_smoothing_enabled;
    canvas_image_smoothing_quality image_smoothing_quality;
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
    cnvs_vec2 cur_user;  // current point in user space (path.cur is device space)
    cnvs_verts scratch_verts;  // stroke triangle output, fed to the coverage rasterizer
    cnvs_cover cover;
    uint8_t *__counted_by(cov_cap) cov;     // per-pixel coverage for the current op's bbox
    int cov_cap;
    cnvs_premul *__counted_by(tile_cap) tile;  // premultiplied tile for the current op's bbox
    int tile_cap;
    cnvs_unpremul ramp[CNVS_GRAD_RAMP_N];  // gradient colour ramp, rebuilt per gradient fill
    float *__counted_by(trow_cap) trow;    // one row of gradient parameters (vectorized solve)
    int trow_cap;
};

static cnvs_vec2 xf(canvas *__single cv, float x, float y);
static bool ensure_tile(canvas *__single cv, int npix);

// The initial drawing state (Canvas defaults): identity transform, opaque black
// fill/stroke, source-over, 1px miter strokes, no dash, 10px text, open clip.
// Shared by canvas_create and canvas_reset so the two can't drift.  Assigned
// field by field (not an init list): a compound literal of side-effecting calls
// has indeterminate evaluation order, which -fbounds-safety flags for a struct
// carrying a __counted_by member.  Clearing the gradient scratch isn't needed --
// fill_grad/stroke_grad are read only when the *_is_gradient flag is set.
static void state_defaults(struct canvas_state *s) {
    s->ctm = cnvs_mat_identity();
    s->fill = cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 1.0f);
    s->fill_is_gradient = 0;
    s->stroke = cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 1.0f);
    s->stroke_is_gradient = 0;
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
    s->image_smoothing_enabled = true;
    s->image_smoothing_quality = CANVAS_SMOOTHING_LOW;
    // Drop the clip: zero the count before NULLing the pointer so the
    // __counted_by(clip_len) invariant never sees NULL with a positive count.
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
    return cv;
}

void canvas_destroy(canvas *__single cv) {
    if (!cv) {
        return;
    }
    compositor_destroy(cv->comp);
    for (int i = 0; i < cv->stack_len; i++) {
        free(cv->stack[i].clip_mask);
    }
    free(cv->stack);
    free(cv->cur.clip_mask);
    cnvs_font_destroy(cv->font);
    cnvs_path_free(&cv->path);
    cnvs_path_free(&cv->text_path);
    cnvs_verts_free(&cv->scratch_verts);
    cnvs_cover_free(&cv->cover);
    free(cv->cov);
    free(cv->tile);
    free(cv->trow);
    free(cv);
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
    cv->stack_len += 1;
}

void canvas_restore(canvas *__single cv) {
    if (cv->stack_len > 0) {
        cv->stack_len -= 1;
        free(cv->cur.clip_mask);
        cv->cur = cv->stack[cv->stack_len];  // adopts the saved clip mask
        compositor_set_clip(cv->comp, cv->cur.clip_mask, cv->cur.clip_len);
    }
}

void canvas_reset(canvas *__single cv) {
    // Empty the saved-state stack (each entry may own a clip-mask copy); keep the
    // backing allocation for reuse.
    for (int i = 0; i < cv->stack_len; i++) {
        free(cv->stack[i].clip_mask);
    }
    cv->stack_len = 0;
    // Drop the current clip mask and restore every state field to its default.
    free(cv->cur.clip_mask);
    state_defaults(&cv->cur);
    // Discard the current path.
    cnvs_path_reset(&cv->path);
    cv->cur_user = (cnvs_vec2){ .x = 0.0f, .y = 0.0f };
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
    // reset() drops the (now wrong-sized) clip masks and saved stack, restores the
    // default state, and clears the fresh bitmap to transparent black.
    canvas_reset(cv);
    return true;
}

void canvas_translate(canvas *__single cv, float tx, float ty) {
    cv->cur.ctm = cnvs_mat_mul(cv->cur.ctm, cnvs_mat_translate(tx, ty));
}

void canvas_scale(canvas *__single cv, float sx, float sy) {
    cv->cur.ctm = cnvs_mat_mul(cv->cur.ctm, cnvs_mat_scale(sx, sy));
}

void canvas_rotate(canvas *__single cv, float radians) {
    cv->cur.ctm = cnvs_mat_mul(cv->cur.ctm, cnvs_mat_rotate(radians));
}

void canvas_transform(canvas *__single cv,
                      float a, float b, float c, float d, float e, float f) {
    cnvs_mat m = { .a = a, .b = b, .c = c, .d = d, .e = e, .f = f };
    cv->cur.ctm = cnvs_mat_mul(cv->cur.ctm, m);
}

void canvas_set_transform(canvas *__single cv,
                          float a, float b, float c, float d, float e, float f) {
    cv->cur.ctm = (cnvs_mat){ .a = a, .b = b, .c = c, .d = d, .e = e, .f = f };
}

void canvas_reset_transform(canvas *__single cv) {
    cv->cur.ctm = cnvs_mat_identity();
}

canvas_matrix canvas_get_transform(canvas *__single cv) {
    cnvs_mat m = cv->cur.ctm;
    return (canvas_matrix){ .a = m.a, .b = m.b, .c = m.c,
                            .d = m.d, .e = m.e, .f = m.f };
}

void canvas_set_fill_rgba(canvas *__single cv, float r, float g, float b, float a) {
    cv->cur.fill = cnvs_unpremul_of(clamp01(r), clamp01(g), clamp01(b), clamp01(a));
    cv->cur.fill_is_gradient = 0;
}

// Average CTM scale, used to bake user-space radii into device space.
static float ctm_scale(cnvs_mat m) {
    float det = m.a * m.d - m.b * m.c;
    return sqrtf(fabsf(det));
}

// Initialise a gradient struct in device space (the CTM is baked in now); the
// caller flips the matching is_gradient flag.
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

void canvas_set_fill_linear_gradient(canvas *__single cv,
                                     float x0, float y0, float x1, float y1) {
    grad_set_linear(cv, &cv->cur.fill_grad, x0, y0, x1, y1);
    cv->cur.fill_is_gradient = 1;
}

void canvas_set_fill_radial_gradient(canvas *__single cv, float x0, float y0,
                                     float r0, float x1, float y1, float r1) {
    grad_set_radial(cv, &cv->cur.fill_grad, x0, y0, r0, x1, y1, r1);
    cv->cur.fill_is_gradient = 1;
}

void canvas_set_fill_conic_gradient(canvas *__single cv, float start_angle,
                                    float x, float y) {
    grad_set_conic(cv, &cv->cur.fill_grad, start_angle, x, y);
    cv->cur.fill_is_gradient = 1;
}

void canvas_add_fill_color_stop(canvas *__single cv, float offset,
                                float r, float g, float b, float a) {
    cnvs_gradient_add_stop(&cv->cur.fill_grad, clamp01(offset),
                           cnvs_unpremul_of(clamp01(r), clamp01(g), clamp01(b), clamp01(a)));
}

void canvas_set_stroke_linear_gradient(canvas *__single cv,
                                       float x0, float y0, float x1, float y1) {
    grad_set_linear(cv, &cv->cur.stroke_grad, x0, y0, x1, y1);
    cv->cur.stroke_is_gradient = 1;
}

void canvas_set_stroke_radial_gradient(canvas *__single cv, float x0, float y0,
                                       float r0, float x1, float y1, float r1) {
    grad_set_radial(cv, &cv->cur.stroke_grad, x0, y0, r0, x1, y1, r1);
    cv->cur.stroke_is_gradient = 1;
}

void canvas_set_stroke_conic_gradient(canvas *__single cv, float start_angle,
                                      float x, float y) {
    grad_set_conic(cv, &cv->cur.stroke_grad, start_angle, x, y);
    cv->cur.stroke_is_gradient = 1;
}

void canvas_add_stroke_color_stop(canvas *__single cv, float offset,
                                  float r, float g, float b, float a) {
    cnvs_gradient_add_stop(&cv->cur.stroke_grad, clamp01(offset),
                           cnvs_unpremul_of(clamp01(r), clamp01(g), clamp01(b), clamp01(a)));
}

void canvas_set_global_alpha(canvas *__single cv, float alpha) {
    cv->cur.global_alpha = clamp01(alpha);
}

// canvas_composite_op mirrors compositor_blend_mode value-for-value (canvas.h
// notes the coupling), so the validated cast is the whole mapping.
void canvas_set_global_composite_operation(canvas *__single cv,
                                           canvas_composite_op op) {
    if ((int)op < 0 || (int)op >= COMPOSITOR_MODE_COUNT) {
        return;
    }
    cv->cur.composite = (compositor_blend_mode)op;
}

static cnvs_vec2 xf(canvas *__single cv, float x, float y) {
    return cnvs_mat_apply(cv->cur.ctm, (cnvs_vec2){ .x = x, .y = y });
}

// Integer device-space bounding box of a point set, clamped to the canvas.
typedef struct {
    int x, y, w, h;
} cbbox;

static cbbox points_bbox(canvas *__single cv,
                         cnvs_vec2 const *__counted_by(n) pts, int n) {
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
    float fx0 = floorf(minx), fy0 = floorf(miny);
    float fx1 = ceilf(maxx), fy1 = ceilf(maxy);
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

// Row buffer of gradient parameters, one float per column of the current bbox.
static bool ensure_trow(canvas *__single cv, int w) {
    if (w > cv->trow_cap) {
        float *nr = realloc(cv->trow, (size_t)w * sizeof *nr);
        if (!nr) {
            return false;
        }
        cv->trow = nr;
        cv->trow_cap = w;
    }
    return true;
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

// Build an RGBA16F tile from the coverage in cv->cov and the given paint, then
// composite it.  Each pixel's alpha is paint_alpha * global_alpha * coverage.
static void paint_tile(canvas *__single cv, cbbox b, int is_grad,
                       cnvs_gradient const *gr, cnvs_unpremul solid) {
    float ga = cv->cur.global_alpha;
    // A gradient over a large enough area amortizes a precomputed colour ramp: the
    // ramp costs CNVS_GRAD_RAMP_N stop evaluations to build, so below that many
    // pixels the per-pixel stop scan (cnvs_gradient_color_at) is cheaper.
    bool use_ramp = is_grad && (long)b.w * (long)b.h >= CNVS_GRAD_RAMP_N;
    if (use_ramp) {
        cnvs_gradient_build_ramp(gr, cv->ramp, CNVS_GRAD_RAMP_N);
    }
    // Solve the gradient parameter a row at a time (vectorized); fall back to the
    // scalar per-pixel solve only if the tiny row buffer can't be grown.
    bool use_row = is_grad && ensure_trow(cv, b.w);
    for (int py = 0; py < b.h; py++) {
        if (use_row) {
            cnvs_gradient_param_row(gr, b.x, (float)b.y + (float)py + 0.5f, b.w,
                                    cv->trow);
        }
        for (int px = 0; px < b.w; px++) {
            int i = py * b.w + px;
            float covf = (float)cv->cov[i] / 255.0f;
            cnvs_unpremul col;
            if (is_grad) {
                float t;
                bool inside;
                if (use_row) {
                    t = cv->trow[px];        // -1 marks "outside the gradient"
                    inside = t >= 0.0f;
                } else {
                    cnvs_vec2 p = { .x = (float)b.x + (float)px + 0.5f,
                                    .y = (float)b.y + (float)py + 0.5f };
                    inside = cnvs_gradient_param(gr, p, &t);
                }
                if (inside) {
                    // Nearest ramp entry (t is clamped to [0,1], so the index is in
                    // range); see CNVS_GRAD_RAMP_N for why not interpolated.
                    col = use_ramp
                        ? cv->ramp[(int)(t * (float)(CNVS_GRAD_RAMP_N - 1) + 0.5f)]
                        : cnvs_gradient_color_at(gr, t);
                } else {
                    col = cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 0.0f);
                }
            } else {
                col = solid;
            }
            // Fold coverage and global alpha into the paint's alpha, then
            // premultiply -- the tile stores premultiplied pixels.
            float alpha = (float)col.a * ga * covf;
            cv->tile[i] = cnvs_premultiply((cnvs_unpremul){
                .r = col.r, .g = col.g, .b = col.b, .a = (_Float16)alpha });
        }
    }
    compositor_blend(cv->comp, b.x, b.y, b.w, b.h, cv->tile, cv->cur.composite);
}

void canvas_clear_rect(canvas *__single cv, float x, float y, float w, float h) {
    cnvs_vec2 q[4] = { xf(cv, x, y), xf(cv, x + w, y),
                       xf(cv, x + w, y + h), xf(cv, x, y + h) };
    cbbox b = points_bbox(cv, q, 4);
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
    cnvs_vec2 q[4] = { xf(cv, x, y), xf(cv, x + w, y),
                       xf(cv, x + w, y + h), xf(cv, x, y + h) };
    cbbox b = points_bbox(cv, q, 4);
    if (b.w <= 0 || b.h <= 0 || !ensure_tile(cv, b.w * b.h) ||
        !cnvs_cover_reset(&cv->cover, b.w, b.h)) {
        return;
    }
    cover_edge(cv, b, q[0], q[1]);
    cover_edge(cv, b, q[1], q[2]);
    cover_edge(cv, b, q[2], q[3]);
    cover_edge(cv, b, q[3], q[0]);
    cnvs_cover_resolve(&cv->cover, b.w, b.h, CNVS_NONZERO, cv->cov);
    paint_tile(cv, b, cv->cur.fill_is_gradient, &cv->cur.fill_grad, cv->cur.fill);
}

void canvas_begin_path(canvas *__single cv) {
    cnvs_path_reset(&cv->path);
}

void canvas_move_to(canvas *__single cv, float x, float y) {
    cnvs_path_move_to(&cv->path, xf(cv, x, y));
    cv->cur_user = (cnvs_vec2){ .x = x, .y = y };
}

void canvas_line_to(canvas *__single cv, float x, float y) {
    cnvs_path_line_to(&cv->path, xf(cv, x, y));
    cv->cur_user = (cnvs_vec2){ .x = x, .y = y };
}

void canvas_rect(canvas *__single cv, float x, float y, float w, float h) {
    cnvs_path_rect(&cv->path, xf(cv, x, y), xf(cv, x + w, y),
                   xf(cv, x + w, y + h), xf(cv, x, y + h));
    cv->cur_user = (cnvs_vec2){ .x = x, .y = y };
}

void canvas_quadratic_curve_to(canvas *__single cv,
                               float cpx, float cpy, float x, float y) {
    cnvs_path_quad_to(&cv->path, xf(cv, cpx, cpy), xf(cv, x, y),
                      CANVAS_FLATTEN_TOL);
    cv->cur_user = (cnvs_vec2){ .x = x, .y = y };
}

void canvas_bezier_curve_to(canvas *__single cv, float c1x, float c1y,
                            float c2x, float c2y, float x, float y) {
    cnvs_path_cubic_to(&cv->path, xf(cv, c1x, c1y), xf(cv, c2x, c2y),
                       xf(cv, x, y), CANVAS_FLATTEN_TOL);
    cv->cur_user = (cnvs_vec2){ .x = x, .y = y };
}

void canvas_ellipse(canvas *__single cv, float x, float y, float rx, float ry,
                    float rotation, float start_angle, float end_angle,
                    bool anticlockwise) {
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
    canvas_ellipse(cv, x, y, radius, radius, 0.0f, start_angle, end_angle,
                   anticlockwise);
}

void canvas_round_rect(canvas *__single cv, float x, float y, float w, float h,
                       float radius) {
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

void canvas_round_rect_radii(canvas *__single cv, float x, float y,
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

void canvas_arc_to(canvas *__single cv, float x1, float y1, float x2, float y2,
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

void canvas_close_path(canvas *__single cv) {
    cnvs_path_close(&cv->path);
}

void canvas_set_fill_rule(canvas *__single cv, canvas_fill_rule rule) {
    switch (rule) {
        case CANVAS_NONZERO: cv->cur.fill_rule = CNVS_NONZERO; break;
        case CANVAS_EVENODD: cv->cur.fill_rule = CNVS_EVENODD; break;
    }
}

// Rasterize a device-space path under `rule` and paint it over its clamped bbox.
static void fill_device_path(canvas *__single cv, cnvs_path const *p,
                             cnvs_fill_rule rule, int is_grad,
                             cnvs_gradient const *gr, cnvs_unpremul solid) {
    cbbox b = points_bbox(cv, p->pts, p->pt_len);
    if (b.w <= 0 || b.h <= 0 || !ensure_tile(cv, b.w * b.h) ||
        !cnvs_cover_reset(&cv->cover, b.w, b.h)) {
        return;
    }
    cover_path_edges(cv, b, p);
    cnvs_cover_resolve(&cv->cover, b.w, b.h, rule, cv->cov);
    paint_tile(cv, b, is_grad, gr, solid);
}

void canvas_fill(canvas *__single cv) {
    fill_device_path(cv, &cv->path, cv->cur.fill_rule, cv->cur.fill_is_gradient,
                     &cv->cur.fill_grad, cv->cur.fill);
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
    int n = cv->width * cv->height;
    uint8_t *nm = malloc((size_t)n);
    if (!nm) {
        return;
    }
    // Rasterize the path's coverage into cv->cov over its (clamped) bbox.
    cbbox b = points_bbox(cv, cv->path.pts, cv->path.pt_len);
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
    cv->cur.stroke = cnvs_unpremul_of(clamp01(r), clamp01(g), clamp01(b), clamp01(a));
    cv->cur.stroke_is_gradient = 0;
}

void canvas_set_line_width(canvas *__single cv, float width) {
    cv->cur.line_width = width;
}

void canvas_set_line_join(canvas *__single cv, canvas_line_join join) {
    switch (join) {
        case CANVAS_JOIN_MITER: cv->cur.line_join = CNVS_JOIN_MITER; break;
        case CANVAS_JOIN_ROUND: cv->cur.line_join = CNVS_JOIN_ROUND; break;
        case CANVAS_JOIN_BEVEL: cv->cur.line_join = CNVS_JOIN_BEVEL; break;
    }
}

void canvas_set_line_cap(canvas *__single cv, canvas_line_cap cap) {
    switch (cap) {
        case CANVAS_CAP_BUTT:   cv->cur.line_cap = CNVS_CAP_BUTT;   break;
        case CANVAS_CAP_ROUND:  cv->cur.line_cap = CNVS_CAP_ROUND;  break;
        case CANVAS_CAP_SQUARE: cv->cur.line_cap = CNVS_CAP_SQUARE; break;
    }
}

void canvas_set_miter_limit(canvas *__single cv, float limit) {
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
    cbbox b = points_bbox(cv, cv->scratch_verts.data, cv->scratch_verts.len);
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
    paint_tile(cv, b, cv->cur.stroke_is_gradient, &cv->cur.stroke_grad, cv->cur.stroke);
}

void canvas_stroke(canvas *__single cv) {
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

bool canvas_is_point_in_stroke(canvas *__single cv, float x, float y) {
    if (!isfinite(x) || !isfinite(y)) {
        return false;
    }
    // Build the same stroke triangles canvas_stroke would paint, then test the
    // (transformed) query point against their union -- inside any triangle hits.
    if (!build_stroke_verts(cv, &cv->path)) {
        return false;
    }
    cnvs_vec2 q = xf(cv, x, y);
    for (int i = 0; i + 2 < cv->scratch_verts.len; i += 3) {
        if (point_in_tri(q, cv->scratch_verts.data[i], cv->scratch_verts.data[i + 1],
                         cv->scratch_verts.data[i + 2])) {
            return true;
        }
    }
    return false;
}

void canvas_stroke_rect(canvas *__single cv, float x, float y, float w, float h) {
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

// Rebuild the cached font when the requested size changes; NULL on failure.
static cnvs_font *__single ensure_font(canvas *__single cv) {
    if (!cv->font || fabsf(cv->font_built_size - cv->cur.font_size) > 1e-6f) {
        cnvs_font_destroy(cv->font);
        cv->font = cnvs_font_create("Libian TC", cv->cur.font_size);
        cv->font_built_size = cv->cur.font_size;
    }
    return cv->font;
}

void canvas_set_font_size(canvas *__single cv, float px) {
    cv->cur.font_size = px > 0.0f ? px : 0.0f;
}

void canvas_set_text_align(canvas *__single cv, canvas_text_align align) {
    switch (align) {
        case CANVAS_ALIGN_START:
        case CANVAS_ALIGN_END:
        case CANVAS_ALIGN_LEFT:
        case CANVAS_ALIGN_RIGHT:
        case CANVAS_ALIGN_CENTER:
            cv->cur.text_align = align;
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
            cv->cur.text_baseline = baseline;
            break;
    }
}

// Resolve the pen origin (alphabetic baseline, in user space) for fill_text /
// stroke_text under the current textAlign and textBaseline.  textAlign shifts x
// by a fraction of the advance width (only measured when not left/start);
// textBaseline shifts y by an offset derived from the font's ascent/descent.
static void text_origin(canvas *__single cv, cnvs_font *__single f,
                        char const *__null_terminated text,
                        float x, float y, float *__single ox, float *__single oy) {
    *ox = x;
    if (cv->cur.text_align != CANVAS_ALIGN_START &&
        cv->cur.text_align != CANVAS_ALIGN_LEFT) {
        float frac = cv->cur.text_align == CANVAS_ALIGN_CENTER ? 0.5f : 1.0f;
        *ox = x - frac * cnvs_font_advance(f, text);
    }
    *oy = y;
    if (cv->cur.text_baseline != CANVAS_BASELINE_ALPHABETIC) {
        float a = 0.0f, d = 0.0f;
        cnvs_font_vmetrics(f, &a, &d);
        float dy = 0.0f;
        switch (cv->cur.text_baseline) {
            case CANVAS_BASELINE_ALPHABETIC:  break;  // handled by the guard
            case CANVAS_BASELINE_TOP:         dy = a; break;
            case CANVAS_BASELINE_HANGING:     dy = a; break;  // no BASE table: ~top
            case CANVAS_BASELINE_MIDDLE:      dy = (a - d) * 0.5f; break;
            case CANVAS_BASELINE_IDEOGRAPHIC: dy = -d; break;
            case CANVAS_BASELINE_BOTTOM:      dy = -d; break;
        }
        *oy = y + dy;
    }
}

float canvas_measure_text(canvas *__single cv, char const *__null_terminated text) {
    cnvs_font *__single f = ensure_font(cv);
    return f ? cnvs_font_advance(f, text) : 0.0f;
}

canvas_text_metrics canvas_measure_text_full(canvas *__single cv,
                                             char const *__null_terminated text) {
    canvas_text_metrics m;
    memset(&m, 0, sizeof m);  // all-zero if the font can't be built
    cnvs_font *__single f = ensure_font(cv);
    if (f) {
        cnvs_text_metrics tm;
        cnvs_font_measure(f, text, &tm);
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

void canvas_fill_text(canvas *__single cv, char const *__null_terminated text,
                      float x, float y) {
    cnvs_font *__single f = ensure_font(cv);
    if (!f) {
        return;
    }
    float ox, oy;
    text_origin(cv, f, text, x, y, &ox, &oy);
    cnvs_path_reset(&cv->text_path);
    cnvs_font_outline(f, text, ox, oy, cv->cur.ctm, CANVAS_FLATTEN_TOL, &cv->text_path);
    // Glyph outlines use nonzero winding (overlapping contours fill solid).
    fill_device_path(cv, &cv->text_path, CNVS_NONZERO, cv->cur.fill_is_gradient,
                     &cv->cur.fill_grad, cv->cur.fill);
}

void canvas_stroke_text(canvas *__single cv, char const *__null_terminated text,
                        float x, float y) {
    cnvs_font *__single f = ensure_font(cv);
    if (!f) {
        return;
    }
    float ox, oy;
    text_origin(cv, f, text, x, y, &ox, &oy);
    cnvs_path_reset(&cv->text_path);
    cnvs_font_outline(f, text, ox, oy, cv->cur.ctm, CANVAS_FLATTEN_TOL, &cv->text_path);
    stroke_device_path(cv, &cv->text_path);
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
    int x1 = x0 + 1, y1 = y0 + 1;
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
    cv->cur.image_smoothing_enabled = enabled;
}

void canvas_set_image_smoothing_quality(canvas *__single cv,
                                        canvas_image_smoothing_quality quality) {
    switch (quality) {
        case CANVAS_SMOOTHING_LOW:
        case CANVAS_SMOOTHING_MEDIUM:
        case CANVAS_SMOOTHING_HIGH:
            cv->cur.image_smoothing_quality = quality;
            break;
    }
}

void canvas_draw_image_subrect(canvas *__single cv,
                               uint8_t const *__counted_by(sw * sh * 4) src,
                               int sw, int sh, float sx, float sy,
                               float sww, float shh, float dx, float dy,
                               float dw, float dh) {
    if (!rgba8_dims_ok(sw, sh) || dw <= 0.0f || dh <= 0.0f) {
        return;
    }
    // The dest rect transforms to a (possibly rotated) device-space quad.
    cnvs_vec2 q[4] = { xf(cv, dx, dy), xf(cv, dx + dw, dy),
                       xf(cv, dx + dw, dy + dh), xf(cv, dx, dy + dh) };
    cbbox b = points_bbox(cv, q, 4);
    if (b.w <= 0 || b.h <= 0 || !ensure_tile(cv, b.w * b.h) ||
        !cnvs_cover_reset(&cv->cover, b.w, b.h)) {
        return;
    }
    cover_edge(cv, b, q[0], q[1]);
    cover_edge(cv, b, q[1], q[2]);
    cover_edge(cv, b, q[2], q[3]);
    cover_edge(cv, b, q[3], q[0]);
    cnvs_cover_resolve(&cv->cover, b.w, b.h, CNVS_NONZERO, cv->cov);

    cnvs_mat inv = cnvs_mat_invert(cv->cur.ctm);  // device -> user
    float ga = cv->cur.global_alpha;
    bool smooth = cv->cur.image_smoothing_enabled;
    for (int py = 0; py < b.h; py++) {
        for (int px = 0; px < b.w; px++) {
            int i = py * b.w + px;
            float covf = (float)cv->cov[i] / 255.0f;
            if (covf <= 0.0f) {
                cv->tile[i] = (cnvs_premul){ .r = 0, .g = 0, .b = 0, .a = 0 };
                continue;
            }
            // Device pixel centre -> user space -> dest-rect uv -> source coords.
            cnvs_vec2 u = cnvs_mat_apply(
                inv, (cnvs_vec2){ .x = (float)b.x + (float)px + 0.5f,
                                  .y = (float)b.y + (float)py + 0.5f });
            float fsx = sx + ((u.x - dx) / dw) * sww;
            float fsy = sy + ((u.y - dy) / dh) * shh;
            float s[4];
            if (smooth) {
                sample_src(src, sw * sh * 4, sw, sh, fsx, fsy, s);
            } else {
                sample_src_nearest(src, sw * sh * 4, sw, sh, fsx, fsy, s);
            }
            float alpha = s[3] * ga * covf;
            cv->tile[i] = cnvs_premultiply(cnvs_unpremul_of(s[0], s[1], s[2], alpha));
        }
    }
    compositor_blend(cv->comp, b.x, b.y, b.w, b.h, cv->tile, cv->cur.composite);
}

void canvas_draw_image(canvas *__single cv,
                       uint8_t const *__counted_by(sw * sh * 4) src,
                       int sw, int sh, float dx, float dy) {
    canvas_draw_image_subrect(cv, src, sw, sh, 0.0f, 0.0f, (float)sw, (float)sh,
                              dx, dy, (float)sw, (float)sh);
}

void canvas_draw_image_scaled(canvas *__single cv,
                              uint8_t const *__counted_by(sw * sh * 4) src,
                              int sw, int sh, float dx, float dy,
                              float dw, float dh) {
    canvas_draw_image_subrect(cv, src, sw, sh, 0.0f, 0.0f, (float)sw, (float)sh,
                              dx, dy, dw, dh);
}

// One pixel's four channels as a vector: a premultiplied cnvs_premul is four
// contiguous _Float16, so it loads straight into an h4.
typedef _Float16 h4 __attribute__((ext_vector_type(4)));
typedef float f4 __attribute__((ext_vector_type(4)));
typedef uint8_t u8x4 __attribute__((ext_vector_type(4)));

// Read the canvas back as unpremultiplied RGBA8: the compositor returns
// premultiplied pixels, and the un-premultiply and 8-bit quantize happen here.
// Vectorized per pixel across the four channels -- one vector divide instead of
// three scalar divides, branchless clamp+convert instead of four cnvs_f2u8 calls.
// Bit-identical to the scalar path: a true per-lane divide (not a reciprocal), and
// the same clamp-then-narrow-to-_Float16 round that cnvs_unpremultiply's clamp16 does.
static void read_unpremul(canvas *__single cv, uint8_t *__counted_by(len) out, int len) {
    if (len < cv->width * cv->height * 4) {
        return;
    }
    int const n = cv->width * cv->height;
    cnvs_premul *__counted_by(n) buf = malloc((size_t)n * sizeof *buf);
    if (!buf) {
        return;
    }
    compositor_read(cv->comp, buf, n);
    for (int i = 0; i < n; i++) {
        h4 ph;
        memcpy(&ph, &buf[i], sizeof ph);          // load (r,g,b,a) _Float16
        f4 p = __builtin_convertvector(ph, f4);
        float a = p[3];
        u8x4 b;
        if (a <= 0.0f) {  // fully transparent un-premultiplies to all zero
            b = (u8x4){ 0, 0, 0, 0 };
        } else {
            f4 u = p / (f4)a;  // r/a, g/a, b/a (lane 3 = a/a, overwritten next)
            u[3] = a;
            u = __builtin_elementwise_min((f4)1.0f,
                                          __builtin_elementwise_max((f4)0.0f, u));
            u = __builtin_convertvector(__builtin_convertvector(u, h4), f4);  // clamp16 round
            f4 v = u * 255.0f + 0.5f;  // in [0.5, 255.5]; truncating convert rounds
            b = __builtin_convertvector(v, u8x4);
        }
        memcpy(out + i * 4, &b, sizeof b);
    }
    free(buf);
}

void canvas_read_rgba(canvas *__single cv, uint8_t *__counted_by(len) out, int len) {
    read_unpremul(cv, out, len);
}

bool canvas_write_png(canvas *__single cv, char const *__null_terminated path) {
    int const len = cv->width * cv->height * 4;
    uint8_t *__counted_by(len) out = malloc((size_t)len);
    if (!out) {
        return false;
    }
    read_unpremul(cv, out, len);
    bool ok = cnvs_png_write(path, out, cv->width, cv->height);
    free(out);
    return ok;
}

void canvas_get_image_data(canvas *__single cv, int x, int y, int w, int h,
                           uint8_t *__counted_by(len) out, int len) {
    if (!rgba8_dims_ok(w, h) || len < w * h * 4) {
        return;
    }
    memset(out, 0, (size_t)len);  // pixels outside the canvas stay transparent
    int const clen = cv->width * cv->height * 4;
    uint8_t *__counted_by(clen) buf = malloc((size_t)clen);
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
    int col0 = (int)(cx0 - dx);
    int row0 = (int)(cy0 - dy);
    for (int py = 0; py < rh; py++) {
        for (int px = 0; px < rw; px++) {
            int si = ((row0 + py) * w + (col0 + px)) * 4;
            cv->tile[py * rw + px] = cnvs_premultiply(cnvs_unpremul_of(
                (float)data[si] / 255.0f, (float)data[si + 1] / 255.0f,
                (float)data[si + 2] / 255.0f, (float)data[si + 3] / 255.0f));
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
