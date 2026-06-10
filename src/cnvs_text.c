#include "cnvs_text.h"

#include <stdlib.h>
#include <string.h>

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

// Transform + flatten one glyph's canonical curves into `out`: the shared back
// half of the cached and uncached outline paths.  The curve stream is boundary
// data (fresh or remembered), so the walk stays defensive: a byte that is not a
// verb, or a verb whose points would run past the count, stops the walk cleanly
// rather than being trusted as an index.
static void walk_curves(cnvs_glyph_verb const *__counted_by(nv) verb, int nv,
                        cnvs_vec2 const *__counted_by(np) pt, int np,
                        struct glyph_place const *__single g, float tol,
                        cnvs_path *__single out) {
    int ip = 0;
    for (int iv = 0; iv < nv; iv++) {
        cnvs_glyph_verb v = verb[iv];
        int k = verb_pts(v);
        if (k < 0 || ip + k > np) {
            break;  // not a verb, or its points would run past the count: stop
        }
        switch (v) {
            case CNVS_GLYPH_MOVE:
                cnvs_path_move_to(out, place(g, pt[ip]));
                break;
            case CNVS_GLYPH_LINE:
                cnvs_path_line_to(out, place(g, pt[ip]));
                break;
            case CNVS_GLYPH_QUAD:
                cnvs_path_quad_to(out, place(g, pt[ip]), place(g, pt[ip + 1]), tol);
                break;
            case CNVS_GLYPH_CUBIC:
                cnvs_path_cubic_to(out, place(g, pt[ip]), place(g, pt[ip + 1]),
                                   place(g, pt[ip + 2]), tol);
                break;
            case CNVS_GLYPH_CLOSE:
                cnvs_path_close(out);
                break;
        }
        ip += k;
    }
}

// ---------------------------------------------------------------------------
// The params -> derived-data lookup (the header has the overview).  All checked
// C: the boundary is consulted only on a miss, every insert is best-effort (an
// allocation failure leaves the entry out and the op still rendered), and the
// stored bytes are exactly what the boundary handed back -- a hit replays them
// through the same checked math a miss runs.

void cnvs_text_cache_init(cnvs_text_cache *__single c) {
    memset(c, 0, sizeof *c);  // empty slots: NULL pointers with zero counts
}

void cnvs_text_cache_clear(cnvs_text_cache *__single c) {
    for (int i = 0; i < CNVS_SHAPE_CACHE_N; i++) {
        if (c->shape[i].s) {
            cnvs_shaped_free(c->shape[i].s);  // releases the runs' CTFontRefs too
            free(c->shape[i].text);
        }
    }
    for (int i = 0; i < c->glyph_cap; i++) {
        if (c->glyph[i].used) {
            free(c->glyph[i].verb);
            free(c->glyph[i].pt);
        }
    }
    free(c->glyph);
    for (int i = 0; i < c->nfonts; i++) {
        free(c->font[i].name);
    }
    cnvs_text_cache_init(c);  // back to the empty state, stats included
}

cnvs_shaped const *__single cnvs_text_cache_shape(cnvs_text_cache *__single c,
        char const *__null_terminated name, float size_px,
        char const *__counted_by(len) text, int len) {
    uint32_t size_bits = 0;
    memcpy(&size_bits, &size_px, sizeof size_bits);
    for (int i = 0; i < CNVS_SHAPE_CACHE_N; i++) {
        cnvs_shape_slot *slot = &c->shape[i];
        if (slot->s && slot->size_bits == size_bits && slot->len == len &&
            memcmp(slot->text, text, (size_t)len) == 0) {
            slot->stamp = ++c->tick;
            c->shape_hits++;
            return slot->s;
        }
    }
    c->shape_misses++;
    // Miss.  One allocation serves twice: the NUL-terminated copy cnvs_shape
    // wants now, and the stored key bytes once shaping succeeds.  If it fails,
    // the op degrades exactly as the uncached copy used to (nothing to draw).
    char *tz = malloc((size_t)len + 1);
    if (!tz) {
        return NULL;
    }
    memcpy(tz, text, (size_t)len);
    tz[len] = '\0';
    cnvs_shaped *__single s = cnvs_shape(name, size_px,
                                         __unsafe_null_terminated_from_indexable(tz));
    if (!s) {
        free(tz);
        return NULL;  // boundary failure: nothing to cache, nothing to draw
    }
    // Insert into an empty slot, else evict the least-recently-used one.  This
    // cannot invalidate a borrowed line: call sites take one lookup per op and
    // never nest, so no borrow is alive across an insert.
    cnvs_shape_slot *victim = &c->shape[0];
    for (int i = 0; i < CNVS_SHAPE_CACHE_N; i++) {
        if (!c->shape[i].s) {
            victim = &c->shape[i];
            break;
        }
        if (c->shape[i].stamp < victim->stamp) {
            victim = &c->shape[i];
        }
    }
    if (victim->s) {
        cnvs_shaped_free(victim->s);
        free(victim->text);
    }
    victim->text = tz;
    victim->len = len;
    victim->size_bits = size_bits;
    victim->stamp = ++c->tick;
    victim->s = s;
    return s;
}

