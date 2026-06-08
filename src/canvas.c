#include "canvas.h"

#include "cnvs_geom.h"
#include "cnvs_math.h"
#include "cnvs_mem.h"
#include "cnvs_path.h"
#include "cnvs_png.h"
#include "cnvs_stroke.h"
#include "cnvs_tess.h"
#include "gpu.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

// Maximum chord deviation (device pixels) when flattening curves.
#define CANVAS_FLATTEN_TOL 0.25f

struct canvas_state {
    cnvs_mat ctm;
    gpu_rgba fill;
    gpu_rgba stroke;
    float global_alpha;
    float line_width;
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
    cnvs_verts scratch_verts;  // reused tessellation/stroke output
    cnvs_ints scratch_ints;    // reused ear-clip working ring
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
    cv->stack = NULL;
    cv->stack_len = 0;
    cv->stack_cap = 0;
    cnvs_path_init(&cv->path);
    // scratch_verts / scratch_ints are zero-initialised by calloc.
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
    cnvs_ints_free(&cv->scratch_ints);
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

// Transform an axis-aligned rectangle by the CTM and draw it as two triangles.
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

// Transform a user-space point by the current transform.
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

void canvas_arc(canvas *__single cv, float x, float y, float radius,
                float start_angle, float end_angle, bool anticlockwise) {
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
    float r = radius < 0.0f ? -radius : radius;
    float rr = r > CANVAS_FLATTEN_TOL ? r : CANVAS_FLATTEN_TOL;
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
    for (int i = 0; i <= segs; i++) {
        float t = start_angle + sweep * ((float)i / (float)segs);
        cnvs_vec2 p = xf(cv, x + r * cosf(t), y + r * sinf(t));
        if (i == 0 && !cv->path.has_cur) {
            cnvs_path_move_to(&cv->path, p);
        } else {
            cnvs_path_line_to(&cv->path, p);
        }
    }
}

void canvas_close_path(canvas *__single cv) {
    cnvs_path_close(&cv->path);
}

void canvas_fill(canvas *__single cv) {
    cnvs_verts_reset(&cv->scratch_verts);
    for (int s = 0; s < cv->path.sp_len; s++) {
        cnvs_subpath sp = cv->path.subs[s];
        if (sp.count < 3) {
            continue;
        }
        cnvs_vec2 *poly = cv->path.pts + sp.start;  // bounds-checked slice
        if (!cnvs_tess_polygon(poly, sp.count, &cv->scratch_verts,
                               &cv->scratch_ints)) {
            return;
        }
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

void canvas_stroke(canvas *__single cv) {
    cnvs_verts_reset(&cv->scratch_verts);
    // Line width is in user units; bake the CTM scale into device space.
    cnvs_mat m = cv->cur.ctm;
    float det = m.a * m.d - m.b * m.c;
    float scale = sqrtf(fabsf(det));
    float hw = cv->cur.line_width * 0.5f * scale;
    for (int s = 0; s < cv->path.sp_len; s++) {
        cnvs_subpath sp = cv->path.subs[s];
        if (sp.count < 2) {
            continue;
        }
        cnvs_vec2 *poly = cv->path.pts + sp.start;
        if (!cnvs_stroke_polyline(poly, sp.count, sp.closed, hw,
                                  &cv->scratch_verts)) {
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
