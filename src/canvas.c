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

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Maximum chord deviation (device pixels) when flattening curves.
#define CANVAS_FLATTEN_TOL 0.25f
#define CANVAS_MAX_DASH 16

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
};

static cnvs_vec2 xf(canvas *__single cv, float x, float y);

canvas *__single canvas_create(int width, int height) {
    if (width <= 0 || height <= 0) {
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
    cv->cur.ctm = cnvs_mat_identity();
    cv->cur.fill = cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 1.0f);
    cv->cur.fill_is_gradient = 0;
    cv->cur.stroke = cnvs_unpremul_of(0.0f, 0.0f, 0.0f, 1.0f);
    cv->cur.stroke_is_gradient = 0;
    cv->cur.global_alpha = 1.0f;
    cv->cur.composite = COMPOSITOR_SRC_OVER;
    cv->cur.line_width = 1.0f;
    cv->cur.fill_rule = CNVS_NONZERO;
    cv->cur.line_join = CNVS_JOIN_MITER;
    cv->cur.line_cap = CNVS_CAP_BUTT;
    cv->cur.miter_limit = 10.0f;
    cv->cur.dash_count = 0;
    cv->cur.dash_offset = 0.0f;
    cv->cur.font_size = 10.0f;
    cv->cur.clip_mask = NULL;
    cv->cur.clip_len = 0;
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
    free(cv);
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
    cv->stack = ns;          // pointer and its count updated together
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

void canvas_set_fill_rgba(canvas *__single cv, float r, float g, float b, float a) {
    cv->cur.fill = cnvs_unpremul_of(r, g, b, a);
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

void canvas_add_fill_color_stop(canvas *__single cv, float offset,
                                float r, float g, float b, float a) {
    cnvs_gradient_add_stop(&cv->cur.fill_grad, offset,
                           cnvs_unpremul_of(r, g, b, a));
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

void canvas_add_stroke_color_stop(canvas *__single cv, float offset,
                                  float r, float g, float b, float a) {
    cnvs_gradient_add_stop(&cv->cur.stroke_grad, offset,
                           cnvs_unpremul_of(r, g, b, a));
}

void canvas_set_global_alpha(canvas *__single cv, float alpha) {
    cv->cur.global_alpha = alpha;
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
    int x0 = (int)fx0, y0 = (int)fy0, x1 = (int)fx1, y1 = (int)fy1;
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
    for (int py = 0; py < b.h; py++) {
        for (int px = 0; px < b.w; px++) {
            int i = py * b.w + px;
            float covf = (float)cv->cov[i] / 255.0f;
            cnvs_unpremul col;
            float a;
            if (is_grad) {
                col = cnvs_gradient_sample(
                    gr, (cnvs_vec2){ .x = (float)b.x + (float)px + 0.5f,
                                     .y = (float)b.y + (float)py + 0.5f }, ga);
                a = (float)col.a;
            } else {
                col = solid;
                a = (float)col.a * ga;
            }
            // Fold coverage and global alpha into the paint's alpha, then
            // premultiply -- the tile stores premultiplied pixels.
            float alpha = a * covf;
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
    if (b.w > 0 && b.h > 0) {
        compositor_clear(cv->comp, b.x, b.y, b.w, b.h);
    }
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
    if (!anticlockwise) {
        while (sweep < 0.0f) {
            sweep += two_pi;
        }
    } else {
        while (sweep > 0.0f) {
            sweep -= two_pi;
        }
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
    int segs = (int)fsegs;
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
    cv->cur.fill_rule = (rule == CANVAS_EVENODD) ? CNVS_EVENODD : CNVS_NONZERO;
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
    cv->cur.stroke = cnvs_unpremul_of(r, g, b, a);
    cv->cur.stroke_is_gradient = 0;
}

void canvas_set_line_width(canvas *__single cv, float width) {
    cv->cur.line_width = width;
}

void canvas_set_line_join(canvas *__single cv, canvas_line_join join) {
    cv->cur.line_join = (join == CANVAS_JOIN_ROUND)   ? CNVS_JOIN_ROUND
                        : (join == CANVAS_JOIN_BEVEL) ? CNVS_JOIN_BEVEL
                                                      : CNVS_JOIN_MITER;
}

void canvas_set_line_cap(canvas *__single cv, canvas_line_cap cap) {
    cv->cur.line_cap = (cap == CANVAS_CAP_ROUND)    ? CNVS_CAP_ROUND
                       : (cap == CANVAS_CAP_SQUARE) ? CNVS_CAP_SQUARE
                                                    : CNVS_CAP_BUTT;
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

void canvas_set_line_dash_offset(canvas *__single cv, float offset) {
    cv->cur.dash_offset = offset;
}

static void stroke_device_path(canvas *__single cv, cnvs_path const *p) {
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
            return;
        }
    }
    if (cv->scratch_verts.len < 3) {
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

float canvas_measure_text(canvas *__single cv, char const *__null_terminated text) {
    cnvs_font *__single f = ensure_font(cv);
    return f ? cnvs_font_advance(f, text) : 0.0f;
}

void canvas_fill_text(canvas *__single cv, char const *__null_terminated text,
                      float x, float y) {
    cnvs_font *__single f = ensure_font(cv);
    if (!f) {
        return;
    }
    cnvs_path_reset(&cv->text_path);
    cnvs_font_outline(f, text, x, y, cv->cur.ctm, CANVAS_FLATTEN_TOL, &cv->text_path);
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
    cnvs_path_reset(&cv->text_path);
    cnvs_font_outline(f, text, x, y, cv->cur.ctm, CANVAS_FLATTEN_TOL, &cv->text_path);
    stroke_device_path(cv, &cv->text_path);
}

// Bilinear sample of an RGBA8 source at source-pixel coords (fx,fy), straight
// alpha, clamp-to-edge.  Four checked taps per pixel -- the indexing is bounded
// by the clamps, and -fbounds-safety guards each src[] against `slen`.
static void sample_src(uint8_t const *__counted_by(slen) src, int slen,
                       int sw, int sh, float fx, float fy,
                       float *__counted_by(4) out) {
    (void)slen;
    float gx = fx - 0.5f, gy = fy - 0.5f;
    float fxx = floorf(gx), fyy = floorf(gy);
    int x0 = (int)fxx, y0 = (int)fyy;
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

void canvas_draw_image_subrect(canvas *__single cv,
                               uint8_t const *__counted_by(sw * sh * 4) src,
                               int sw, int sh, float sx, float sy,
                               float sww, float shh, float dx, float dy,
                               float dw, float dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0.0f || dh <= 0.0f) {
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
            sample_src(src, sw * sh * 4, sw, sh, fsx, fsy, s);
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

void canvas_read_rgba(canvas *__single cv, uint8_t *__counted_by(len) out, int len) {
    compositor_read_rgba(cv->comp, out, len);
}

bool canvas_write_png(canvas *__single cv, char const *__null_terminated path) {
    int const len = cv->width * cv->height * 4;
    uint8_t *__counted_by(len) out = malloc((size_t)len);
    if (!out) {
        return false;
    }
    compositor_read_rgba(cv->comp, out, len);
    bool ok = cnvs_png_write(path, out, cv->width, cv->height);
    free(out);
    return ok;
}

void canvas_get_image_data(canvas *__single cv, int x, int y, int w, int h,
                           uint8_t *__counted_by(len) out, int len) {
    if (w <= 0 || h <= 0 || len < w * h * 4) {
        return;
    }
    memset(out, 0, (size_t)len);  // pixels outside the canvas stay transparent
    int const clen = cv->width * cv->height * 4;
    uint8_t *__counted_by(clen) buf = malloc((size_t)clen);
    if (!buf) {
        return;
    }
    compositor_read_rgba(cv->comp, buf, clen);
    cnvs_blit_rgba(out, w, h, 0, 0, buf, cv->width, cv->height, x, y, w, h);
    free(buf);
}

void canvas_put_image_data(canvas *__single cv,
                           uint8_t const *__counted_by(len) data, int len,
                           int w, int h, int dx, int dy) {
    if (w <= 0 || h <= 0 || len < w * h * 4) {
        return;
    }
    int cx0 = dx < 0 ? 0 : dx;
    int cy0 = dy < 0 ? 0 : dy;
    int cx1 = dx + w;
    int cy1 = dy + h;
    if (cx1 > cv->width) {
        cx1 = cv->width;
    }
    if (cy1 > cv->height) {
        cy1 = cv->height;
    }
    int rw = cx1 - cx0;
    int rh = cy1 - cy0;
    if (rw <= 0 || rh <= 0) {
        return;
    }
    if (cx0 == dx && cy0 == dy && rw == w && rh == h) {
        compositor_replace(cv->comp, dx, dy, w, h, data);  // fully inside; no copy
        return;
    }
    int const tlen = rw * rh * 4;
    uint8_t *__counted_by(tlen) tmp = malloc((size_t)tlen);
    if (!tmp) {
        return;
    }
    cnvs_blit_rgba(tmp, rw, rh, 0, 0, data, w, h, cx0 - dx, cy0 - dy, rw, rh);
    compositor_replace(cv->comp, cx0, cy0, rw, rh, tmp);
    free(tmp);
}
