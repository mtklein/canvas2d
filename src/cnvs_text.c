#include "cnvs_text.h"

#include <stdlib.h>
#include <string.h>

// Checked-core consumers of a shaped run.  Every glyph and cluster access is
// bounds-checked against the counts the boundary handed over; the cluster value is
// range-checked against the source length before it is trusted as an index.

float cnvs_shaped_width(struct cnvs_shaped const *__single s) {
    if (!s) {
        return 0.0f;
    }
    float w = 0.0f;
    for (int r = 0; r < s->nruns; r++) {
        struct cnvs_glyph_run const run = s->run[r];
        for (int i = 0; i < run.count; i++) {
            w += run.xadv[i];
        }
    }
    return w;
}

// Decode one UTF-8 code point starting at byte `i` of `text` (len bytes): the
// code point comes back in *cp, the byte length of the sequence is the return
// value.  A malformed lead or truncated/continuation byte decodes as the single
// byte at `i` (cp = that byte, length 1) -- enough to keep the UTF-16 index walk
// in step; the only code point spacing inspects is U+0020 SPACE, which is plain
// ASCII, so a mis-decode never spuriously matches it.
static int utf8_decode(char const *__counted_by(len) text, int len, int i,
                       uint32_t *__single cp) {
    unsigned char const b0 = (unsigned char)text[i];
    if (b0 < 0x80) {
        *cp = b0;
        return 1;
    }
    int n;
    uint32_t c;
    if ((b0 & 0xE0u) == 0xC0u)      { n = 2; c = b0 & 0x1Fu; }
    else if ((b0 & 0xF0u) == 0xE0u) { n = 3; c = b0 & 0x0Fu; }
    else if ((b0 & 0xF8u) == 0xF0u) { n = 4; c = b0 & 0x07u; }
    else { *cp = b0; return 1; }  // a stray continuation/illegal lead byte
    if (i + n > len) {
        *cp = b0;
        return 1;  // truncated: step one byte
    }
    for (int k = 1; k < n; k++) {
        unsigned char const bk = (unsigned char)text[i + k];
        if ((bk & 0xC0u) != 0x80u) {
            *cp = b0;
            return 1;  // not a continuation byte: step one byte
        }
        c = (c << 6) | (bk & 0x3Fu);
    }
    *cp = c;
    return n;
}

// Add per-cluster letterSpacing and per-space wordSpacing into a Core-Text-shaped
// line's advances, in place, before it is interned in the cache.  A cluster is a
// maximal run of glyphs sharing one cluster[] value; the spacing rides the LAST
// glyph of each cluster (so a measure/draw sweep accumulates it once per
// cluster, at the cluster's trailing edge), matching browser measureText.
// letterSpacing adds `ls` to every cluster (the final cluster included);
// wordSpacing adds `ws` to a cluster whose source character is U+0020 SPACE.
// Advances stay positive in both ltr and rtl runs, so the same addition applies
// regardless of direction.  ls == 0 && ws == 0 is a pure no-op: it touches no
// advance and leaves the bits identical.  The word test maps a cluster's UTF-16
// index (cluster[i]) to the source code point via a UTF-8<->UTF-16 walk over
// `text`: one pass builds, for each UTF-16 unit, whether the code point that
// unit begins is a SPACE (a non-leading unit -- a surrogate pair's low half --
// is never a SPACE since SPACE is one UTF-16 unit), then each cluster reads that
// flag at its cluster[] value.
void cnvs_shaped_apply_spacing(struct cnvs_shaped *__single s,
                               char const *__counted_by(text_len) text, int text_len,
                               float ls, float ws) {
    if (!s || (ls == 0.0f && ws == 0.0f)) {
        return;  // no-op: leave every advance bit-identical
    }
    // is_space[u] == true iff the code point beginning at UTF-16 unit u is
    // U+0020.  Built from one UTF-8 walk over `text`; its length (sp_n) is 0
    // until the allocation lands, so the __counted_by pointer never holds NULL
    // with a positive count.  Word spacing needs it; letter spacing alone skips
    // it (sp_n stays 0).
    int sp_n = 0;
    bool *__counted_by(sp_n) is_space = NULL;
    if (ws != 0.0f && s->utf16s > 0) {
        bool *tmp = calloc((size_t)s->utf16s, sizeof *tmp);
        if (tmp) {
            int u = 0;  // running UTF-16 index, in step with the byte walk
            for (int i = 0; i < text_len && u < s->utf16s;) {
                uint32_t cp = 0;
                int const nb = utf8_decode(text, text_len, i, &cp);
                int const nu = cp >= 0x10000u ? 2 : 1;  // UTF-16 units this cp costs
                if (cp == 0x20u) {
                    tmp[u] = true;
                }
                i += nb;
                u += nu;
            }
            is_space = tmp;
            sp_n = s->utf16s;  // publish the count only with the pointer set
        }
    }
    for (int r = 0; r < s->nruns; r++) {
        struct cnvs_glyph_run *__single run = &s->run[r];
        int i = 0;
        while (i < run->count) {
            int32_t const cl = run->cluster[i];
            int j = i + 1;
            while (j < run->count && run->cluster[j] == cl) {
                j++;  // extend over every glyph sharing this cluster value
            }
            int const last = j - 1;  // the cluster's trailing glyph
            if (ls != 0.0f) {
                run->xadv[last] += ls;
            }
            if (cl >= 0 && cl < sp_n && is_space[cl]) {
                run->xadv[last] += ws;
            }
            i = j;
        }
    }
    free(is_space);
}

