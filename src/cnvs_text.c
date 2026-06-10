#include "cnvs_text.h"

#include <stdlib.h>

// Checked-core consumers of a shaped run.  Every glyph and cluster access is
// bounds-checked against the counts the boundary handed over; the cluster value is
// range-checked against the source length before it is trusted as an index.

float cnvs_shaped_width(cnvs_shaped const *__single s) {
    if (!s) {
        return 0.0f;
    }
    float w = 0.0f;
    for (int r = 0; r < s->nruns; r++) {
        cnvs_glyph_run run = s->run[r];
        for (int i = 0; i < run.count; i++) {
            w += run.xadv[i];
        }
    }
    return w;
}

int cnvs_shaped_index_at_x(cnvs_shaped const *__single s, float x) {
    if (!s) {
        return -1;
    }
    float pen = 0.0f;
    for (int r = 0; r < s->nruns; r++) {
        cnvs_glyph_run run = s->run[r];  // glyphs are visual-order, so a single
        for (int i = 0; i < run.count; i++) {  // left-to-right sweep works for RTL too
            if (x < pen + run.xadv[i]) {
                int32_t c = run.cluster[i];
                if (c < 0 || c >= s->text_len) {  // defensive: a bad cluster is not
                    return -1;                    // a valid source index
                }
                return c;
            }
            pen += run.xadv[i];
        }
    }
    return -1;
}

float cnvs_shaped_x_at_index(cnvs_shaped const *__single s, int index) {
    if (!s) {
        return 0.0f;
    }
    float pen = 0.0f;
    for (int r = 0; r < s->nruns; r++) {
        cnvs_glyph_run run = s->run[r];
        for (int i = 0; i < run.count; i++) {
            if (run.cluster[i] == index) {
                return pen;  // leading visual edge of the glyph at this logical index
            }
            pen += run.xadv[i];
        }
    }
    return pen;  // index past the last glyph -> end of the line
}

int cnvs_shaped_selection(cnvs_shaped const *__single s, int lo, int hi,
                          cnvs_xspan *__counted_by(max) out, int max) {
    if (!s || max <= 0 || hi <= lo) {
        return 0;
    }
    int n = 0;
    float pen = 0.0f, start = 0.0f;
    bool in = false;  // currently inside a selected visual run of glyphs
    for (int r = 0; r < s->nruns; r++) {
        cnvs_glyph_run run = s->run[r];
        for (int i = 0; i < run.count; i++) {
            bool sel = run.cluster[i] >= lo && run.cluster[i] < hi;
            if (sel && !in) {
                start = pen;
                in = true;
            } else if (!sel && in) {
                if (n < max) {
                    out[n++] = (cnvs_xspan){ start, pen };
                }
                in = false;
            }
            pen += run.xadv[i];
        }
    }
    if (in && n < max) {
        out[n++] = (cnvs_xspan){ start, pen };
    }
    return n;
}

// How many control points one canonical verb consumes; -1 for a byte that is not a
// verb.  The curve stream is boundary data, so a bad byte stops the walk cleanly
// rather than being trusted -- the same posture as the cluster range check.
static int verb_pts(cnvs_glyph_verb v) {
    switch (v) {
        case CNVS_GLYPH_MOVE:
        case CNVS_GLYPH_LINE:  return 1;
        case CNVS_GLYPH_QUAD:  return 2;
        case CNVS_GLYPH_CUBIC: return 3;
        case CNVS_GLYPH_CLOSE: return 0;
    }
    return -1;  // not a valid verb byte
}

// Map one canonical font-unit point to device space: scale to user px (font units
// are y up, canvas user space is y down), place at the pen, then the CTM.
struct glyph_place {
    cnvs_mat to_device;
    float ox, oy, scale;
};

static cnvs_vec2 place(struct glyph_place const *g, cnvs_vec2 fu) {
    cnvs_vec2 u = { g->ox + fu.x * g->scale, g->oy - fu.y * g->scale };
    return cnvs_mat_apply(g->to_device, u);
}

