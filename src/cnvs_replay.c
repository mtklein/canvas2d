// Text canvas-program parser + replayer (the cnvs_replay.h core, and
// canvas_replay_from()).  Parses untrusted text by index over a __counted_by
// buffer -- the bounds-safe-parsing exercise -- and dispatches to the public
// canvas_* API.  No forges and no __null_terminated: numbers are parsed in place
// by index (no strtof), and the text tail is passed as a slice to the
// length-counted canvas_*_text_n; the parser stays entirely in the indexable
// world.  (docs/bounds-safety.md walks through what that took.)

#include "cnvs_replay.h"

#include "canvas.h"
#include "cnvs_text.h"

#include <math.h>
#include <ptrcheck.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPLAY_LINE_MAX 65536u       // over-long line -> reject (DoS guard; a
                                     // glyph block line carries one outline's
                                     // whole curve list -- a dense CJK glyph
                                     // runs ~10-20 KB -- and a bitmap block's
                                     // `bits` lines carry 16 KiB of base64)
#define REPLAY_FILE_MAX (64u << 20)  // 64 MiB file cap
#define REPLAY_DASH_MAX 64           // max dash segments from one line
#define REPLAY_RUNS_MAX 1024         // run lines one shape block may declare
#define REPLAY_BITMAP_DIM_MAX 512    // capture dims cap: bounds a bitmap
                                     // block's allocation at 1 MiB (the
                                     // recorder writes CNVS_CAPTURE_EM = 160)

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

// Next whitespace-delimited token within [*jp, le): sets [*ts, *ts+*tlen) and
// advances *jp past it.  false if only whitespace remains.
static bool read_token(char const *__counted_by(le) data, size_t le,
                       size_t *__single jp, size_t *__single ts, size_t *__single tlen) {
    size_t j = *jp;
    while (j < le && is_ws(data[j])) { j++; }
    if (j >= le) { *jp = j; return false; }
    size_t start = j;
    while (j < le && !is_ws(data[j])) { j++; }
    *ts = start;
    *tlen = j - start;
    *jp = j;
    return true;
}

// Compare token data[ts, ts+tlen) to a literal.  `lit` is walked by pointer
// (a __null_terminated pointer can't be subscripted), `data` indexed normally.
static bool tok_eq(char const *__counted_by(le) data, size_t le, size_t ts,
                   size_t tlen, char const *__null_terminated lit) {
    if (ts + tlen > le) {  // always false for tokens from read_token; bounds the
        return false;      // unsafe/fuzz build too, where __counted_by is absent
    }
    for (size_t k = 0; k < tlen; k++) {
        char c = *lit;
        if (c == '\0' || data[ts + k] != c) {
            return false;
        }
        lit++;
    }
    return *lit == '\0';  // exact-length match: literal ends right here too
}