int cnvs_shaped_index_at_x(struct cnvs_shaped const *__single s, float x) {
    if (!s) {
        return -1;
    }
    float pen = 0.0f;
    for (int r = 0; r < s->nruns; r++) {
        struct cnvs_glyph_run const run = s->run[r];  // glyphs are visual-order, so a single
        for (int i = 0; i < run.count; i++) {  // left-to-right sweep works for RTL too
            if (x < pen + run.xadv[i]) {
                int32_t const c = run.cluster[i];
                if (c < 0 || c >= s->utf16s) {  // defensive: a bad cluster is not
                    return -1;                    // a valid source index
                }
                return c;
            }
            pen += run.xadv[i];
        }
    }
    return -1;
}

float cnvs_shaped_x_at_index(struct cnvs_shaped const *__single s, int index) {
    if (!s) {
        return 0.0f;
    }
    // One visual sweep tracking the glyph whose cluster START is the greatest
    // one <= index: an exact hit is its own cluster's start, and an index
    // INSIDE a cluster (a surrogate pair's low half, a ligature's interior)
    // snaps to the enclosing cluster's edge -- no glyph carries that index, so
    // without the snap the caret would fall off the end of the line.
    float pen = 0.0f, best_x = 0.0f;
    int32_t best = -1;  // greatest cluster start <= index seen so far
    for (int r = 0; r < s->nruns; r++) {
        struct cnvs_glyph_run const run = s->run[r];
        for (int i = 0; i < run.count; i++) {
            int32_t const c = run.cluster[i];
            if (c <= index && c > best) {
                best = c;
                best_x = pen;
            }
            pen += run.xadv[i];
        }
    }
    if (index >= s->utf16s) {
        return pen;  // at or past the source's end -> the caret after the last glyph
    }
    return best >= 0 ? best_x : 0.0f;
}