void cnvs_glyph_outline(void *__single font, uint16_t glyph, float size_px,
                        float ox, float oy, cnvs_mat to_device, float tol,
                        cnvs_path *__single out) {
    // Stack buffers cover the typical glyph; a rare complex one (a dense CJK
    // ideograph, an ornate display face) takes the grow-and-refetch path below.
    enum { VSTACK = 256, PSTACK = 512 };
    cnvs_glyph_verb vstack[VSTACK];
    cnvs_vec2 pstack[PSTACK];
    int nv = 0, np = 0;
    float upem = 0.0f;
    cnvs_glyph_curves(font, glyph, vstack, VSTACK, pstack, PSTACK, &nv, &np, &upem);
    if (nv <= 0 || np < 0 || upem <= 0.0f) {
        return;  // a blank or color glyph (or no font): no outline to add
    }
    cnvs_glyph_verb *verb = vstack;
    cnvs_vec2 *pt = pstack;
    cnvs_glyph_verb *vheap = NULL;
    cnvs_vec2 *pheap = NULL;
    if (nv > VSTACK || np > PSTACK) {
        vheap = malloc((size_t)nv * sizeof *vheap);
        pheap = malloc((size_t)(np > 0 ? np : 1) * sizeof *pheap);
        if (!vheap || !pheap) {
            free(vheap);
            free(pheap);
            return;  // OOM: skip the glyph, the same degradation as a failed append
        }
        int nv2 = 0, np2 = 0;
        cnvs_glyph_curves(font, glyph, vheap, nv, pheap, np, &nv2, &np2, &upem);
        if (nv2 < nv) { nv = nv2; }  // defensive: walk only what was both reported
        if (np2 < np) { np = np2; }  // and allocated
        verb = vheap;
        pt = pheap;
    }
    struct glyph_place g = { .to_device = to_device, .ox = ox, .oy = oy,
                             .scale = size_px / upem };
    int ip = 0;
    for (int iv = 0; iv < nv; iv++) {
        cnvs_glyph_verb v = verb[iv];
        int k = verb_pts(v);
        if (k < 0 || ip + k > np) {
            break;  // not a verb, or its points would run past the count: stop
        }
        switch (v) {
            case CNVS_GLYPH_MOVE:
                cnvs_path_move_to(out, place(&g, pt[ip]));
                break;
            case CNVS_GLYPH_LINE:
                cnvs_path_line_to(out, place(&g, pt[ip]));
                break;
            case CNVS_GLYPH_QUAD:
                cnvs_path_quad_to(out, place(&g, pt[ip]), place(&g, pt[ip + 1]), tol);
                break;
            case CNVS_GLYPH_CUBIC:
                cnvs_path_cubic_to(out, place(&g, pt[ip]), place(&g, pt[ip + 1]),
                                   place(&g, pt[ip + 2]), tol);
                break;
            case CNVS_GLYPH_CLOSE:
                cnvs_path_close(out);
                break;
        }
        ip += k;
    }
    free(vheap);
    free(pheap);
}

float cnvs_shaped_outline(cnvs_shaped const *__single s, float ox, float oy,
                          cnvs_mat to_device, float tol, cnvs_path *__single out,
                          cnvs_color_glyph_fn color, void *__single ctx) {
    if (!s) {
        return 0.0f;
    }
    float pen = ox;
    for (int r = 0; r < s->nruns; r++) {
        cnvs_glyph_run run = s->run[r];  // visual-order glyphs, so advancing the pen
        bool is_color = cnvs_run_is_color(run.font);
        for (int i = 0; i < run.count; i++) {  // left-to-right places RTL runs too
            if (is_color) {
                if (color) {  // no outline to trace: hand it over to be drawn
                    color(ctx, run.font, run.glyph[i], pen, oy);
                }
            } else {
                cnvs_glyph_outline(run.font, run.glyph[i], s->size_px, pen, oy,
                                   to_device, tol, out);
            }
            pen += run.xadv[i];
        }
    }
    return pen - ox;
}