int cnvs_text_cache_font(cnvs_text_cache *__single c, void *__single font) {
    if (!c || !font) {
        return -1;
    }
    char buf[256];
    int n = cnvs_run_font_name(font, buf, (int)sizeof buf);
    if (n <= 0) {
        return -1;
    }
    // The sized model throughout: compare by (length, bytes), no str*() bridge.
    for (int i = 0; i < c->nfonts; i++) {
        if (c->font[i].len == n && memcmp(c->font[i].name, buf, (size_t)n) == 0) {
            return i;
        }
    }
    if (c->nfonts == CNVS_FONT_INTERN_N) {
        return -1;  // intern table full: those runs degrade to boundary calls
    }
    char *copy = malloc((size_t)n);
    if (!copy) {
        return -1;
    }
    memcpy(copy, buf, (size_t)n);
    c->font[c->nfonts].name = copy;
    c->font[c->nfonts].len = n;
    c->nfonts += 1;
    return c->nfonts - 1;
}

// MurmurHash3's 32-bit finalizer: avalanche the packed key across the table.
// Its multiplies wrap by design, so run them in 64-bit and truncate explicitly
// -- the same bits, but nothing for -fsanitize=integer's unsigned-wrap check
// (the debug variant) to flag.
static uint32_t mix32(uint32_t h) {
    h ^= h >> 16;
    h = (uint32_t)((uint64_t)h * 0x85EBCA6Bu);
    h ^= h >> 13;
    h = (uint32_t)((uint64_t)h * 0xC2B2AE35u);
    h ^= h >> 16;
    return h;
}

// Find `key`'s slot by linear probing: the hit, or the unused slot where it
// would insert.  NULL only when the table hasn't been built.  Inserts stop at
// CNVS_GLYPH_CACHE_N < CNVS_GLYPH_TABLE_N entries, so an unused slot always
// terminates the probe.
static cnvs_glyph_slot *__single glyph_probe(cnvs_text_cache *__single c,
                                             uint32_t key) {
    if (c->glyph_cap == 0) {
        return NULL;
    }
    uint32_t mask = (uint32_t)c->glyph_cap - 1u;
    for (uint32_t i = mix32(key) & mask;; i = (i + 1u) & mask) {
        cnvs_glyph_slot *slot = &c->glyph[i];
        if (!slot->used || slot->key == key) {
            return slot;
        }
    }
}

