#include "cnvs_fill.h"

#include "cnvs_mem.h"

#include <math.h>
#include <stdlib.h>

static bool edges_push(cnvs_edges *e, cnvs_edge edge) {
    if (e->len >= e->cap) {
        int newcap = cnvs_grow_cap(e->cap, e->len + 1);
        cnvs_edge *nd = realloc(e->data, (size_t)newcap * sizeof *nd);
        if (!nd) {
            return false;
        }
        e->data = nd;
        e->cap = newcap;
    }
    e->data[e->len] = edge;
    e->len += 1;
    return true;
}

static bool xings_push(cnvs_xings *x, cnvs_xing xing) {
    if (x->len >= x->cap) {
        int newcap = cnvs_grow_cap(x->cap, x->len + 1);
        cnvs_xing *nd = realloc(x->data, (size_t)newcap * sizeof *nd);
        if (!nd) {
            return false;
        }
        x->data = nd;
        x->cap = newcap;
    }
    x->data[x->len] = xing;
    x->len += 1;
    return true;
}

static bool spans_push(cnvs_spans *s, cnvs_span span) {
    if (s->len >= s->cap) {
        int newcap = cnvs_grow_cap(s->cap, s->len + 1);
        cnvs_span *nd = realloc(s->data, (size_t)newcap * sizeof *nd);
        if (!nd) {
            return false;
        }
        s->data = nd;
        s->cap = newcap;
    }
    s->data[s->len] = span;
    s->len += 1;
    return true;
}

void cnvs_edges_free(cnvs_edges *e) {
    free(e->data);
    e->data = NULL;
    e->len = 0;
    e->cap = 0;
}

void cnvs_xings_free(cnvs_xings *x) {
    free(x->data);
    x->data = NULL;
    x->len = 0;
    x->cap = 0;
}

void cnvs_spans_free(cnvs_spans *s) {
    free(s->data);
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

static bool add_edge(cnvs_edges *edges, cnvs_vec2 p0, cnvs_vec2 p1) {
    cnvs_edge e;
    if (p0.y < p1.y) {
        e.ytop = p0.y;
        e.ybot = p1.y;
        e.x_at_top = p0.x;
        e.dir = 1;
    } else if (p0.y > p1.y) {
        e.ytop = p1.y;
        e.ybot = p0.y;
        e.x_at_top = p1.x;
        e.dir = -1;
    } else {
        return true;  // horizontal: crosses no scanline
    }
    e.dxdy = (p1.x - p0.x) / (p1.y - p0.y);
    return edges_push(edges, e);
}

// qsort handing the comparator bare `const void *` is the one spot where a
// __counted_by element loses its bounds; each pointer addresses a single edge,
// so we forge a __single from it.  (The per-scanline crossing sort below stays
// fully checked instead.)
static int edge_cmp(void const *pa, void const *pb) {
    cnvs_edge const *a = __unsafe_forge_single(cnvs_edge const *, pa);
    cnvs_edge const *b = __unsafe_forge_single(cnvs_edge const *, pb);
    if (a->ytop < b->ytop) {
        return -1;
    }
    if (a->ytop > b->ytop) {
        return 1;
    }
    return 0;
}

static void xings_sort(cnvs_xings *x) {
    for (int i = 1; i < x->len; i++) {
        cnvs_xing key = x->data[i];
        int j = i - 1;
        while (j >= 0 && x->data[j].x > key.x) {
            x->data[j + 1] = x->data[j];
            j -= 1;
        }
        x->data[j + 1] = key;
    }
}

static bool emit_span(cnvs_spans *out, float xl, float xr, int row, int width) {
    if (xl < 0.0f) {
        xl = 0.0f;
    }
    if (xr > (float)width) {
        xr = (float)width;
    }
    if (xr <= xl) {
        return true;
    }
    return spans_push(out, (cnvs_span){ .xl = xl, .xr = xr, .row = row });
}

bool cnvs_fill_spans(cnvs_path const *path, cnvs_fill_rule rule,
                     int width, int height, cnvs_spans *out,
                     cnvs_edges *edges, cnvs_xings *xings) {
    edges->len = 0;
    for (int s = 0; s < path->sp_len; s++) {
        cnvs_subpath sp = path->subs[s];
        if (sp.count < 3) {
            continue;
        }
        for (int k = 0; k < sp.count; k++) {
            cnvs_vec2 p0 = path->pts[sp.start + k];
            cnvs_vec2 p1 = path->pts[sp.start + (k + 1) % sp.count];
            if (!add_edge(edges, p0, p1)) {
                return false;
            }
        }
    }
    if (edges->len == 0) {
        return true;
    }

    qsort(edges->data, (size_t)edges->len, sizeof(cnvs_edge), edge_cmp);

    float ymin = edges->data[0].ytop;  // smallest ytop after the sort
    float ymax = ymin;
    for (int i = 0; i < edges->len; i++) {
        if (edges->data[i].ybot > ymax) {
            ymax = edges->data[i].ybot;
        }
    }
    float fy0 = floorf(ymin);
    float fy1 = ceilf(ymax);
    int row0 = (int)fy0;
    int row1 = (int)fy1;
    if (row0 < 0) {
        row0 = 0;
    }
    if (row1 > height) {
        row1 = height;
    }

    for (int row = row0; row < row1; row++) {
        float yc = (float)row + 0.5f;
        xings->len = 0;
        for (int i = 0; i < edges->len; i++) {
            cnvs_edge e = edges->data[i];
            if (e.ytop > yc) {
                break;  // edges are sorted by ytop: none later cross this row
            }
            if (e.ybot <= yc) {
                continue;  // [ytop, ybot) is half-open
            }
            float x = e.x_at_top + (yc - e.ytop) * e.dxdy;
            if (!xings_push(xings, (cnvs_xing){ .x = x, .dir = e.dir })) {
                return false;
            }
        }
        xings_sort(xings);

        int wind = 0;
        int count = 0;
        bool inside = false;
        float span_start = 0.0f;
        for (int i = 0; i < xings->len; i++) {
            bool was_inside = inside;
            if (rule == CNVS_EVENODD) {
                count += 1;
                inside = (count & 1) != 0;
            } else {
                wind += xings->data[i].dir;
                inside = wind != 0;
            }
            if (!was_inside && inside) {
                span_start = xings->data[i].x;
            } else if (was_inside && !inside) {
                if (!emit_span(out, span_start, xings->data[i].x, row, width)) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool cnvs_fill_path(cnvs_path const *path, cnvs_fill_rule rule,
                    int width, int height, cnvs_verts *out,
                    cnvs_edges *edges, cnvs_xings *xings, cnvs_spans *spans) {
    spans->len = 0;
    if (!cnvs_fill_spans(path, rule, width, height, spans, edges, xings)) {
        return false;
    }
    for (int i = 0; i < spans->len; i++) {
        cnvs_span s = spans->data[i];
        float y0 = (float)s.row;
        float y1 = (float)(s.row + 1);
        gpu_vert a = { .x = s.xl, .y = y0 };
        gpu_vert b = { .x = s.xr, .y = y0 };
        gpu_vert c = { .x = s.xr, .y = y1 };
        gpu_vert d = { .x = s.xl, .y = y1 };
        if (!cnvs_verts_tri(out, a, b, c) || !cnvs_verts_tri(out, a, c, d)) {
            return false;
        }
    }
    return true;
}