int cnvs_shaped_selection(struct cnvs_shaped const *__single s, int lo, int hi,
                          cnvs_xspan *__counted_by(max) out, int max) {
    if (!s || max <= 0 || hi <= lo) {
        return 0;
    }
    int n = 0;
    float pen = 0.0f, start = 0.0f;
    bool in = false;  // currently inside a selected visual run of glyphs
    for (int r = 0; r < s->nruns; r++) {
        struct cnvs_glyph_run const run = s->run[r];
        for (int i = 0; i < run.count; i++) {
            bool const sel = run.cluster[i] >= lo && run.cluster[i] < hi;
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
static int verb_pts(enum cnvs_glyph_verb v) {
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
    cnvs_vec2 const u = { g->ox + fu.x * g->scale, g->oy - fu.y * g->scale };
    return cnvs_mat_apply(g->to_device, u);
}

// Transform + flatten one glyph's canonical curves into `out`: the shared back
// half of the cached and uncached outline paths.  The curve stream is boundary
// data (fresh or remembered), so the walk stays defensive: a byte that is not a
// verb, or a verb whose points would run past the count, stops the walk cleanly
// rather than being trusted as an index.
static void walk_curves(enum cnvs_glyph_verb const *__counted_by(nv) verb, int nv,
                        cnvs_vec2 const *__counted_by(np) pt, int np,
                        struct glyph_place const *__single g, float tol,
                        struct cnvs_path *__single out) {
    int ip = 0;
    for (int iv = 0; iv < nv; iv++) {
        enum cnvs_glyph_verb const v = verb[iv];
        int const k = verb_pts(v);
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

void cnvs_text_cache_init(struct cnvs_text_cache *__single c) {
    memset(c, 0, sizeof *c);  // empty slots: NULL pointers with zero counts
}

void cnvs_text_cache_reset(struct cnvs_text_cache *__single c) {
    for (int i = 0; i < CNVS_SHAPING_CACHE_N; i++) {
        if (c->shaping[i].s) {
            cnvs_shaped_free(c->shaping[i].s);  // releases the runs' CTFontRefs too
            free(c->shaping[i].text);
            free(c->shaping[i].fam);
        }
    }
    for (int i = 0; i < c->glyph_cap; i++) {
        if (c->glyph[i].used) {
            free(c->glyph[i].verb);
            free(c->glyph[i].pt);
            free(c->glyph[i].capture);
            for (int m = 0; m < c->glyph[i].nmips; m++) {
                free(c->glyph[i].mip[m].px);
            }
            free(c->glyph[i].mip);
        }
    }
    free(c->glyph);
    for (int i = 0; i < c->nfonts; i++) {
        free(c->font[i].name);
    }
    cnvs_text_cache_init(c);  // back to the empty state, stats included
}

// The slot a fresh shaped line lands in: the first empty slot, else the
// least-recently-used one.
static struct cnvs_shaping_slot *__single shaping_lru_victim(struct cnvs_text_cache *__single c) {
    struct cnvs_shaping_slot *victim = &c->shaping[0];
    for (int i = 0; i < CNVS_SHAPING_CACHE_N; i++) {
        if (!c->shaping[i].s) {
            return &c->shaping[i];
        }
        if (c->shaping[i].last_use < victim->last_use) {
            victim = &c->shaping[i];
        }
    }
    return victim;
}

struct cnvs_shaped const *__single cnvs_text_cache_shaping(struct cnvs_text_cache *__single c,
        char const *__counted_by(name_len) name, int name_len, float size_px,
        bool rtl, float ls, float ws, int weight, bool italic,
        char const *__counted_by(len) text, int len) {
    struct cnvs_shaping_slot *__single hit = cnvs_text_cache_shaping_slot(c, name, name_len,
                                                  size_px, rtl, ls, ws, weight, italic, text, len);
    if (hit) {
        hit->last_use = ++c->tick;
        c->shaping_hits++;
        return hit->s;
    }
    c->shaping_misses++;
    // Miss.  Copy the key bytes the slot will own (text and the requested
    // family; +1 each so a zero-length key still has an allocation to own), then
    // shape through the counted boundary: (text, len) crosses as-is, so no
    // NUL-terminated copy exists anywhere on this path.  Any failure degrades to
    // nothing-to-draw.
    char *copy = malloc((size_t)len + 1);
    char *fam = malloc((size_t)name_len + 1);
    if (!copy || !fam) {
        free(copy);
        free(fam);
        return NULL;
    }
    memcpy(copy, text, (size_t)len);
    memcpy(fam, name, (size_t)name_len);
    struct cnvs_shaped *__single s = cnvs_shape_text(name, name_len, size_px, rtl,
                                                     weight, italic, text, len);
    if (!s) {
        free(copy);
        free(fam);
        return NULL;  // boundary failure: nothing to cache, nothing to draw
    }
    // Bake letterSpacing/wordSpacing into the advances before the line is
    // interned, so a cache hit (live or replayed) carries them and measureText
    // and the draw walk both reflect them.  A no-op when both are 0.
    cnvs_shaped_apply_spacing(s, text, len, ls, ws);
    // Insert into an empty slot, else evict the least-recently-used one.  This
    // cannot invalidate a borrowed line: call sites take one lookup per op and
    // never nest, so no borrow is alive across an insert.
    uint32_t size_bits = 0, ls_bits = 0, ws_bits = 0;
    memcpy(&size_bits, &size_px, sizeof size_bits);
    memcpy(&ls_bits, &ls, sizeof ls_bits);
    memcpy(&ws_bits, &ws, sizeof ws_bits);
    struct cnvs_shaping_slot *victim = shaping_lru_victim(c);
    if (victim->s) {
        cnvs_shaped_free(victim->s);
        free(victim->text);
        free(victim->fam);
    }
    victim->text = copy;
    victim->len = len;
    victim->fam = fam;
    victim->fam_len = name_len;
    victim->size_bits = size_bits;
    victim->ls_bits = ls_bits;
    victim->ws_bits = ws_bits;
    victim->weight = weight;
    victim->italic = italic;
    victim->rtl = rtl;
    victim->last_use = ++c->tick;
    victim->s = s;
    victim->emitted = false;  // a fresh line (even in a reused slot) is not yet
    return s;                 // serialized into any active recording
}

int cnvs_text_cache_intern(struct cnvs_text_cache *__single c,
                           char const *__counted_by(len) name, int len,
                           int weight, bool italic) {
    if (!c || len <= 0) {
        return -1;
    }
    // The sized model throughout: compare by (length, bytes, weight, style), no
    // str*() bridge.  weight/style are part of the key so a synthesized bold/
    // italic (same resolved name as regular) gets a distinct id.
    for (int i = 0; i < c->nfonts; i++) {
        if (c->font[i].len == len && c->font[i].weight == weight &&
            c->font[i].italic == italic &&
            memcmp(c->font[i].name, name, (size_t)len) == 0) {
            return i;
        }
    }
    if (c->nfonts == CNVS_FONT_INTERN_N) {
        return -1;  // intern table full: those runs degrade to boundary calls
    }
    char *copy = malloc((size_t)len);
    if (!copy) {
        return -1;
    }
    memcpy(copy, name, (size_t)len);
    c->font[c->nfonts].name = copy;
    c->font[c->nfonts].len = len;
    c->font[c->nfonts].weight = weight;
    c->font[c->nfonts].italic = italic;
    c->nfonts += 1;
    return c->nfonts - 1;
}

int cnvs_text_cache_font(struct cnvs_text_cache *__single c, void *__single font,
                         int weight, bool italic) {
    if (!c || !font) {
        return -1;
    }
    char buf[256];
    int const n = cnvs_run_font_name(font, buf, (int)sizeof buf);
    if (n <= 0) {
        return -1;
    }
    int const fid = cnvs_text_cache_intern(c, buf, n, weight, italic);
    // A live handle is the chance to record the name's vmetrics (one boundary
    // fetch per font ever) -- the serialized `font` block reads them back.
    if (fid >= 0 && !c->font[fid].has_vm) {
        float a1 = 0.0f, d1 = 0.0f;
        cnvs_run_vmetrics(font, &a1, &d1);
        cnvs_text_cache_set_vmetrics(c, fid, a1, d1);
    }
    return fid;
}

void cnvs_text_cache_set_vmetrics(struct cnvs_text_cache *__single c, int fid,
                                  float asc1, float desc1) {
    if (!c || fid < 0 || fid >= c->nfonts || c->font[fid].has_vm) {
        return;  // first value wins: live and replayed values agree by design
    }
    c->font[fid].asc1 = asc1;
    c->font[fid].desc1 = desc1;
    c->font[fid].has_vm = true;
}

bool cnvs_text_cache_get_vmetrics(struct cnvs_text_cache *__single c, int fid,
                                  float *__single asc1, float *__single desc1) {
    if (!c || fid < 0 || fid >= c->nfonts || !c->font[fid].has_vm) {
        return false;
    }
    *asc1 = c->font[fid].asc1;
    *desc1 = c->font[fid].desc1;
    return true;
}

// MurmurHash3's 32-bit finalizer: avalanche the packed key across the table.
// Its multiplies wrap by design; __builtin_mul_overflow with the overflow flag
// ignored is the sanctioned spelling of that intent -- the builtin is *defined*
// as wrapping, so -fsanitize=integer's unsigned-wrap check (the debug variant)
// leaves it alone, and the call site says "deliberate" without width games.
static uint32_t mix32(uint32_t h) {
    h ^= h >> 16;
    (void)__builtin_mul_overflow(h, 0x85EBCA6Bu, &h);
    h ^= h >> 13;
    (void)__builtin_mul_overflow(h, 0xC2B2AE35u, &h);
    h ^= h >> 16;
    return h;
}

// Find `key`'s slot by linear probing: the hit, or the unused slot where it
// would insert.  NULL only when the table hasn't been built.  Inserts stop at
// CNVS_GLYPH_CACHE_N < CNVS_GLYPH_TABLE_N entries, so an unused slot always
// terminates the probe.
static struct cnvs_glyph_slot *__single glyph_probe(struct cnvs_text_cache *__single c,
                                             uint32_t key) {
    if (c->glyph_cap == 0) {
        return NULL;
    }
    uint32_t const mask = (uint32_t)c->glyph_cap - 1u;
    for (uint32_t i = mix32(key) & mask;; i = (i + 1u) & mask) {
        struct cnvs_glyph_slot *slot = &c->glyph[i];
        if (!slot->used || slot->key == key) {
            return slot;
        }
    }
}

// Build the glyph table on the first miss or insert that needs it; false when
// it (still) doesn't exist -- a failed build just retries on the next call.
static bool glyph_table_ensure(struct cnvs_text_cache *__single c) {
    if (c->glyph_cap == 0) {
        struct cnvs_glyph_slot *t = calloc(CNVS_GLYPH_TABLE_N, sizeof *t);
        if (t) {
            c->glyph = t;
            c->glyph_cap = CNVS_GLYPH_TABLE_N;
        }
    }
    return c->glyph_cap != 0;
}

struct cnvs_glyph_slot *__single cnvs_text_cache_glyph(struct cnvs_text_cache *__single c,
        int fid, void *__single font, uint16_t glyph, float size_px) {
    if (!c || fid < 0) {
        return NULL;
    }
    uint32_t key = ((uint32_t)fid << 16) | (uint32_t)glyph;
    struct cnvs_glyph_slot *slot = glyph_probe(c, key);
    if (slot && slot->used) {
        c->glyph_hits++;
        return slot;
    }
    if (!font) {
        return NULL;  // nothing to fetch with (a replay-built run whose glyph
    }                 // block was missing): degrade, don't poison the cache
    c->glyph_misses++;
    if (glyph_table_ensure(c)) {
        slot = glyph_probe(c, key);
    }
    if (!slot || c->glyph_count >= CNVS_GLYPH_CACHE_N) {
        return NULL;  // nowhere to remember the fetch: plain boundary path
    }
    // Stack buffers cover the typical glyph; a rare complex one (a dense CJK
    // ideograph, an ornate display face) takes the grow-and-refetch path below.
    enum { VSTACK = 256, PSTACK = 512 };
    enum cnvs_glyph_verb vstack[VSTACK];
    cnvs_vec2 pstack[PSTACK];
    int nv = 0, np = 0;
    float upem = 0.0f;
    cnvs_glyph_curves(font, glyph, vstack, VSTACK, pstack, PSTACK, &nv, &np, &upem);
    bool const blank = nv <= 0 || np < 0 || upem <= 0.0f;
    if (blank) {  // remembered too -- "no outline" is itself a boundary answer
        slot->verb = NULL;     // (a space costs one fetch ever)
        slot->nverbs = 0;
        slot->pt = NULL;
        slot->npts = 0;
        slot->upem = 0.0f;
    } else if (nv > VSTACK || np > PSTACK) {
        // Overflow: exact-size heap buffers, refetched once and donated to the
        // slot -- the complex glyph's second fetch happens once per (font,
        // glyph), not once per draw.
        enum cnvs_glyph_verb *vheap = malloc((size_t)nv * sizeof *vheap);
        cnvs_vec2 *pheap = malloc((size_t)(np > 0 ? np : 1) * sizeof *pheap);
        if (!vheap || !pheap) {
            free(vheap);
            free(pheap);
            return NULL;  // OOM: the caller takes the plain boundary path
        }
        int nv2 = 0, np2 = 0;
        cnvs_glyph_curves(font, glyph, vheap, nv, pheap, np, &nv2, &np2, &upem);
        if (nv2 < nv) { nv = nv2; }  // defensive: keep only what was both
        if (np2 < np) { np = np2; }  // reported and allocated
        slot->verb = vheap;
        slot->nverbs = nv;
        slot->pt = pheap;
        slot->npts = np;
        slot->upem = upem;
    } else {  // the common case: copy out of the stack buffers
        enum cnvs_glyph_verb *vc = malloc((size_t)nv * sizeof *vc);
        cnvs_vec2 *pc = malloc((size_t)(np > 0 ? np : 1) * sizeof *pc);
        if (!vc || !pc) {
            free(vc);
            free(pc);
            return NULL;
        }
        memcpy(vc, vstack, (size_t)nv * sizeof *vc);
        memcpy(pc, pstack, (size_t)np * sizeof *pc);
        slot->verb = vc;
        slot->nverbs = nv;
        slot->pt = pc;
        slot->npts = np;
        slot->upem = upem;
    }
    // The ink box rides along in the same entry, normalized to font units like
    // the curves (cnvs_glyph_bounds reports px at the size `font` is built at),
    // so metrics replay from the cache too.  A blank glyph has no ink.
    slot->ink_x0 = 0.0f;
    slot->ink_y0 = 0.0f;
    slot->ink_x1 = 0.0f;
    slot->ink_y1 = 0.0f;
    if (!blank && size_px > 0.0f) {
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        cnvs_glyph_bounds(font, glyph, &x0, &y0, &x1, &y1);
        float const k = upem / size_px;
        slot->ink_x0 = x0 * k;
        slot->ink_y0 = y0 * k;
        slot->ink_x1 = x1 * k;
        slot->ink_y1 = y1 * k;
    }
    slot->key = key;
    slot->used = true;
    slot->emitted = false;
    c->glyph_count += 1;
    return slot;
}

void cnvs_text_cache_put_glyph(struct cnvs_text_cache *__single c, int fid,
        uint16_t glyph, enum cnvs_glyph_verb *__counted_by(nverbs) verb, int nverbs,
        cnvs_vec2 *__counted_by(npts) pt, int npts, float upem,
        float ink_x0, float ink_y0, float ink_x1, float ink_y1) {
    if (!c || fid < 0 || nverbs < 0 || npts < 0) {
        free(verb);
        free(pt);
        return;
    }
    (void)glyph_table_ensure(c);  // first insert builds it, like a live miss
    uint32_t key = ((uint32_t)fid << 16) | (uint32_t)glyph;
    struct cnvs_glyph_slot *slot = glyph_probe(c, key);
    if (!slot || slot->used || c->glyph_count >= CNVS_GLYPH_CACHE_N) {
        free(verb);  // an existing entry wins (replay onto a warm canvas), and
        free(pt);    // a full table is the usual best-effort degradation
        return;
    }
    slot->verb = verb;
    slot->nverbs = nverbs;
    slot->pt = pt;
    slot->npts = npts;
    slot->upem = upem;
    slot->ink_x0 = ink_x0;
    slot->ink_y0 = ink_y0;
    slot->ink_x1 = ink_x1;
    slot->ink_y1 = ink_y1;
    slot->key = key;
    slot->used = true;
    slot->emitted = false;
    c->glyph_count += 1;
}

// ---------------------------------------------------------------------------
// Color (emoji) glyph captures + the derived mip pyramid.  The capture is the
// canonical form -- one premultiplied RGBA8 render per (font name, glyph id)
// at CNVS_CAPTURE_EM px to the em, fetched through the boundary once ever and
// sampled from then on.  The pyramid is checked-C derived data: repeated 2x2
// box halving of the capture, rebuilt on demand, never serialized.

struct cnvs_glyph_slot *__single cnvs_text_cache_color(struct cnvs_text_cache *__single c,
        int fid, void *__single font, uint16_t glyph) {
    if (!c || fid < 0) {
        return NULL;
    }
    uint32_t key = ((uint32_t)fid << 16) | (uint32_t)glyph;
    struct cnvs_glyph_slot *slot = glyph_probe(c, key);
    if (slot && slot->used) {
        c->glyph_hits++;
        return slot;
    }
    if (!font) {
        return NULL;  // nothing to rasterize with (a replay-built run whose
    }                 // bitmap block was missing): degrade, don't poison
    c->glyph_misses++;
    if (glyph_table_ensure(c)) {
        slot = glyph_probe(c, key);
    }
    if (!slot || c->glyph_count >= CNVS_GLYPH_CACHE_N) {
        return NULL;  // nowhere to remember the capture: per-draw boundary path
    }
    // The one boundary crossing per color glyph: a sized handle at the capture
    // em, its ink box (already in capture px), and one draw with the ink box's
    // bottom-left pinned to the buffer's bottom-left corner, no margin (the
    // strike box CT reports already pads the ink, and the sampler clamps to
    // edge).
    void *__single big = cnvs_font_resized(font, (float)CNVS_CAPTURE_EM);
    if (!big) {
        return NULL;
    }
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    cnvs_glyph_bounds(big, glyph, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0) {  // blank color glyph: remembered as no-capture,
        cnvs_font_release(big);  // so it costs one boundary query ever
        slot->key = key;
        slot->used = true;
        slot->emitted = false;
        c->glyph_count += 1;
        return slot;
    }
    int const w = CNVS_CAPTURE_EM, h = CNVS_CAPTURE_EM;
    int const len = w * h * 4;
    uint8_t *px = calloc((size_t)len, 1);  // transparent ground
    if (!px) {
        cnvs_font_release(big);
        return NULL;  // OOM: the caller takes the per-draw boundary path
    }
    cnvs_glyph_draw(big, glyph, -x0, -y0, px, w, h);
    cnvs_font_release(big);
    slot->capture = px;
    slot->capture_size = len;
    slot->capture_w = w;
    slot->capture_h = h;
    slot->ink_x0 = x0;
    slot->ink_y0 = y0;
    slot->ink_x1 = x1;
    slot->ink_y1 = y1;
    slot->key = key;
    slot->used = true;
    slot->emitted = false;
    c->glyph_count += 1;
    return slot;
}

void cnvs_text_cache_put_capture(struct cnvs_text_cache *__single c, int fid,
        uint16_t glyph, uint8_t *__counted_by(len) px, int len, int w, int h,
        float ink_x0, float ink_y0, float ink_x1, float ink_y1) {
    if (!c || fid < 0 || !px || w <= 0 || h <= 0 || len != w * h * 4) {
        free(px);
        return;
    }
    (void)glyph_table_ensure(c);  // first insert builds it, like a live miss
    uint32_t key = ((uint32_t)fid << 16) | (uint32_t)glyph;
    struct cnvs_glyph_slot *slot = glyph_probe(c, key);
    if (!slot || slot->used || c->glyph_count >= CNVS_GLYPH_CACHE_N) {
        free(px);  // an existing entry wins (replay onto a warm canvas), and
        return;    // a full table is the usual best-effort degradation
    }
    slot->capture = px;
    slot->capture_size = len;
    slot->capture_w = w;
    slot->capture_h = h;
    slot->ink_x0 = ink_x0;
    slot->ink_y0 = ink_y0;
    slot->ink_x1 = ink_x1;
    slot->ink_y1 = ink_y1;
    slot->key = key;
    slot->used = true;
    slot->emitted = false;
    c->glyph_count += 1;
}

void cnvs_mip_halve(uint8_t const *__counted_by(sw * sh * 4) src, int sw, int sh,
                    uint8_t *__counted_by(dw * dh * 4) dst, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw != (sw + 1) / 2 || dh != (sh + 1) / 2) {
        return;  // not a halving step: leave dst alone (defensive contract)
    }
    for (int y = 0; y < dh; y++) {
        int const y0 = 2 * y;
        int const y1 = y0 + 1 < sh ? y0 + 1 : y0;  // odd sh: replicate the edge
        for (int x = 0; x < dw; x++) {
            int const x0 = 2 * x;
            int const x1 = x0 + 1 < sw ? x0 + 1 : x0;
            for (int k = 0; k < 4; k++) {
                int const s = src[(y0 * sw + x0) * 4 + k]
                            + src[(y0 * sw + x1) * 4 + k]
                            + src[(y1 * sw + x0) * 4 + k]
                            + src[(y1 * sw + x1) * 4 + k];
                // One rounding shared by all four channels, so sum(r) <= sum(a)
                // implies the halved r <= the halved a: premul survives exactly.
                dst[(y * dw + x) * 4 + k] = (uint8_t)((s + 2) >> 2);
            }
        }
    }
}

// Build the whole pyramid under `slot`'s capture: ceil-halve until 1x1.  Best
// effort -- a failed allocation keeps the prefix built so far, and selection
// then degrades to the coarsest available level (worst case the capture).
static void build_mips(struct cnvs_glyph_slot *__single slot) {
    if (slot->nmips > 0 || slot->capture_w <= 0) {
        return;  // built (even partially), or nothing to derive from
    }
    int n = 0;
    for (int w = slot->capture_w, h = slot->capture_h; w > 1 || h > 1;) {
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        n++;
    }
    if (n == 0) {
        return;  // a 1x1 capture is its own pyramid
    }
    cnvs_mip *m = calloc((size_t)n, sizeof *m);
    if (!m) {
        return;
    }
    uint8_t *prev = slot->capture;
    int pw = slot->capture_w, ph = slot->capture_h;
    int built = 0;
    for (int i = 0; i < n; i++) {
        int const lw = (pw + 1) / 2, lh = (ph + 1) / 2;
        int const llen = lw * lh * 4;
        uint8_t *px = malloc((size_t)llen);
        if (!px) {
            break;  // keep the prefix
        }
        cnvs_mip_halve(prev, pw, ph, px, lw, lh);
        m[i].px = px;
        m[i].len = llen;
        m[i].w = lw;
        m[i].h = lh;
        prev = px;
        pw = lw;
        ph = lh;
        built++;
    }
    if (built == 0) {
        free(m);
        return;
    }
    slot->mip = m;
    slot->nmips = built;
}

cnvs_mip cnvs_glyph_mip(struct cnvs_glyph_slot *__single slot, float footprint) {
    cnvs_mip pick = { .px = NULL, .len = 0, .w = 0, .h = 0 };
    if (!slot || slot->capture_w <= 0) {
        return pick;
    }
    build_mips(slot);
    float const f = footprint > 1.0f ? footprint : 1.0f;  // <=0/NaN: smallest
    // The capture is level 0 (always available); each mip is half the one
    // before.  Walk down while the next level still covers the footprint in
    // both dimensions, so the level sampled is the smallest one >= footprint.
    pick.px = slot->capture;
    pick.len = slot->capture_size;
    pick.w = slot->capture_w;
    pick.h = slot->capture_h;
    for (int i = 0; i < slot->nmips; i++) {
        if ((float)slot->mip[i].w < f || (float)slot->mip[i].h < f) {
            break;
        }
        pick = slot->mip[i];
    }
    return pick;
}

float cnvs_glyph_mip_pair(struct cnvs_glyph_slot *__single slot, float footprint,
                          cnvs_mip *__single fine, cnvs_mip *__single coarse) {
    *fine = (cnvs_mip){ .px = NULL, .len = 0, .w = 0, .h = 0 };
    *coarse = *fine;
    if (!slot || slot->capture_w <= 0) {
        return 0.0f;
    }
    build_mips(slot);
    *fine = (cnvs_mip){ .px = slot->capture, .len = slot->capture_size,
                        .w = slot->capture_w, .h = slot->capture_h };
    *coarse = *fine;
    // The footprint in ratio form: minification relative to the capture's
    // smaller dim (the one the single-level rule's both-dims walk keys on).
    // Not minifying -- or a degenerate footprint that lands the ratio at
    // NaN/negative -- is the capture alone.
    float const cap = (float)(slot->capture_w < slot->capture_h
                                  ? slot->capture_w : slot->capture_h);
    float const f = cap / footprint;
    if (!(f > 1.0f)) {
        return 0.0f;
    }
    // Doubling finds the floor level (capped at the pyramid's last), exact
    // float arithmetic the blend.
    int need = 0;
    float scale = 1.0f;
    while (scale * 2.0f <= f && need < slot->nmips) {
        scale *= 2.0f;
        need++;
    }
    if (need > 0) {
        *fine = slot->mip[need - 1];  // level `need`; level 0 is the capture
    }
    if (need < slot->nmips) {
        *coarse = slot->mip[need];
    } else {
        *coarse = *fine;  // the floor: nothing further to blend toward
        return 0.0f;
    }
    float const t = (f - scale) / scale;
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

struct cnvs_shaping_slot *__single cnvs_text_cache_shaping_slot(struct cnvs_text_cache *__single c,
        char const *__counted_by(name_len) name, int name_len, float size_px,
        bool rtl, float ls, float ws, int weight, bool italic,
        char const *__counted_by(len) text, int len) {
    if (!c || len < 0 || name_len < 0) {
        return NULL;
    }
    uint32_t size_bits = 0, ls_bits = 0, ws_bits = 0;
    memcpy(&size_bits, &size_px, sizeof size_bits);
    memcpy(&ls_bits, &ls, sizeof ls_bits);
    memcpy(&ws_bits, &ws, sizeof ws_bits);
    for (int i = 0; i < CNVS_SHAPING_CACHE_N; i++) {
        struct cnvs_shaping_slot *slot = &c->shaping[i];
        if (slot->s && slot->size_bits == size_bits && slot->rtl == rtl &&
            slot->ls_bits == ls_bits && slot->ws_bits == ws_bits &&
            slot->weight == weight && slot->italic == italic &&
            slot->fam_len == name_len &&
            memcmp(slot->fam, name, (size_t)name_len) == 0 &&
            slot->len == len && memcmp(slot->text, text, (size_t)len) == 0) {
            return slot;
        }
    }
    return NULL;
}

void cnvs_text_cache_put_shaping(struct cnvs_text_cache *__single c,
        char const *__counted_by(name_len) name, int name_len, float size_px,
        bool rtl, float ls, float ws, int weight, bool italic,
        char const *__counted_by(len) text, int len,
        struct cnvs_shaped *__single s) {
    if (!c || !s || len < 0 || name_len < 0) {
        cnvs_shaped_free(s);
        return;
    }
    char *copy = malloc((size_t)len + 1);        // +1 each: a zero-length key
    char *fam = malloc((size_t)name_len + 1);    // still needs an allocation to own
    if (!copy || !fam) {
        free(copy);
        free(fam);
        cnvs_shaped_free(s);  // can't key it: the usual best-effort degradation
        return;
    }
    memcpy(copy, text, (size_t)len);  // key bytes only: the slot's text is
    memcpy(fam, name, (size_t)name_len);  // __counted_by(len), no NUL contract
    // Replace an existing entry for the key (a re-recorded shape block after
    // the first copy was evicted), else fill an empty slot or evict the LRU --
    // the same victim scan as a live insert.
    struct cnvs_shaping_slot *victim = cnvs_text_cache_shaping_slot(c, name, name_len,
                                              size_px, rtl, ls, ws, weight, italic, text, len);
    if (!victim) {
        victim = shaping_lru_victim(c);
    }
    if (victim->s) {
        cnvs_shaped_free(victim->s);
        free(victim->text);
        free(victim->fam);
    }
    uint32_t size_bits = 0, ls_bits = 0, ws_bits = 0;
    memcpy(&size_bits, &size_px, sizeof size_bits);
    memcpy(&ls_bits, &ls, sizeof ls_bits);
    memcpy(&ws_bits, &ws, sizeof ws_bits);
    victim->text = copy;
    victim->len = len;
    victim->fam = fam;
    victim->fam_len = name_len;
    victim->size_bits = size_bits;
    victim->ls_bits = ls_bits;
    victim->ws_bits = ws_bits;
    victim->weight = weight;
    victim->italic = italic;
    victim->rtl = rtl;
    victim->last_use = ++c->tick;
    victim->s = s;
    victim->emitted = false;
}

void cnvs_text_cache_unmark(struct cnvs_text_cache *__single c) {
    if (!c) {
        return;
    }
    for (int i = 0; i < CNVS_SHAPING_CACHE_N; i++) {
        c->shaping[i].emitted = false;
    }
    for (int i = 0; i < c->glyph_cap; i++) {
        c->glyph[i].emitted = false;
    }
    for (int i = 0; i < c->nfonts; i++) {
        c->font[i].emitted = false;
    }
}

// The uncached path: fetch one glyph's canonical font-unit curves from the
// boundary and run the same checked transform/flatten a cache hit replays --
// the degradation glyph_outline_cached takes when the cache can't serve.
static void cnvs_glyph_outline(void *__single font, uint16_t glyph, float size_px,
                               float ox, float oy, cnvs_mat to_device, float tol,
                               struct cnvs_path *__single out) {
    // Stack buffers cover the typical glyph; a rare complex one takes the
    // grow-and-refetch path (cnvs_glyph_curves reports the true counts).
    enum { VSTACK = 256, PSTACK = 512 };
    enum cnvs_glyph_verb vstack[VSTACK];
    cnvs_vec2 pstack[PSTACK];
    int nv = 0, np = 0;
    float upem = 0.0f;
    cnvs_glyph_curves(font, glyph, vstack, VSTACK, pstack, PSTACK, &nv, &np, &upem);
    if (nv <= 0 || np < 0 || upem <= 0.0f) {
        return;  // blank or color glyph: no outline
    }
    enum cnvs_glyph_verb *vheap = NULL;
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
    }
    struct glyph_place g = { .to_device = to_device, .ox = ox, .oy = oy,
                             .scale = size_px / upem };
    walk_curves(vheap ? vheap : vstack, nv, pheap ? pheap : pstack, np, &g, tol,
                out);
    free(vheap);
    free(pheap);
}

// One glyph's outline, through the cache when it can serve: a hit (or a fresh
// fetch the cache just remembered) replays the canonical curves through the
// checked transform/flatten.  When the cache can't serve -- no cache, the font
// couldn't intern, a full table, OOM -- the plain boundary path (the uncached
// cnvs_glyph_outline) fetches and walks without remembering.  A replay-built
// run (font == NULL) whose glyph block is missing has nothing to draw and
// contributes only its advance.
static void glyph_outline_cached(struct cnvs_text_cache *__single c, int fid,
                                 void *__single font, uint16_t glyph,
                                 float size_px, float ox, float oy,
                                 cnvs_mat to_device, float tol,
                                 struct cnvs_path *__single out) {
    struct cnvs_glyph_slot *slot = cnvs_text_cache_glyph(c, fid, font, glyph, size_px);
    if (slot) {
        if (slot->upem > 0.0f && slot->nverbs > 0) {
            struct glyph_place g = { .to_device = to_device, .ox = ox, .oy = oy,
                                     .scale = size_px / slot->upem };
            walk_curves(slot->verb, slot->nverbs, slot->pt, slot->npts, &g, tol,
                        out);
        }
        return;  // a cached blank: known to have no outline, nothing to add
    }
    if (font) {
        cnvs_glyph_outline(font, glyph, size_px, ox, oy, to_device, tol, out);
    }
}

float cnvs_shaped_outline(struct cnvs_text_cache *__single cache,
                          struct cnvs_shaped const *__single s, float ox, float oy,
                          cnvs_mat to_device, float tol, struct cnvs_path *__single out,
                          cnvs_color_glyph_fn color, void *__single ctx) {
    if (!s) {
        return 0.0f;
    }
    float pen = ox;
    for (int r = 0; r < s->nruns; r++) {
        struct cnvs_glyph_run const run = s->run[r];  // visual-order glyphs, so advancing the pen
        // The glyph key (color runs included -- captures key by name too): a
        // replay-built run carries its interned id; a live run resolves through
        // one name fetch per run, not per glyph.  The line's weight/style join
        // the intern key so a synthesized bold/italic doesn't alias the regular
        // face (which it shares a resolved name with).
        int fid = run.name_id >= 0 ? run.name_id
                                   : cnvs_text_cache_font(cache, run.font, s->weight, s->italic);
        for (int i = 0; i < run.count; i++) {  // left-to-right places RTL runs too
            if (run.is_color) {
                if (color) {  // no outline to trace: hand it over to be drawn
                    color(ctx, fid, run.font, run.glyph[i], pen, oy);
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

// Full TextMetrics for a shaped line (the header has the contract).  The ink
// walk reads each glyph's canonical box from the cache entry the draw walks
// share -- populated through the boundary on a miss -- and scales it to
// size_px in the same float math live and replayed canvases both run, so a
// recorded program measures bit-identically after replay.  Outline glyphs
// scale their font-unit box by size_px/upem; color (emoji) glyphs scale their
// capture-px box by size_px/CNVS_CAPTURE_EM.  When the cache can't serve, a
// run that still has its font handle measures through a live
// cnvs_glyph_bounds, and a handle-less run's glyph adds no ink.
void cnvs_shaped_metrics(struct cnvs_text_cache *__single cache,
                         struct cnvs_shaped const *__single s, float size_px,
                         float ascent_px, float descent_px,
                         cnvs_text_metrics *__single m) {
    memset(m, 0, sizeof *m);
    m->font_ascent = ascent_px;
    m->font_descent = descent_px;
    // Split the em square (height == size) by the ascent/descent ratio.
    double const denom = (double)ascent_px + (double)descent_px;
    double em_asc = denom > 0.0 ? (double)size_px * (double)ascent_px / denom
                                : (double)size_px;
    m->em_ascent = (float)em_asc;
    m->em_descent = (float)((double)size_px - em_asc);
    m->alphabetic_baseline = 0.0f;
    m->hanging_baseline = ascent_px;        // ~top of the ascenders
    m->ideographic_baseline = -descent_px;  // ~bottom of the descenders
    if (!s) {
        return;
    }
    // Walk the shaped runs, summing advances and unioning each glyph's tight
    // box (y up, baseline at 0) offset by the running pen.
    float pen = 0.0f;
    bool any = false;
    float minx = 0.0f, maxx = 0.0f, miny = 0.0f, maxy = 0.0f;
    for (int r = 0; r < s->nruns; r++) {
        struct cnvs_glyph_run const run = s->run[r];
        int fid = run.name_id >= 0 ? run.name_id
                                   : cnvs_text_cache_font(cache, run.font, s->weight, s->italic);
        for (int i = 0; i < run.count; i++) {
            float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
            struct cnvs_glyph_slot *slot = run.is_color
                ? cnvs_text_cache_color(cache, fid, run.font, run.glyph[i])
                : cnvs_text_cache_glyph(cache, fid, run.font, run.glyph[i],
                                        size_px);
            if (slot) {
                float k = 0.0f;  // a blank glyph's box stays all-zero
                if (run.is_color && slot->capture_w > 0) {
                    k = size_px / (float)CNVS_CAPTURE_EM;
                } else if (!run.is_color && slot->upem > 0.0f) {
                    k = size_px / slot->upem;
                }
                if (k > 0.0f) {
                    x0 = slot->ink_x0 * k;
                    y0 = slot->ink_y0 * k;
                    x1 = slot->ink_x1 * k;
                    y1 = slot->ink_y1 * k;
                }
            } else if (run.font) {
                cnvs_glyph_bounds(run.font, run.glyph[i], &x0, &y0, &x1, &y1);
            }
            if (x1 > x0 && y1 > y0) {
                float const gx0 = pen + x0;
                float const gx1 = pen + x1;
                if (!any) {
                    minx = gx0; maxx = gx1; miny = y0; maxy = y1;
                    any = true;
                } else {
                    if (gx0 < minx) { minx = gx0; }
                    if (gx1 > maxx) { maxx = gx1; }
                    if (y0 < miny)  { miny = y0; }
                    if (y1 > maxy)  { maxy = y1; }
                }
            }
            pen += run.xadv[i];
        }
    }
    m->width = pen;
    m->actual_left = any ? -minx : 0.0f;
    m->actual_right = any ? maxx : 0.0f;
    m->actual_ascent = any ? maxy : 0.0f;
    m->actual_descent = any ? -miny : 0.0f;
}