// One glyph's outline, through the cache when there is one: a hit replays the
// remembered canonical curves and never reaches the boundary; a miss fetches
// them (the stack-then-grow shape), walks them, and remembers them.  fid < 0
// (no cache, or the font couldn't intern) is the plain boundary path.
static void glyph_outline_cached(cnvs_text_cache *__single c, int fid,
                                 void *__single font, uint16_t glyph,
                                 float size_px, float ox, float oy,
                                 cnvs_mat to_device, float tol,
                                 cnvs_path *__single out) {
    bool cached = c && fid >= 0;
    uint32_t key = 0;
    cnvs_glyph_slot *slot = NULL;
    if (cached) {
        key = ((uint32_t)fid << 16) | (uint32_t)glyph;
        slot = glyph_probe(c, key);
        if (slot && slot->used) {
            c->glyph_hits++;
            if (slot->upem > 0.0f && slot->nverbs > 0) {
                struct glyph_place g = { .to_device = to_device, .ox = ox,
                                         .oy = oy,
                                         .scale = size_px / slot->upem };
                walk_curves(slot->verb, slot->nverbs, slot->pt, slot->npts, &g,
                            tol, out);
            }
            return;  // a cached blank: known to have no outline, nothing to add
        }
        c->glyph_misses++;
        if (c->glyph_cap == 0) {  // first miss: build the table (a failure here
            cnvs_glyph_slot *t = calloc(CNVS_GLYPH_TABLE_N, sizeof *t);
            if (t) {              // just retries on the next miss)
                c->glyph = t;
                c->glyph_cap = CNVS_GLYPH_TABLE_N;
                slot = glyph_probe(c, key);
            }
        }
    }
    // Stack buffers cover the typical glyph; a rare complex one (a dense CJK
    // ideograph, an ornate display face) takes the grow-and-refetch path below.
    enum { VSTACK = 256, PSTACK = 512 };
    cnvs_glyph_verb vstack[VSTACK];
    cnvs_vec2 pstack[PSTACK];
    int nv = 0, np = 0;
    float upem = 0.0f;
    cnvs_glyph_curves(font, glyph, vstack, VSTACK, pstack, PSTACK, &nv, &np, &upem);
    bool blank = nv <= 0 || np < 0 || upem <= 0.0f;
    cnvs_glyph_verb *vheap = NULL;
    cnvs_vec2 *pheap = NULL;
    if (!blank && (nv > VSTACK || np > PSTACK)) {
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
    }
    if (!blank) {
        struct glyph_place g = { .to_device = to_device, .ox = ox, .oy = oy,
                                 .scale = size_px / upem };
        walk_curves(vheap ? vheap : vstack, nv, pheap ? pheap : pstack, np, &g,
                    tol, out);
    }
    // Remember the result, best-effort: a full table or a failed copy just means
    // this glyph misses again next time.  Blanks are remembered too -- "no
    // outline" is itself a boundary answer (a space costs one fetch ever).
    if (!cached || !slot || slot->used || c->glyph_count >= CNVS_GLYPH_CACHE_N) {
        free(vheap);
        free(pheap);
        return;
    }
    if (blank) {
        slot->verb = NULL;
        slot->nverbs = 0;
        slot->pt = NULL;
        slot->npts = 0;
        slot->upem = 0.0f;
    } else if (vheap) {  // donate the exact-size refetch buffers: the overflow
        slot->verb = vheap;     // glyph's second fetch now happens once per
        slot->nverbs = nv;      // (font, glyph), not once per draw
        slot->pt = pheap;
        slot->npts = np;
        slot->upem = upem;
        vheap = NULL;
        pheap = NULL;
    } else {  // the common case: copy out of the stack buffers
        cnvs_glyph_verb *vc = malloc((size_t)nv * sizeof *vc);
        cnvs_vec2 *pc = malloc((size_t)(np > 0 ? np : 1) * sizeof *pc);
        if (!vc || !pc) {
            free(vc);
            free(pc);
            return;
        }
        memcpy(vc, vstack, (size_t)nv * sizeof *vc);
        memcpy(pc, pstack, (size_t)np * sizeof *pc);
        slot->verb = vc;
        slot->nverbs = nv;
        slot->pt = pc;
        slot->npts = np;
        slot->upem = upem;
    }
    slot->key = key;
    slot->used = true;
    c->glyph_count += 1;
    free(vheap);
    free(pheap);
}

void cnvs_glyph_outline(void *__single font, uint16_t glyph, float size_px,
                        float ox, float oy, cnvs_mat to_device, float tol,
                        cnvs_path *__single out) {
    glyph_outline_cached(NULL, -1, font, glyph, size_px, ox, oy, to_device, tol,
                         out);
}

float cnvs_shaped_outline(cnvs_text_cache *__single cache,
                          cnvs_shaped const *__single s, float ox, float oy,
                          cnvs_mat to_device, float tol, cnvs_path *__single out,
                          cnvs_color_glyph_fn color, void *__single ctx) {
    if (!s) {
        return 0.0f;
    }
    float pen = ox;
    for (int r = 0; r < s->nruns; r++) {
        cnvs_glyph_run run = s->run[r];  // visual-order glyphs, so advancing the pen
        bool is_color = cnvs_run_is_color(run.font);
        // One name fetch per run, not per glyph, to key the run's glyphs.
        int fid = is_color ? -1 : cnvs_text_cache_font(cache, run.font);
        for (int i = 0; i < run.count; i++) {  // left-to-right places RTL runs too
            if (is_color) {
                if (color) {  // no outline to trace: hand it over to be drawn
                    color(ctx, run.font, run.glyph[i], pen, oy);
                }
            } else {
                glyph_outline_cached(cache, fid, run.font, run.glyph[i],
                                     s->size_px, pen, oy, to_device, tol, out);
            }
            pen += run.xadv[i];
        }
    }
    return pen - ox;
}