// Exact powers of ten: 10^k for k <= 22 is exactly representable in a double
// (5^22 < 2^53), so each scaling step in read_float is one correctly-rounded
// multiply or divide by an exact constant.
static double const k_pow10[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

// Next token parsed as a float, in place by index -- no strtof, no
// __null_terminated, no forge.  Stricter than strtof: the whole token must be a
// number (sign? digits, optional .fraction, optional e[+-]exp).
//
// EXACT for everything the recorder emits: a %.9g-printed float32 has a <=
// 9-digit mantissa (exact in the double accumulator) and a base-10 exponent
// within +/-53, so the table-stepped scaling below costs at most three
// roundings (~2^-51 relative) -- orders of magnitude inside the margin nine
// significant digits leave around any float32, so the (float) conversion lands
// on the identical value (record -> replay round-trips bit for bit;
// test_record_text sweeps this).  Hostile exponents keep the old clamping
// posture: the magnitude saturates and the value flushes to +/-inf or 0.
static bool read_float(char const *__counted_by(le) data, size_t le,
                       size_t *__single jp, float *__single out) {
    size_t ts, tlen;
    if (!read_token(data, le, jp, &ts, &tlen) || tlen == 0) {
        return false;
    }
    size_t i = ts, te = ts + tlen;
    bool neg = false;
    if (i < te && (data[i] == '+' || data[i] == '-')) { neg = data[i] == '-'; i++; }
    double mant = 0.0;
    int fexp = 0;
    bool any = false;
    while (i < te && data[i] >= '0' && data[i] <= '9') {
        mant = mant * 10.0 + (data[i] - '0'); i++; any = true;
    }
    if (i < te && data[i] == '.') {
        i++;
        while (i < te && data[i] >= '0' && data[i] <= '9') {
            mant = mant * 10.0 + (data[i] - '0'); fexp--; i++; any = true;
        }
    }
    if (!any) {
        return false;  // ".", "+", "e3" -- no digits
    }
    int eexp = 0;
    if (i < te && (data[i] == 'e' || data[i] == 'E')) {
        i++;
        bool eneg = false, eany = false;
        if (i < te && (data[i] == '+' || data[i] == '-')) { eneg = data[i] == '-'; i++; }
        // Saturate the exponent magnitude as we read it: any |exp| past a few
        // dozen already over/underflows float32 to inf/0, so capping changes no
        // result while preventing signed-overflow UB (and an unbounded scale
        // loop below) on adversarial tokens like "1e99999999999".
        while (i < te && data[i] >= '0' && data[i] <= '9') {
            if (eexp < 1000) { eexp = eexp * 10 + (data[i] - '0'); }
            i++; eany = true;
        }
        if (!eany) {
            return false;
        }
        if (eneg) { eexp = -eexp; }
    }
    if (i != te) {
        return false;  // trailing junk in the token (e.g. "1.5.2", "1x")
    }
    // Scale by 10^(fexp+eexp) in exact-table steps (see k_pow10).  Negative
    // exponents divide by the exact power rather than multiplying by an inexact
    // 10^-k.  |fexp| <= the token length and |eexp| <= 1000, so the loops are
    // bounded; once the value saturates to inf or flushes to 0 further steps
    // hold it there.
    int e = fexp + eexp;
    double d = mant;
    for (int rem = e; rem > 0;) {
        int step = rem > 22 ? 22 : rem;
        d *= k_pow10[step];
        rem -= step;
    }
    for (int rem = -e; rem > 0;) {
        int step = rem > 22 ? 22 : rem;
        d /= k_pow10[step];
        rem -= step;
    }
    *out = (float)(neg ? -d : d);
    return true;
}

// Next token parsed as a non-negative decimal integer <= cap (block counts,
// ids, glyph ids, cluster indices).  Stricter than the float reader: digits
// only, and the cap doubles as the overflow guard.
static bool read_uint(char const *__counted_by(le) data, size_t le,
                      size_t *__single jp, long cap, long *__single out) {
    size_t ts, tlen;
    if (!read_token(data, le, jp, &ts, &tlen) || tlen == 0) {
        return false;
    }
    long v = 0;
    for (size_t k = 0; k < tlen; k++) {
        char ch = data[ts + k];
        if (ch < '0' || ch > '9') {
            return false;
        }
        v = v * 10 + (ch - '0');
        if (v > cap) {
            return false;
        }
    }
    *out = v;
    return true;
}

static bool read_floats(char const *__counted_by(le) data, size_t le,
                        size_t *__single jp, float *__counted_by(n) f, int n) {
    for (int i = 0; i < n; i++) {
        if (!read_float(data, le, jp, &f[i])) {
            return false;
        }
    }
    return true;
}

// The rest of the line must be empty or a trailing # comment.
static bool at_eol(char const *__counted_by(le) data, size_t le, size_t j) {
    while (j < le && is_ws(data[j])) { j++; }
    return j >= le || data[j] == '#';
}

static bool read_bool(char const *__counted_by(le) data, size_t le,
                      size_t *__single jp, bool *__single out) {
    size_t ts, tlen;
    if (!read_token(data, le, jp, &ts, &tlen)) {
        return false;
    }
    if (tok_eq(data, le, ts, tlen, "0") || tok_eq(data, le, ts, tlen, "false")) {
        *out = false;
        return true;
    }
    if (tok_eq(data, le, ts, tlen, "1") || tok_eq(data, le, ts, tlen, "true")) {
        *out = true;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Text blocks: `font` / `glyph` / `bitmap`+`bits` / `shape`+`run` lines
// (written by cnvs_rec_text_blocks) pre-populate the canvas's text cache so
// the text ops that follow replay with no Core Text boundary call at all --
// the serialized half of the params -> derived-data lookup
// (docs/text-boundary.md).  The parser carries three pieces of cross-line
// state: the file-local font-id map, the shape block whose `run` lines are
// still arriving, and the bitmap block whose `bits` lines are.  Same posture
// as the op lines, applied to richer data: every count, id, verb token,
// base64 chunk, and cluster index is validated before it is trusted (the
// slice-A "untrusted verb stream" rule, now at the parser), and any violation
// -- or an allocation failure while rebuilding -- stops replay false.
// Cache-side degradation (a full intern/glyph table) is not a parse error:
// those entries drop and the affected text degrades exactly as a live cache
// under pressure does.

struct replay_blocks {
    int map[CNVS_FONT_INTERN_N];    // file font id -> interned cache id (-1 =
                                    // interning degraded; references still parse)
    bool seen[CNVS_FONT_INTERN_N];  // file font id declared by a `font` line
    cnvs_shaped *__single s;        // shape block under construction (owned
                                    // until its last `run` line inserts it)
    int runs_done;
    float size_px;                  // the pending shape's cache key
    char *__counted_by(text_len) text;  // owned copy of the pending key bytes
    int text_len;
    // The bitmap block under construction (owned until its last `bits` line
    // hands it to the cache).  bm stays NULL when the block's font id maps to
    // a degraded intern (-1): the bits still parse and validate, they just
    // have nowhere to land -- the glyph-block posture.
    uint8_t *__counted_by(bm_len) bm;
    int bm_len;     // the buffer's byte size (bm_total when allocated)
    int bm_total;   // expected decoded bytes: w*h*4
    int bm_fill;    // decoded so far
    int bm_lines;   // `bits` lines still expected; > 0 = a block is pending
    int bm_fid;     // interned cache id for the insert, or -1
    long bm_gid;
    int bm_w, bm_h;
    float bm_ink[4];  // capture-px ink box x0 y0 x1 y1
};

// Free the cross-line state (the pending shape, if its run lines never
// arrived, and the key copy).  cnvs_shaped_free handles a partially-built
// line: calloc'd runs hold NULL arrays with zero counts and no font handle.
static void blocks_drop(struct replay_blocks *__single b) {
    cnvs_shaped_free(b->s);
    b->s = NULL;
    b->runs_done = 0;
    free(b->text);
    b->text_len = 0;
    b->text = NULL;
}

// Free the pending bitmap state (a block whose `bits` lines never finished,
// or one just handed to the cache -- the caller NULLs bm first in that case).
static void bitmap_drop(struct replay_blocks *__single b) {
    free(b->bm);
    b->bm_len = 0;
    b->bm = NULL;
    b->bm_total = 0;
    b->bm_fill = 0;
    b->bm_lines = 0;
    b->bm_fid = -1;
    b->bm_gid = 0;
    b->bm_w = 0;
    b->bm_h = 0;
}

// The pending shape's last run line landed: hand it to the cache, which takes
// ownership, under its (size_px, text) key -- exactly the key a live lookup
// uses, so the fill_text/stroke_text op that follows hits.
static void blocks_finish_shape(canvas *__single cv,
                                struct replay_blocks *__single b) {
    cnvs_text_cache_put_shape(cnvs_canvas_text_cache(cv), b->size_px, b->text,
                              b->text_len, b->s);
    b->s = NULL;  // ownership went to the cache
    blocks_drop(b);
}

// font <id> <asc1> <desc1> <name...> -- declare a file-local font id: intern
// the name (the rest of the line; names can contain spaces) and record its
// vertical metrics (normalized at size 1.0).  Strict: id in range and not yet
// declared, finite metrics, non-empty name.
static bool replay_font(canvas *__single cv, struct replay_blocks *__single b,
                        char const *__counted_by(le) data, size_t le, size_t j) {
    long id = 0;
    float vm[2];
    if (!read_uint(data, le, &j, CNVS_FONT_INTERN_N - 1, &id) || b->seen[id]) {
        return false;
    }
    if (!read_floats(data, le, &j, vm, 2) ||
        !isfinite(vm[0]) || !isfinite(vm[1])) {
        return false;
    }
    if (j < le && data[j] == ' ') { j++; }  // the single separator before the name
    if (j >= le) {
        return false;  // empty name
    }
    b->seen[id] = true;
    int fid = cnvs_text_cache_intern(cnvs_canvas_text_cache(cv), data + j,
                                     (int)(le - j));
    b->map[id] = fid;
    if (fid >= 0) {
        cnvs_text_cache_set_vmetrics(cnvs_canvas_text_cache(cv), fid,
                                     vm[0], vm[1]);
    }
    return true;
}

// glyph <font-id> <gid> <upem> <ink x0 y0 x1 y1> <curves...> -- one glyph's
// canonical font-unit data, inserted under the interned (font, gid) key.  The
// curve list is m/l/q/c/z verb tokens with their control points, validated
// structurally in a counting pass and built in a second; a blank glyph (upem
// 0) carries no curves.  Strict: declared font id, gid <= 0xFFFF, finite
// numbers, upem >= 0.
static bool replay_glyph(canvas *__single cv, struct replay_blocks *__single b,
                         char const *__counted_by(le) data, size_t le, size_t j) {
    long id = 0, gid = 0;
    float meta[5];  // upem, then the font-unit ink box x0 y0 x1 y1
    if (!read_uint(data, le, &j, CNVS_FONT_INTERN_N - 1, &id) || !b->seen[id]) {
        return false;
    }
    if (!read_uint(data, le, &j, 0xFFFF, &gid)) {
        return false;
    }
    if (!read_floats(data, le, &j, meta, 5)) {
        return false;
    }
    for (int k = 0; k < 5; k++) {
        if (!isfinite(meta[k])) {
            return false;
        }
    }
    if (meta[0] < 0.0f) {
        return false;
    }
    // Pass 1: validate every token and count the verbs and points.
    size_t curves = j;
    int nv = 0, np = 0;
    while (!at_eol(data, le, j)) {
        size_t ts, tl;
        if (!read_token(data, le, &j, &ts, &tl) || tl != 1) {
            return false;
        }
        int k;
        switch (data[ts]) {
            case 'm':
            case 'l': k = 1; break;
            case 'q': k = 2; break;
            case 'c': k = 3; break;
            case 'z': k = 0; break;
            default: return false;  // not a verb token
        }
        for (int p = 0; p < k; p++) {
            float xy[2];
            if (!read_floats(data, le, &j, xy, 2) ||
                !isfinite(xy[0]) || !isfinite(xy[1])) {
                return false;
            }
        }
        nv++;
        np += k;
    }
    if (meta[0] == 0.0f && nv != 0) {
        return false;  // a blank glyph has no curves
    }
    if (b->map[id] < 0) {
        return true;  // interning degraded: a well-formed block with nowhere
    }                 // to land; its references degrade to blank glyphs
    // Pass 2: rebuild the owned arrays (the cache takes ownership).
    cnvs_glyph_verb *verbs = NULL;
    cnvs_vec2 *pts = NULL;
    if (nv > 0) {
        verbs = malloc((size_t)nv * sizeof *verbs);
        pts = malloc((size_t)(np > 0 ? np : 1) * sizeof *pts);
        if (!verbs || !pts) {
            free(verbs);
            free(pts);
            return false;  // OOM while rebuilding: stop replay
        }
        j = curves;
        int iv = 0, ip = 0;
        while (iv < nv && !at_eol(data, le, j)) {
            size_t ts = 0, tl = 0;
            (void)read_token(data, le, &j, &ts, &tl);  // validated by pass 1
            cnvs_glyph_verb v = CNVS_GLYPH_CLOSE;
            int k = 0;
            switch (data[ts]) {
                case 'm': v = CNVS_GLYPH_MOVE;  k = 1; break;
                case 'l': v = CNVS_GLYPH_LINE;  k = 1; break;
                case 'q': v = CNVS_GLYPH_QUAD;  k = 2; break;
                case 'c': v = CNVS_GLYPH_CUBIC; k = 3; break;
                default: break;  // 'z': close, no points
            }
            verbs[iv++] = v;
            for (int p = 0; p < k && ip < np; p++) {
                float xy[2] = { 0.0f, 0.0f };
                (void)read_floats(data, le, &j, xy, 2);
                pts[ip++] = (cnvs_vec2){ xy[0], xy[1] };
            }
        }
    }
    cnvs_text_cache_put_glyph(cnvs_canvas_text_cache(cv), b->map[id],
                              (uint16_t)gid, verbs, nv, pts, np, meta[0],
                              meta[1], meta[2], meta[3], meta[4]);
    return true;
}

// bitmap <font-id> <gid> <w> <h> <ink x0 y0 x1 y1> <nlines> -- begin one color
// glyph's canonical capture: w x h premultiplied RGBA8 at CNVS_CAPTURE_EM px
// to the em, whose w*h*4 bytes arrive base64-chunked in exactly <nlines>
// `bits` lines immediately following.  The ink box is in capture px (y up,
// baseline-relative; the buffer's bottom-left corner sits at (x0, y0)).
// Strict: declared font id, gid <= 0xFFFF, dims in [1, REPLAY_BITMAP_DIM_MAX],
// finite non-empty ink (the recorder never captures empty ink), and nlines in
// [1, ceil(w*h*4 / 3)] (each line must contribute at least one decoded byte).
static bool replay_bitmap(struct replay_blocks *__single b,
                          char const *__counted_by(le) data, size_t le, size_t j) {
    long id = 0, gid = 0, w = 0, h = 0, nlines = 0;
    float ink[4];
    if (!read_uint(data, le, &j, CNVS_FONT_INTERN_N - 1, &id) || !b->seen[id]) {
        return false;
    }
    if (!read_uint(data, le, &j, 0xFFFF, &gid)) {
        return false;
    }
    if (!read_uint(data, le, &j, REPLAY_BITMAP_DIM_MAX, &w) || w < 1 ||
        !read_uint(data, le, &j, REPLAY_BITMAP_DIM_MAX, &h) || h < 1) {
        return false;
    }
    if (!read_floats(data, le, &j, ink, 4)) {
        return false;
    }
    for (int k = 0; k < 4; k++) {
        if (!isfinite(ink[k])) {
            return false;
        }
    }
    if (ink[2] <= ink[0] || ink[3] <= ink[1]) {
        return false;  // empty ink never records a capture
    }
    long const total = w * h * 4;  // <= 1 MiB by the dims cap
    if (!read_uint(data, le, &j, (total + 2) / 3, &nlines) || nlines < 1) {
        return false;
    }
    if (!at_eol(data, le, j)) {
        return false;
    }
    b->bm_fid = b->map[id];  // -1 = interning degraded: validate, don't land
    if (b->bm_fid >= 0) {
        uint8_t *px = malloc((size_t)total);
        if (!px) {
            return false;  // OOM while rebuilding: stop replay
        }
        b->bm = px;
        b->bm_len = (int)total;
    }
    b->bm_total = (int)total;
    b->bm_fill = 0;
    b->bm_lines = (int)nlines;
    b->bm_gid = gid;
    b->bm_w = (int)w;
    b->bm_h = (int)h;
    for (int k = 0; k < 4; k++) {
        b->bm_ink[k] = ink[k];
    }
    return true;
}

// One base64 character's 6-bit value, or -1 (the strict alphabet: A-Z a-z 0-9
// + / only; '=' padding is handled structurally by the caller).
static int b64v(char ch) {
    if (ch >= 'A' && ch <= 'Z') { return ch - 'A'; }
    if (ch >= 'a' && ch <= 'z') { return ch - 'a' + 26; }
    if (ch >= '0' && ch <= '9') { return ch - '0' + 52; }
    if (ch == '+') { return 62; }
    if (ch == '/') { return 63; }
    return -1;
}

// bits <base64> -- one chunk of the pending bitmap block's capture bytes.
// Strict: only legal while a bitmap block is pending, one token of 4-char
// base64 groups, '=' padding only in the final group of the block's final
// line, the decoded bytes never exceeding w*h*4 -- and the block's last line
// must land the total exactly, at which point the capture is handed to the
// cache (which takes ownership; an existing entry wins, the usual best-effort
// posture).
static bool replay_bits(canvas *__single cv, struct replay_blocks *__single b,
                        char const *__counted_by(le) data, size_t le, size_t j) {
    if (b->bm_lines <= 0) {
        return false;  // no bitmap block pending
    }
    size_t ts = 0, tl = 0;
    if (!read_token(data, le, &j, &ts, &tl) || !at_eol(data, le, j)) {
        return false;
    }
    if (tl == 0 || tl % 4 != 0) {
        return false;
    }
    bool const last_line = b->bm_lines == 1;
    int fill = b->bm_fill;
    for (size_t g = 0; g < tl; g += 4) {
        int const v0 = b64v(data[ts + g]);
        int const v1 = b64v(data[ts + g + 1]);
        if (v0 < 0 || v1 < 0) {
            return false;
        }
        char const c2 = data[ts + g + 2], c3 = data[ts + g + 3];
        int v2 = b64v(c2), v3 = b64v(c3);
        int nbytes = 3;
        if (c3 == '=') {  // padding: only the final group of the final line
            if (!last_line || g + 4 != tl) {
                return false;
            }
            if (c2 == '=') {
                nbytes = 1;
                v2 = 0;
            } else {
                if (v2 < 0) {
                    return false;
                }
                nbytes = 2;
            }
            v3 = 0;
        } else if (v2 < 0 || v3 < 0) {
            return false;  // a bad character, or '=' anywhere but the tail
        }
        if (fill + nbytes > b->bm_total) {
            return false;  // more bytes than the header declared
        }
        uint32_t const v = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) |
                           ((uint32_t)v2 << 6) | (uint32_t)v3;
        uint8_t const by[3] = { (uint8_t)(v >> 16), (uint8_t)((v >> 8) & 0xFFu),
                                (uint8_t)(v & 0xFFu) };
        for (int k = 0; k < nbytes; k++) {
            if (b->bm) {
                b->bm[fill + k] = by[k];
            }
        }
        fill += nbytes;
    }
    b->bm_fill = fill;
    b->bm_lines -= 1;
    if (b->bm_lines == 0) {
        if (fill != b->bm_total) {
            return false;  // short block: fewer bytes than declared
        }
        if (b->bm) {
            cnvs_text_cache_put_capture(cnvs_canvas_text_cache(cv), b->bm_fid,
                                        (uint16_t)b->bm_gid, b->bm, b->bm_total,
                                        b->bm_w, b->bm_h, b->bm_ink[0],
                                        b->bm_ink[1], b->bm_ink[2],
                                        b->bm_ink[3]);
            b->bm_len = 0;  // ownership went to the cache
            b->bm = NULL;
        }
        bitmap_drop(b);
    }
    return true;
}

// shape <size_px> <utf16-len> <nruns> <byte-len> <text...> -- begin one shaped
// line; its `run` lines must follow immediately.  The text is exactly byte-len
// raw bytes after a single separating space (it is the cache key, byte for
// byte).  Strict: finite size, utf16-len <= byte-len (every UTF-16 unit costs
// at least one UTF-8 byte), and the byte count exactly fills the line.
static bool replay_shape(canvas *__single cv, struct replay_blocks *__single b,
                         char const *__counted_by(le) data, size_t le, size_t j) {
    float size = 0.0f;
    long t16 = 0, nruns = 0, blen = 0;
    if (!read_float(data, le, &j, &size) || !isfinite(size) || size < 0.0f) {
        return false;
    }
    if (!read_uint(data, le, &j, (long)REPLAY_LINE_MAX, &t16) ||
        !read_uint(data, le, &j, REPLAY_RUNS_MAX, &nruns) ||
        !read_uint(data, le, &j, (long)REPLAY_LINE_MAX, &blen)) {
        return false;
    }
    if (j < le && data[j] == ' ') { j++; }  // the single separator before the text
    if ((size_t)blen != le - j || t16 > blen) {
        return false;
    }
    cnvs_shaped *s = calloc(1, sizeof *s);
    if (!s) {
        return false;
    }
    s->size_px = size;
    s->text_len = (int)t16;
    if (nruns > 0) {
        cnvs_glyph_run *runs = calloc((size_t)nruns, sizeof *runs);
        if (!runs) {
            free(s);
            return false;
        }
        s->run = runs;
        s->nruns = (int)nruns;
    }
    char *txt = malloc((size_t)blen > 0 ? (size_t)blen : 1);
    if (!txt) {
        cnvs_shaped_free(s);
        return false;
    }
    if (blen > 0) {
        memcpy(txt, data + j, (size_t)blen);
    }
    b->s = s;
    b->runs_done = 0;
    b->size_px = size;
    b->text = txt;
    b->text_len = (int)blen;
    if (nruns == 0) {
        blocks_finish_shape(cv, b);  // nothing more to wait for
    }
    return true;
}

// run <font-id|-1> <rtl 0|1> <color 0|1> <nglyphs> (gid adv cluster)* -- one
// visual-order run of the pending shape block.  The rebuilt run carries the
// interned name id in place of a font handle (font == NULL): drawing reads its
// curves -- or, for a color run, its captures -- from the cache by name, so no
// CTFontRef is ever needed.  Strict: only legal while a shape block is
// pending, a declared (or -1) font id, finite advances, and every cluster
// index within the shape's UTF-16 length.
static bool replay_run(canvas *__single cv, struct replay_blocks *__single b,
                       char const *__counted_by(le) data, size_t le, size_t j) {
    if (!b->s) {
        return false;  // no shape block pending
    }
    int name_id = -1;
    size_t j0 = j, ts = 0, tl = 0;
    if (!read_token(data, le, &j, &ts, &tl)) {
        return false;
    }
    if (!tok_eq(data, le, ts, tl, "-1")) {  // -1 = an unkeyed (or color) run
        long id = 0;
        j = j0;
        if (!read_uint(data, le, &j, CNVS_FONT_INTERN_N - 1, &id) ||
            !b->seen[id]) {
            return false;
        }
        name_id = b->map[id];  // -1 when interning degraded
    }
    bool rtl = false, color = false;
    long n = 0;
    if (!read_bool(data, le, &j, &rtl) || !read_bool(data, le, &j, &color) ||
        !read_uint(data, le, &j, (long)(le - j), &n)) {
        return false;  // (each glyph triple takes >= 6 bytes, so the remaining
    }                  // line length safely bounds the allocation below)
    uint16_t *g = NULL;
    float *adv = NULL;
    int32_t *cl = NULL;
    if (n > 0) {
        g = malloc((size_t)n * sizeof *g);
        adv = malloc((size_t)n * sizeof *adv);
        cl = malloc((size_t)n * sizeof *cl);
        bool ok = g && adv && cl;
        for (long i = 0; ok && i < n; i++) {
            long gid = 0, cluster = 0;
            float a = 0.0f;
            ok = read_uint(data, le, &j, 0xFFFF, &gid) &&
                 read_float(data, le, &j, &a) && isfinite(a) &&
                 read_uint(data, le, &j, (long)b->s->text_len - 1, &cluster);
            if (ok) {
                g[i] = (uint16_t)gid;
                adv[i] = a;
                cl[i] = (int32_t)cluster;
            }
        }
        if (!ok || !at_eol(data, le, j)) {
            free(g);
            free(adv);
            free(cl);
            return false;
        }
    } else if (!at_eol(data, le, j)) {
        return false;
    }
    cnvs_glyph_run *run = &b->s->run[b->runs_done];
    run->glyph = g;
    run->xadv = adv;
    run->cluster = cl;
    run->count = (int)n;
    run->rtl = rtl;
    run->is_color = color;
    run->name_id = name_id;  // color runs too: the capture cache keys by name
    run->font = NULL;
    b->runs_done++;
    if (b->runs_done == b->s->nruns) {
        blocks_finish_shape(cv, b);
    }
    return true;
}

// Composite-op names in enum order (canvas_composite_op).
static char const *const k_composite[] = {
    "source-over", "source-in", "source-out", "source-atop", "destination-over",
    "destination-in", "destination-out", "destination-atop", "xor", "lighter",
    "copy", "multiply", "screen", "overlay", "darken", "lighten", "color-dodge",
    "color-burn", "hard-light", "soft-light", "difference", "exclusion", "hue",
    "saturation", "color", "luminosity",
};

static bool replay_line(canvas *__single cv, struct replay_blocks *__single blk,
                        char const *__counted_by(le) data, size_t ls, size_t le) {
    size_t j = ls;
    size_t cs, cl;
    if (!read_token(data, le, &j, &cs, &cl)) {
        return blk->s == NULL &&
               blk->bm_lines == 0;  // blank line (illegal inside a block)
    }
    if (data[cs] == '#') {
        return blk->s == NULL && blk->bm_lines == 0;  // comment line (ditto)
    }
    if (blk->s && !tok_eq(data, le, cs, cl, "run")) {
        return false;  // a shape block's run lines must follow it directly
    }
    if (blk->bm_lines > 0 && !tok_eq(data, le, cs, cl, "bits")) {
        return false;  // a bitmap block's bits lines must follow it directly
    }

    float f[8];
    bool b;

    // --- no-arg ---
    if (tok_eq(data, le, cs, cl, "save"))            { canvas_save(cv); }
    else if (tok_eq(data, le, cs, cl, "restore"))    { canvas_restore(cv); }
    else if (tok_eq(data, le, cs, cl, "reset_transform")) { canvas_reset_transform(cv); }
    else if (tok_eq(data, le, cs, cl, "begin_path")) { canvas_begin_path(cv); }
    else if (tok_eq(data, le, cs, cl, "close_path")) { canvas_close_path(cv); }
    else if (tok_eq(data, le, cs, cl, "fill"))       { canvas_fill(cv); }
    else if (tok_eq(data, le, cs, cl, "stroke"))     { canvas_stroke(cv); }
    else if (tok_eq(data, le, cs, cl, "clip"))       { canvas_clip(cv); }

    // --- 1 float ---
    else if (tok_eq(data, le, cs, cl, "rotate"))           { if (!read_floats(data, le, &j, f, 1)) return false; canvas_rotate(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "set_global_alpha"))     { if (!read_floats(data, le, &j, f, 1)) return false; canvas_set_global_alpha(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "set_line_width"))       { if (!read_floats(data, le, &j, f, 1)) return false; canvas_set_line_width(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "set_miter_limit"))      { if (!read_floats(data, le, &j, f, 1)) return false; canvas_set_miter_limit(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "set_line_dash_offset")) { if (!read_floats(data, le, &j, f, 1)) return false; canvas_set_line_dash_offset(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "set_font_size"))        { if (!read_floats(data, le, &j, f, 1)) return false; canvas_set_font_size(cv, f[0]); }

    // --- 2 float ---
    else if (tok_eq(data, le, cs, cl, "translate")) { if (!read_floats(data, le, &j, f, 2)) return false; canvas_translate(cv, f[0], f[1]); }
    else if (tok_eq(data, le, cs, cl, "scale"))     { if (!read_floats(data, le, &j, f, 2)) return false; canvas_scale(cv, f[0], f[1]); }
    else if (tok_eq(data, le, cs, cl, "move_to"))   { if (!read_floats(data, le, &j, f, 2)) return false; canvas_move_to(cv, f[0], f[1]); }
    else if (tok_eq(data, le, cs, cl, "line_to"))   { if (!read_floats(data, le, &j, f, 2)) return false; canvas_line_to(cv, f[0], f[1]); }

    // --- 4 float ---
    else if (tok_eq(data, le, cs, cl, "rect"))       { if (!read_floats(data, le, &j, f, 4)) return false; canvas_rect(cv, f[0], f[1], f[2], f[3]); }
    else if (tok_eq(data, le, cs, cl, "clear_rect")) { if (!read_floats(data, le, &j, f, 4)) return false; canvas_clear_rect(cv, f[0], f[1], f[2], f[3]); }
    else if (tok_eq(data, le, cs, cl, "fill_rect"))  { if (!read_floats(data, le, &j, f, 4)) return false; canvas_fill_rect(cv, f[0], f[1], f[2], f[3]); }
    else if (tok_eq(data, le, cs, cl, "set_fill_rgba"))  { if (!read_floats(data, le, &j, f, 4)) return false; canvas_set_fill_rgba(cv, f[0], f[1], f[2], f[3]); }
    else if (tok_eq(data, le, cs, cl, "set_stroke_rgba")){ if (!read_floats(data, le, &j, f, 4)) return false; canvas_set_stroke_rgba(cv, f[0], f[1], f[2], f[3]); }
    else if (tok_eq(data, le, cs, cl, "quadratic_curve_to"))    { if (!read_floats(data, le, &j, f, 4)) return false; canvas_quadratic_curve_to(cv, f[0], f[1], f[2], f[3]); }
    else if (tok_eq(data, le, cs, cl, "set_fill_linear_gradient"))   { if (!read_floats(data, le, &j, f, 4)) return false; canvas_set_fill_linear_gradient(cv, f[0], f[1], f[2], f[3]); }
    else if (tok_eq(data, le, cs, cl, "set_stroke_linear_gradient")) { if (!read_floats(data, le, &j, f, 4)) return false; canvas_set_stroke_linear_gradient(cv, f[0], f[1], f[2], f[3]); }

    // --- 5 float ---
    else if (tok_eq(data, le, cs, cl, "round_rect"))       { if (!read_floats(data, le, &j, f, 5)) return false; canvas_round_rect(cv, f[0], f[1], f[2], f[3], f[4]); }
    else if (tok_eq(data, le, cs, cl, "arc_to"))           { if (!read_floats(data, le, &j, f, 5)) return false; canvas_arc_to(cv, f[0], f[1], f[2], f[3], f[4]); }
    else if (tok_eq(data, le, cs, cl, "add_fill_color_stop"))  { if (!read_floats(data, le, &j, f, 5)) return false; canvas_add_fill_color_stop(cv, f[0], f[1], f[2], f[3], f[4]); }
    else if (tok_eq(data, le, cs, cl, "add_stroke_color_stop")){ if (!read_floats(data, le, &j, f, 5)) return false; canvas_add_stroke_color_stop(cv, f[0], f[1], f[2], f[3], f[4]); }

    // --- 6 float ---
    else if (tok_eq(data, le, cs, cl, "transform"))      { if (!read_floats(data, le, &j, f, 6)) return false; canvas_transform(cv, f[0], f[1], f[2], f[3], f[4], f[5]); }
    else if (tok_eq(data, le, cs, cl, "set_transform"))  { if (!read_floats(data, le, &j, f, 6)) return false; canvas_set_transform(cv, f[0], f[1], f[2], f[3], f[4], f[5]); }
    else if (tok_eq(data, le, cs, cl, "bezier_curve_to"))      { if (!read_floats(data, le, &j, f, 6)) return false; canvas_bezier_curve_to(cv, f[0], f[1], f[2], f[3], f[4], f[5]); }
    else if (tok_eq(data, le, cs, cl, "set_fill_radial_gradient"))   { if (!read_floats(data, le, &j, f, 6)) return false; canvas_set_fill_radial_gradient(cv, f[0], f[1], f[2], f[3], f[4], f[5]); }
    else if (tok_eq(data, le, cs, cl, "set_stroke_radial_gradient")) { if (!read_floats(data, le, &j, f, 6)) return false; canvas_set_stroke_radial_gradient(cv, f[0], f[1], f[2], f[3], f[4], f[5]); }

    // --- float run + trailing bool ---
    else if (tok_eq(data, le, cs, cl, "arc")) {
        if (!read_floats(data, le, &j, f, 5) || !read_bool(data, le, &j, &b)) return false;
        canvas_arc(cv, f[0], f[1], f[2], f[3], f[4], b);
    }
    else if (tok_eq(data, le, cs, cl, "ellipse")) {
        if (!read_floats(data, le, &j, f, 7) || !read_bool(data, le, &j, &b)) return false;
        canvas_ellipse(cv, f[0], f[1], f[2], f[3], f[4], f[5], f[6], b);
    }

    // --- enums by name ---
    else if (tok_eq(data, le, cs, cl, "set_fill_rule")) {
        size_t ts, tl;
        if (!read_token(data, le, &j, &ts, &tl)) return false;
        if (tok_eq(data, le, ts, tl, "nonzero"))      canvas_set_fill_rule(cv, CANVAS_NONZERO);
        else if (tok_eq(data, le, ts, tl, "evenodd")) canvas_set_fill_rule(cv, CANVAS_EVENODD);
        else return false;
    }
    else if (tok_eq(data, le, cs, cl, "set_line_join")) {
        size_t ts, tl;
        if (!read_token(data, le, &j, &ts, &tl)) return false;
        if (tok_eq(data, le, ts, tl, "miter"))      canvas_set_line_join(cv, CANVAS_JOIN_MITER);
        else if (tok_eq(data, le, ts, tl, "round")) canvas_set_line_join(cv, CANVAS_JOIN_ROUND);
        else if (tok_eq(data, le, ts, tl, "bevel")) canvas_set_line_join(cv, CANVAS_JOIN_BEVEL);
        else return false;
    }
    else if (tok_eq(data, le, cs, cl, "set_line_cap")) {
        size_t ts, tl;
        if (!read_token(data, le, &j, &ts, &tl)) return false;
        if (tok_eq(data, le, ts, tl, "butt"))        canvas_set_line_cap(cv, CANVAS_CAP_BUTT);
        else if (tok_eq(data, le, ts, tl, "round"))  canvas_set_line_cap(cv, CANVAS_CAP_ROUND);
        else if (tok_eq(data, le, ts, tl, "square")) canvas_set_line_cap(cv, CANVAS_CAP_SQUARE);
        else return false;
    }
    else if (tok_eq(data, le, cs, cl, "set_text_align")) {
        size_t ts, tl;
        if (!read_token(data, le, &j, &ts, &tl)) return false;
        if (tok_eq(data, le, ts, tl, "start"))       canvas_set_text_align(cv, CANVAS_ALIGN_START);
        else if (tok_eq(data, le, ts, tl, "end"))    canvas_set_text_align(cv, CANVAS_ALIGN_END);
        else if (tok_eq(data, le, ts, tl, "left"))   canvas_set_text_align(cv, CANVAS_ALIGN_LEFT);
        else if (tok_eq(data, le, ts, tl, "right"))  canvas_set_text_align(cv, CANVAS_ALIGN_RIGHT);
        else if (tok_eq(data, le, ts, tl, "center")) canvas_set_text_align(cv, CANVAS_ALIGN_CENTER);
        else return false;
    }
    else if (tok_eq(data, le, cs, cl, "set_text_baseline")) {
        size_t ts, tl;
        if (!read_token(data, le, &j, &ts, &tl)) return false;
        if (tok_eq(data, le, ts, tl, "alphabetic"))       canvas_set_text_baseline(cv, CANVAS_BASELINE_ALPHABETIC);
        else if (tok_eq(data, le, ts, tl, "top"))         canvas_set_text_baseline(cv, CANVAS_BASELINE_TOP);
        else if (tok_eq(data, le, ts, tl, "hanging"))     canvas_set_text_baseline(cv, CANVAS_BASELINE_HANGING);
        else if (tok_eq(data, le, ts, tl, "middle"))      canvas_set_text_baseline(cv, CANVAS_BASELINE_MIDDLE);
        else if (tok_eq(data, le, ts, tl, "ideographic")) canvas_set_text_baseline(cv, CANVAS_BASELINE_IDEOGRAPHIC);
        else if (tok_eq(data, le, ts, tl, "bottom"))      canvas_set_text_baseline(cv, CANVAS_BASELINE_BOTTOM);
        else return false;
    }
    else if (tok_eq(data, le, cs, cl, "set_global_composite_operation")) {
        size_t ts, tl;
        if (!read_token(data, le, &j, &ts, &tl)) return false;
        int op = -1;
        for (int k = 0; k < (int)(sizeof k_composite / sizeof k_composite[0]); k++) {
            if (tok_eq(data, le, ts, tl, k_composite[k])) { op = k; break; }
        }
        if (op < 0) return false;
        canvas_set_global_composite_operation(cv, (canvas_composite_op)op);
    }

    // --- variadic dash ---
    else if (tok_eq(data, le, cs, cl, "set_line_dash")) {
        float dash[REPLAY_DASH_MAX];
        int n = 0;
        while (!at_eol(data, le, j)) {
            if (n >= REPLAY_DASH_MAX || !read_float(data, le, &j, &dash[n])) return false;
            n++;
        }
        canvas_set_line_dash(cv, dash, n);
        return true;  // dash consumed the rest of the line
    }

    // --- text blocks (cross-line state; see canvas.h on the program format) ---
    else if (tok_eq(data, le, cs, cl, "font"))   { return replay_font(cv, blk, data, le, j); }
    else if (tok_eq(data, le, cs, cl, "glyph"))  { return replay_glyph(cv, blk, data, le, j); }
    else if (tok_eq(data, le, cs, cl, "bitmap")) { return replay_bitmap(blk, data, le, j); }
    else if (tok_eq(data, le, cs, cl, "bits"))   { return replay_bits(cv, blk, data, le, j); }
    else if (tok_eq(data, le, cs, cl, "shape"))  { return replay_shape(cv, blk, data, le, j); }
    else if (tok_eq(data, le, cs, cl, "run"))    { return replay_run(cv, blk, data, le, j); }

    // --- text tail (rest of line, UTF-8) ---
    else if (tok_eq(data, le, cs, cl, "fill_text") || tok_eq(data, le, cs, cl, "stroke_text")) {
        bool fill = data[cs] == 'f';
        if (!read_floats(data, le, &j, f, 2)) return false;
        while (j < le && is_ws(data[j])) { j++; }
        // The text is the rest of the line: hand the slice straight to the
        // length-counted text API -- no copy, no NUL, no forge.
        int n = (int)(le - j);  // le - j <= line cap < INT_MAX
        if (fill) canvas_fill_text_n(cv, data + j, n, f[0], f[1]);
        else      canvas_stroke_text_n(cv, data + j, n, f[0], f[1]);
        return true;  // text consumed the rest of the line
    }

    else {
        return false;  // unknown command
    }

    return at_eol(data, le, j);  // fixed-arity commands: nothing should follow
}

bool cnvs_replay_text(canvas *__single cv, char const *__counted_by(len) data, size_t len) {
    struct replay_blocks b = { .s = NULL, .runs_done = 0, .size_px = 0.0f,
                               .text_len = 0, .text = NULL, .bm = NULL,
                               .bm_len = 0, .bm_total = 0, .bm_fill = 0,
                               .bm_lines = 0, .bm_fid = -1, .bm_gid = 0,
                               .bm_w = 0, .bm_h = 0, .bm_ink = { 0 } };
    for (int k = 0; k < CNVS_FONT_INTERN_N; k++) {
        b.map[k] = -1;
        b.seen[k] = false;
    }
    bool ok = true;
    size_t i = 0;
    while (ok && i < len) {
        size_t le = i;
        while (le < len && data[le] != '\n') { le++; }
        ok = le - i < REPLAY_LINE_MAX            // over-long line: reject
             && replay_line(cv, &b, data, i, le);
        i = le + 1;  // past the '\n' (or past end)
    }
    if (b.s || b.bm_lines > 0) {
        ok = false;  // truncated shape or bitmap block: its run/bits lines
    }                // never arrived
    blocks_drop(&b);
    bitmap_drop(&b);
    return ok;
}

bool canvas_replay_from(canvas *__single cv, char const *__null_terminated path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return false;
    }
    long sz = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    if (sz < 0 || (unsigned long)sz > REPLAY_FILE_MAX) {
        (void)fclose(f);
        return false;
    }
    size_t const n = (size_t)sz;
    char *__counted_by_or_null(n) buf = malloc(n > 0 ? n : 1);
    if (!buf) {
        (void)fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, n, f);
    (void)fclose(f);
    bool ok = cnvs_replay_text(cv, buf, got);
    free(buf);
    return ok;
}
