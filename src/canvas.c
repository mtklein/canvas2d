#include "canvas.h"

#include "cnvs_fill.h"
#include "cnvs_geom.h"
#include "cnvs_image.h"
#include "cnvs_math.h"
#include "cnvs_mem.h"
#include "cnvs_path.h"
#include "cnvs_png.h"
#include "cnvs_stroke.h"
#include "gpu.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Maximum chord deviation (device pixels) when flattening curves.
#define CANVAS_FLATTEN_TOL 0.25f
#define CANVAS_MAX_DASH 16

struct canvas_state {
    cnvs_mat ctm;
    gpu_rgba fill;
    gpu_rgba stroke;
    float global_alpha;
    float line_width;
    cnvs_fill_rule fill_rule;
    cnvs_line_join line_join;
    cnvs_line_cap line_cap;
    float miter_limit;
    float dash[CANVAS_MAX_DASH];
    int dash_count;
    float dash_offset;
};

struct canvas {
    gpu *__single g;
    int width;
    int height;
    struct canvas_state cur;
    struct canvas_state *__counted_by(stack_cap) stack;
    int stack_len;
    int stack_cap;
    cnvs_path path;
    cnvs_verts scratch_verts;
    cnvs_edges scratch_edges;
    cnvs_xings scratch_xings;
};

canvas *__single canvas_create(int width, int height) {
    if (width <= 0 || height <= 0) {
        return NULL;
    }
    gpu *__single g = gpu_create(width, height);
    if (!g) {
        return NULL;
    }
    canvas *__single cv = calloc(1, sizeof *cv);
    if (!cv) {
        gpu_destroy(g);
        return NULL;
    }
    cv->g = g;
    cv->width = width;
    cv->height = height;
    cv->cur.ctm = cnvs_mat_identity();
    cv->cur.fill = (gpu_rgba){ .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f };
    cv->cur.stroke = (gpu_rgba){ .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f };
    cv->cur.global_alpha = 1.0f;
    cv->cur.line_width = 1.0f;
    cv->cur.fill_rule = CNVS_NONZERO;
    cv->cur.line_join = CNVS_JOIN_MITER;
    cv->cur.line_cap = CNVS_CAP_BUTT;
    cv->cur.miter_limit = 10.0f;
    cv->cur.dash_count = 0;
    cv->cur.dash_offset = 0.0f;
    cv->stack = NULL;
    cv->stack_len = 0;
    cv->stack_cap = 0;
    cnvs_path_init(&cv->path);
    return cv;
}

void canvas_destroy(canvas *__single cv) {
    if (!cv) {
        return;
    }
    gpu_destroy(cv->g);
    free(cv->stack);
    cnvs_path_free(&cv->path);
    cnvs_verts_free(&cv->scratch_verts);
    cnvs_edges_free(&cv->scratch_edges);
    cnvs_xings_free(&cv->scratch_xings);
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
    cv->stack_len += 1;
}

void canvas_restore(canvas *__single cv) {
    if (cv->stack_len > 0) {
        cv->stack_len -= 1;
        cv->cur = cv->stack[cv->stack_len];
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
    cv->cur.fill = (gpu_rgba){ .r = r, .g = g, .b = b, .a = a };
}

void canvas_set_global_alpha(canvas *__single cv, float alpha) {
    cv->cur.global_alpha = alpha;
}

static void fill_quad(canvas *__single cv, float x, float y, float w, float h,
                      gpu_rgba color, bool blend) {
    cnvs_mat m = cv->cur.ctm;
    cnvs_vec2 p0 = cnvs_mat_apply(m, (cnvs_vec2){ .x = x, .y = y });
    cnvs_vec2 p1 = cnvs_mat_apply(m, (cnvs_vec2){ .x = x + w, .y = y });
    cnvs_vec2 p2 = cnvs_mat_apply(m, (cnvs_vec2){ .x = x + w, .y = y + h });
    cnvs_vec2 p3 = cnvs_mat_apply(m, (cnvs_vec2){ .x = x, .y = y + h });
    gpu_vert verts[6] = {
        { .x = p0.x, .y = p0.y }, { .x = p1.x, .y = p1.y }, { .x = p2.x, .y = p2.y },
        { .x = p0.x, .y = p0.y }, { .x = p2.x, .y = p2.y }, { .x = p3.x, .y = p3.y },
    };
    gpu_draw_solid(cv->g, verts, 6, color, blend);
}

void canvas_clear_rect(canvas *__single cv, float x, float y, float w, float h) {
    // clearRect erases to transparent black, overwriting (no blend).
    fill_quad(cv, x, y, w, h,
              (gpu_rgba){ .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f }, false);
}

void canvas_fill_rect(canvas *__single cv, float x, float y, float w, float h) {
    gpu_rgba color = cv->cur.fill;
    color.a *= cv->cur.global_alpha;
    fill_quad(cv, x, y, w, h, color, true);
}

static cnvs_vec2 xf(canvas *__single cv, float x, float y) {
    return cnvs_mat_apply(cv->cur.ctm, (cnvs_vec2){ .x = x, .y = y });
}

void canvas_begin_path(canvas *__single cv) {
    cnvs_path_reset(&cv->path);
}

void canvas_move_to(canvas *__single cv, float x, float y) {
    cnvs_path_move_to(&cv->path, xf(cv, x, y));
}

void canvas_line_to(canvas *__single cv, float x, float y) {
    cnvs_path_line_to(&cv->path, xf(cv, x, y));
}

void canvas_rect(canvas *__single cv, float x, float y, float w, float h) {
    cnvs_path_rect(&cv->path, xf(cv, x, y), xf(cv, x + w, y),
                   xf(cv, x + w, y + h), xf(cv, x, y + h));
}

void canvas_quadratic_curve_to(canvas *__single cv,
                               float cpx, float cpy, float x, float y) {
    cnvs_path_quad_to(&cv->path, xf(cv, cpx, cpy), xf(cv, x, y),
                      CANVAS_FLATTEN_TOL);
}

void canvas_bezier_curve_to(canvas *__single cv, float c1x, float c1y,
                            float c2x, float c2y, float x, float y) {
    cnvs_path_cubic_to(&cv->path, xf(cv, c1x, c1y), xf(cv, c2x, c2y),
                       xf(cv, x, y), CANVAS_FLATTEN_TOL);
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

void canvas_close_path(canvas *__single cv) {
    cnvs_path_close(&cv->path);
}

void canvas_set_fill_rule(canvas *__single cv, canvas_fill_rule rule) {
    cv->cur.fill_rule = (rule == CANVAS_EVENODD) ? CNVS_EVENODD : CNVS_NONZERO;
}

void canvas_fill(canvas *__single cv) {
    cnvs_verts_reset(&cv->scratch_verts);
    if (!cnvs_fill_path(&cv->path, cv->cur.fill_rule, cv->width, cv->height,
                        &cv->scratch_verts, &cv->scratch_edges,
                        &cv->scratch_xings)) {
        return;
    }
    if (cv->scratch_verts.len > 0) {
        gpu_rgba color = cv->cur.fill;
        color.a *= cv->cur.global_alpha;
        gpu_draw_solid(cv->g, cv->scratch_verts.data, cv->scratch_verts.len,
                       color, true);
    }
}

void canvas_set_stroke_rgba(canvas *__single cv, float r, float g, float b, float a) {
    cv->cur.stroke = (gpu_rgba){ .r = r, .g = g, .b = b, .a = a };
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

void canvas_stroke(canvas *__single cv) {
    cnvs_verts_reset(&cv->scratch_verts);
    // Line width and dash lengths are in user units; bake the CTM scale in.
    cnvs_mat m = cv->cur.ctm;
    float det = m.a * m.d - m.b * m.c;
    float scale = sqrtf(fabsf(det));
    float hw = cv->cur.line_width * 0.5f * scale;

    bool dashed = cv->cur.dash_count > 0;
    float sdash[CANVAS_MAX_DASH];
    for (int i = 0; i < cv->cur.dash_count; i++) {
        sdash[i] = cv->cur.dash[i] * scale;
    }
    float soff = cv->cur.dash_offset * scale;

    for (int s = 0; s < cv->path.sp_len; s++) {
        cnvs_subpath sp = cv->path.subs[s];
        if (sp.count < 2) {
            continue;
        }
        cnvs_vec2 *poly = cv->path.pts + sp.start;
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
    if (cv->scratch_verts.len > 0) {
        gpu_rgba color = cv->cur.stroke;
        color.a *= cv->cur.global_alpha;
        gpu_draw_solid(cv->g, cv->scratch_verts.data, cv->scratch_verts.len,
                       color, true);
    }
}

void canvas_read_rgba(canvas *__single cv, uint8_t *__counted_by(len) out, int len) {
    gpu_read_rgba(cv->g, out, len);
}

bool canvas_write_png(canvas *__single cv, char const *__null_terminated path) {
    int const len = cv->width * cv->height * 4;
    uint8_t *__counted_by(len) out = malloc((size_t)len);
    if (!out) {
        return false;
    }
    gpu_read_rgba(cv->g, out, len);
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
    gpu_read_rgba(cv->g, buf, clen);
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
        gpu_write_region(cv->g, dx, dy, w, h, data);  // fully inside; no copy
        return;
    }
    int const tlen = rw * rh * 4;
    uint8_t *__counted_by(tlen) tmp = malloc((size_t)tlen);
    if (!tmp) {
        return;
    }
    cnvs_blit_rgba(tmp, rw, rh, 0, 0, data, w, h, cx0 - dx, cy0 - dy, rw, rh);
    gpu_write_region(cv->g, cx0, cy0, rw, rh, tmp);
    free(tmp);
}
