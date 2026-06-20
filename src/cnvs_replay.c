// Text canvas-program parser + replayer (the cnvs_replay.h core, and
// canvas_replay_from()).  Parses untrusted text by index over a __counted_by
// buffer -- the bounds-safe-parsing exercise -- and dispatches to the public
// canvas_* API.  No forges and no __null_terminated: numbers are parsed in place
// by index (no strtof), and the text tail is passed as a slice to the
// length-counted canvas_*_text_n; the parser stays entirely in the indexable
// world.  (docs/bounds-safety.md walks through what that took.)

#include "cnvs_replay.h"

#include "canvas.h"
#include "cnvs_path2d.h"  // struct canvas_path2d's command count (OOM-drop detection)
#include "cnvs_record.h"  // the shared numbered-object caps (CNVS_REC_*_MAX)
#include "cnvs_text.h"
#include "cnvs_zlib.h"

#include <limits.h>
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
#define REPLAY_PATH_CMDS_MAX (1 << 20)  // command lines one path block may
                                        // declare (a sanity cap: the storage
                                        // grows per arriving line, so memory
                                        // tracks the file's actual size)
#define REPLAY_RESIZE_DIM_MAX 16384     // resize dims (mirrors canvas.c's
                                        // CANVAS_DIM_MAX: the recorder only
                                        // writes a resize that succeeded)
#define REPLAY_BITMAP_DIM_MAX 512    // capture dims cap: bounds a bitmap
                                     // block's decoded allocation at 1 MiB
                                     // (the recorder writes CNVS_CAPTURE_EM =
                                     // 160); the deflated stream is further
                                     // capped at cnvs_zlib_bound of that, so
                                     // both of the block's buffers are bounded
                                     // before either is allocated

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
    size_t const start = j;
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
        char const c = *lit;
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
static_assert(sizeof k_pow10 / sizeof *k_pow10 == 23,
              "read_float steps its scaling by at most 10^22 at a time");

// Next token parsed as a float, in place by index -- no strtof.  Stricter than
// strtof: the whole token must be a
// number (sign? digits, optional .fraction, optional e[+-]exp).
//
// EXACT for everything the recorder emits: a %.9g-printed float32 has a <=
// 9-digit mantissa (exact in the double accumulator) and a base-10 exponent
// within +/-53, so the table-stepped scaling below costs at most three
// roundings (~2^-51 relative) -- orders of magnitude inside the margin nine
// significant digits leave around any float32, so the (float) conversion lands
// on the identical value (record -> replay round-trips bit for bit).  Hostile
// exponents saturate in magnitude and flush the value to +/-inf or 0.
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
    int const e = fexp + eexp;
    double d = mant;
    for (int rem = e; rem > 0;) {
        int step = rem > 22 ? 22 : rem;
        d *= k_pow10[step];
        rem -= step;
    }
    for (int rem = -e; rem > 0;) {
        int const step = rem > 22 ? 22 : rem;
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
        char const ch = data[ts + k];
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

// Next token parsed as a signed decimal integer with |value| <= cap (the
// int-typed op arguments: put_image_data placement, dirty rects, resize).
// An optional leading '-' (the recorder's %d never writes '+'), then digits.
static bool read_int(char const *__counted_by(le) data, size_t le,
                     size_t *__single jp, long cap, long *__single out) {
    size_t ts, tlen;
    if (!read_token(data, le, jp, &ts, &tlen) || tlen == 0) {
        return false;
    }
    size_t k = 0;
    bool const neg = data[ts] == '-';
    if (neg) {
        k = 1;
        if (tlen == 1) {
            return false;  // a bare "-"
        }
    }
    long v = 0;
    for (; k < tlen; k++) {
        char const ch = data[ts + k];
        if (ch < '0' || ch > '9') {
            return false;
        }
        v = v * 10 + (ch - '0');
        if (v > cap) {
            return false;
        }
    }
    *out = neg ? -v : v;
    return true;
}

static bool read_ints(char const *__counted_by(le) data, size_t le,
                      size_t *__single jp, long cap,
                      long *__counted_by(n) v, int n) {
    for (int i = 0; i < n; i++) {
        if (!read_int(data, le, jp, cap, &v[i])) {
            return false;
        }
    }
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

// A colour op line's OPTIONAL trailing colour-space token, read after its
// floats: absent (end of line) -> CANVAS_CS_SRGB, the sRGB default, so a legacy
// untagged colour line parses byte-identically; present -> one of the three
// colour-space names (the same emit-when-non-sRGB token cnvs_rec_floats_cs
// writes).  Strict: any other trailing token (or anything after the token) is
// malformed and stops replay, matching the format's posture for every other
// trailing enum.  *jp is advanced past the token (or left at end of line).
static bool read_opt_cs(char const *__counted_by(le) data, size_t le,
                        size_t *__single jp, enum canvas_color_space *__single out) {
    *out = CANVAS_CS_SRGB;
    if (at_eol(data, le, *jp)) {
        return true;  // no token: the sRGB default
    }
    size_t ts, tl;
    if (!read_token(data, le, jp, &ts, &tl)) {
        return false;
    }
    if      (tok_eq(data, le, ts, tl, canvas_color_space_name[CANVAS_CS_SRGB])) {
        *out = CANVAS_CS_SRGB;
    } else if (tok_eq(data, le, ts, tl, canvas_color_space_name[CANVAS_CS_LINEAR_SRGB])) {
        *out = CANVAS_CS_LINEAR_SRGB;
    } else if (tok_eq(data, le, ts, tl, canvas_color_space_name[CANVAS_CS_OKLAB])) {
        *out = CANVAS_CS_OKLAB;
    } else {
        return false;  // an unknown trailing token: malformed
    }
    return at_eol(data, le, *jp);  // nothing may follow the token
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
// Text blocks: `font` / `glyph` / `bitmap`+`bits` / `shaping`+`run` lines
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

// One file-local numbered image (`image` block): a borrowed view of the
// decoded RGBA8 the canvas has adopted (cnvs_canvas_own_image -- patterns
// borrow their source, so the pixels must outlive replay).  px != NULL is the
// "declared" flag: unlike font interning, an image block has no degraded
// landing -- a rebuild failure stops replay -- so a declared id always holds
// pixels.
struct replay_image {
    uint8_t *__counted_by(len) px;
    int len;  // w * h * bpp bytes
    int w, h;
    enum canvas_color_type ct;  // the block's colour type, read by name...
    enum canvas_alpha_type at;  // ...and its alpha type, likewise
    enum canvas_color_space cs; // ...and its colour-space tag (optional token,
                                // absent == sRGB)
    bool mips;    // an `image_mips` line ran: draws carry mip-chain semantics
};

struct replay_blocks {
    int map[CNVS_FONT_INTERN_N];    // file font id -> interned cache id (-1 =
                                    // interning degraded; references still parse)
    bool seen[CNVS_FONT_INTERN_N];  // file font id declared by a `font` line
    struct cnvs_shaped *__single s;        // shape block under construction (owned
                                    // until its last `run` line inserts it)
    int runs_done;
    float size_px;                  // the pending shape's cache key...
    bool rtl;                       // ...its paragraph-direction half...
    float ls, ws;                   // ...and its letter/word spacing halves
    char *__counted_by(text_len) text;  // owned copy of the pending key bytes
    int text_len;
    // The bitmap/image block under construction (owned until its last `bits`
    // line inflates and hands the pixels on).  bm accumulates the DEFLATED
    // stream the `bits` lines carry (bm_zlen declared bytes); the decoded
    // w*h*4 pixel buffer is allocated only once the stream has landed
    // completely and must inflate to exactly bm_total bytes.  bm_img >= 0
    // means the pending block is an `image` (the pixels land in the image
    // table under that id); -1 means a `bitmap` (emoji capture), landing in
    // the text cache under bm_fid.  A capture's stream is decoded and
    // inflate-validated even when its font id maps to a degraded intern
    // (bm_fid -1): the bits still parse and validate, they just have nowhere
    // to land -- the glyph-block posture.
    uint8_t *__counted_by(bm_zlen) bm;
    int bm_zlen;    // declared deflated byte count, the buffer's size
    int bm_total;   // expected inflated bytes: w*h*4
    int bm_fill;    // deflated bytes decoded so far
    int bm_lines;   // `bits` lines still expected; > 0 = a block is pending
    int bm_fid;     // interned cache id for a capture's insert, or -1
    int bm_img;     // image-table id for an `image` block's insert, or -1
    enum canvas_color_type bm_ct;  // the pending image block's colour type
    enum canvas_alpha_type bm_at;  // ...and its alpha type
    enum canvas_color_space bm_cs; // ...and its colour-space tag (absent==sRGB)
    long bm_gid;
    int bm_w, bm_h;
    float bm_ink[4];  // capture-px ink box x0 y0 x1 y1
    struct replay_image img[CNVS_REC_IMAGES_MAX];  // declared `image` blocks
    // Path2D blocks: file-local numbered paths, rebuilt through the public
    // builders and owned by the parser for the duration of replay (the path
    // ops replay a path's commands immediately and retain nothing, so unlike
    // images they don't outlive replay).  pend_path is the block under
    // construction: its command lines must follow directly, pend_left of them
    // still expected.
    struct canvas_path2d *__single paths[CNVS_REC_PATHS_MAX];
    struct canvas_path2d *__single pend_path;
    int pend_id;
    int pend_cmds;  // the block's declared command count
    int pend_left;  // command lines still expected
    // The working_space line (if any) must lead the file -- it reconfigures the
    // fresh canvas's immutable space before the first colour interns.  `started`
    // latches on the first command line that does anything (a draw, a setter, a
    // block), so a working_space arriving after that is rejected as malformed.
    bool started;
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

// Free the pending bitmap/image state (a block whose `bits` lines never
// finished, or one whose inflated pixels were just handed on).
static void bitmap_drop(struct replay_blocks *__single b) {
    free(b->bm);
    b->bm_zlen = 0;
    b->bm = NULL;
    b->bm_total = 0;
    b->bm_fill = 0;
    b->bm_lines = 0;
    b->bm_fid = -1;
    b->bm_img = -1;
    b->bm_gid = 0;
    b->bm_w = 0;
    b->bm_h = 0;
}

// Free the parser's Path2D state: the pending block (if its command lines
// never finished) and every installed path -- the parser owns them all; the
// path ops borrow one only for the duration of the call.
static void paths_drop(struct replay_blocks *__single b) {
    canvas_path2d_free(b->pend_path);
    b->pend_path = NULL;
    b->pend_id = 0;
    b->pend_cmds = 0;
    b->pend_left = 0;
    for (int i = 0; i < CNVS_REC_PATHS_MAX; i++) {
        canvas_path2d_free(b->paths[i]);
        b->paths[i] = NULL;
    }
}

// The pending shape's last run line landed: hand it to the cache, which takes
// ownership, under its (size_px, rtl, text) key -- exactly the key a live
// lookup uses, so the fill_text/stroke_text op that follows hits (and an ltr
// and an rtl shaping of the same bytes land in distinct slots, never aliased).
static void blocks_finish_shaping(struct canvas *__single cv,
                                struct replay_blocks *__single b) {
    cnvs_text_cache_put_shaping(cnvs_canvas_text_cache(cv), b->size_px, b->rtl,
                              b->ls, b->ws, b->text, b->text_len, b->s);
    b->s = NULL;  // ownership went to the cache
    // Set the canvas's spacing from the block: a fill_text/stroke_text op carries
    // no spacing of its own and the recorder writes no spacing setters, so the
    // shaping block that precedes the op is what restores the spacing its lookup
    // must key against (the recorder re-emits the block whenever this context
    // would otherwise be stale).
    canvas_set_letter_spacing(cv, b->ls);
    canvas_set_word_spacing(cv, b->ws);
    blocks_drop(b);
}

// font <id> <asc1> <desc1> <name...> -- declare a file-local font id: intern
// the name (the rest of the line; names can contain spaces) and record its
// vertical metrics (normalized at size 1.0).  Strict: id in range and not yet
// declared, finite metrics, non-empty name.
static bool replay_font(struct canvas *__single cv, struct replay_blocks *__single b,
                        char const *__counted_by(le) data, size_t le, size_t j) {
    long file_id = 0;
    float vm[2];
    if (!read_uint(data, le, &j, CNVS_FONT_INTERN_N - 1, &file_id) ||
        b->seen[file_id]) {
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
    b->seen[file_id] = true;
    int fid = cnvs_text_cache_intern(cnvs_canvas_text_cache(cv), data + j,
                                     (int)(le - j));
    b->map[file_id] = fid;
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
static bool replay_glyph(struct canvas *__single cv, struct replay_blocks *__single b,
                         char const *__counted_by(le) data, size_t le, size_t j) {
    long file_id = 0, gid = 0;
    float meta[5];  // upem, then the font-unit ink box x0 y0 x1 y1
    if (!read_uint(data, le, &j, CNVS_FONT_INTERN_N - 1, &file_id) ||
        !b->seen[file_id]) {
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
    size_t const curves = j;
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
    if (b->map[file_id] < 0) {
        return true;  // interning degraded: a well-formed block with nowhere
    }                 // to land; its references degrade to blank glyphs
    // Pass 2: rebuild the owned arrays (the cache takes ownership).
    enum cnvs_glyph_verb *verbs = NULL;
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
            enum cnvs_glyph_verb v = CNVS_GLYPH_CLOSE;
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
    cnvs_text_cache_put_glyph(cnvs_canvas_text_cache(cv), b->map[file_id],
                              (uint16_t)gid, verbs, nv, pts, np, meta[0],
                              meta[1], meta[2], meta[3], meta[4]);
    return true;
}

// bitmap <font-id> <gid> <w> <h> <ink x0 y0 x1 y1> <zlen> <nlines> -- begin
// one color glyph's canonical capture: w x h premultiplied RGBA8 at
// CNVS_CAPTURE_EM px to the em, deflated (cnvs_zlib) to exactly <zlen> bytes
// that arrive base64-chunked in exactly <nlines> `bits` lines immediately
// following and must inflate back to exactly w*h*4 bytes.  The ink box is in
// capture px (y up, baseline-relative; the buffer's bottom-left corner sits
// at (x0, y0)).  Strict: declared font id, gid <= 0xFFFF, dims in
// [1, REPLAY_BITMAP_DIM_MAX], finite non-empty ink (the recorder never
// captures empty ink), zlen in [1, cnvs_zlib_bound(w*h*4)] (no stream our
// deflate writes is longer, so a larger claim is a lie -- and the cap bounds
// the stream buffer, checked before it is allocated), and nlines in
// [1, ceil(zlen / 3)] (each line must contribute at least one decoded byte).
static bool replay_bitmap(struct replay_blocks *__single b,
                          char const *__counted_by(le) data, size_t le, size_t j) {
    long file_id = 0, gid = 0, w = 0, h = 0, zlen = 0, nlines = 0;
    float ink[4];
    if (!read_uint(data, le, &j, CNVS_FONT_INTERN_N - 1, &file_id) ||
        !b->seen[file_id]) {
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
    if (!read_uint(data, le, &j, cnvs_zlib_bound((int)total), &zlen) ||
        zlen < 1) {
        return false;  // deflated length: declared, sane, before any malloc
    }
    if (!read_uint(data, le, &j, (zlen + 2) / 3, &nlines) || nlines < 1) {
        return false;
    }
    if (!at_eol(data, le, j)) {
        return false;
    }
    uint8_t *zs = malloc((size_t)zlen);
    if (!zs) {
        return false;  // OOM while rebuilding: stop replay
    }
    b->bm_zlen = (int)zlen;
    b->bm = zs;              // count before pointer
    b->bm_fid = b->map[file_id];  // -1 = interning degraded: validate, don't land
    b->bm_img = -1;          // a capture, not an `image` block
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

// image <id> <w> <h> <zlen> <nlines> -- begin one numbered RGBA8 image (a
// drawImage / putImageData / pattern source): w x h straight-alpha pixels,
// deflated to exactly <zlen> bytes arriving in exactly <nlines> `bits` lines,
// the emoji-capture machinery reused wholesale.  Strict: an unused id (ids
// declare once), dims >= 1 with w*h*4 <= CNVS_REC_IMAGE_BYTES_MAX (bounding
// the decoded allocation before either buffer exists), zlen in
// [1, cnvs_zlib_bound(w*h*4)] and nlines in [1, ceil(zlen / 3)].
static bool replay_image(struct replay_blocks *__single b,
                         char const *__counted_by(le) data, size_t le, size_t j) {
    long id = 0, w = 0, h = 0, zlen = 0, nlines = 0;
    if (!read_uint(data, le, &j, CNVS_REC_IMAGES_MAX - 1, &id) || b->img[id].px) {
        return false;
    }
    // The format axes, by name, like every other enum in the format.
    size_t ts, tl;
    enum canvas_color_type ct;
    if (!read_token(data, le, &j, &ts, &tl)) return false;
    if (tok_eq(data, le, ts, tl, "unorm8"))   ct = CANVAS_COLOR_UNORM8;
    else if (tok_eq(data, le, ts, tl, "f16")) ct = CANVAS_COLOR_F16;
    else return false;
    enum canvas_alpha_type at;
    if (!read_token(data, le, &j, &ts, &tl)) return false;
    if (tok_eq(data, le, ts, tl, "unpremul"))    at = CANVAS_ALPHA_UNPREMUL;
    else if (tok_eq(data, le, ts, tl, "premul")) at = CANVAS_ALPHA_PREMUL;
    else return false;
    long const bpp = ct == CANVAS_COLOR_F16 ? 8 : 4;
    long const dcap = CNVS_REC_IMAGE_BYTES_MAX / bpp;  // a 1-px-tall image's max w
    if (!read_uint(data, le, &j, dcap, &w) || w < 1 ||
        !read_uint(data, le, &j, dcap, &h) || h < 1 ||
        w * h * bpp > CNVS_REC_IMAGE_BYTES_MAX) {  // both <= 2^24: no overflow
        return false;
    }
    long const total = w * h * bpp;
    if (!read_uint(data, le, &j, cnvs_zlib_bound((int)total), &zlen) ||
        zlen < 1) {
        return false;
    }
    if (!read_uint(data, le, &j, (zlen + 2) / 3, &nlines) || nlines < 1) {
        return false;
    }
    // The colour-space tag is the OPTIONAL trailing token (the sampler converts
    // the resolved sample to the working space on deposit): absent == sRGB, so a
    // legacy (no-token) block parses as sRGB and stays byte-identical.  Only
    // the three colour-space names are valid here; anything else is malformed.
    enum canvas_color_space cs = CANVAS_CS_SRGB;
    if (!at_eol(data, le, j)) {
        if (!read_token(data, le, &j, &ts, &tl)) {
            return false;
        }
        if      (tok_eq(data, le, ts, tl, canvas_color_space_name[CANVAS_CS_SRGB])) {
            cs = CANVAS_CS_SRGB;
        } else if (tok_eq(data, le, ts, tl, canvas_color_space_name[CANVAS_CS_LINEAR_SRGB])) {
            cs = CANVAS_CS_LINEAR_SRGB;
        } else if (tok_eq(data, le, ts, tl, canvas_color_space_name[CANVAS_CS_OKLAB])) {
            cs = CANVAS_CS_OKLAB;
        } else {
            return false;
        }
        if (!at_eol(data, le, j)) {
            return false;
        }
    }
    uint8_t *zs = malloc((size_t)zlen);
    if (!zs) {
        return false;  // OOM while rebuilding: stop replay
    }
    b->bm_zlen = (int)zlen;
    b->bm = zs;  // count before pointer
    b->bm_fid = -1;
    b->bm_img = (int)id;
    b->bm_ct = ct;
    b->bm_at = at;
    b->bm_cs = cs;
    b->bm_total = (int)total;
    b->bm_fill = 0;
    b->bm_lines = (int)nlines;
    b->bm_gid = 0;
    b->bm_w = (int)w;
    b->bm_h = (int)h;
    return true;
}

// path <id> <ncmds> -- begin one numbered Path2D; exactly ncmds command lines
// (one verb each: m/l/q/c with their points, a/e with a trailing winding
// bool, t/r/rr, z) must follow directly.  The path is rebuilt through the
// public canvas_path2d_* builders into a parser-owned object that
// fill_path/stroke_path/clip_path then reference by id.  Strict: an unused id
// (ids declare once), ncmds within the sanity cap.  An empty path (ncmds 0)
// installs immediately.
static bool replay_path(struct replay_blocks *__single b,
                        char const *__counted_by(le) data, size_t le, size_t j) {
    long id = 0, n = 0;
    if (!read_uint(data, le, &j, CNVS_REC_PATHS_MAX - 1, &id) || b->paths[id]) {
        return false;
    }
    if (!read_uint(data, le, &j, REPLAY_PATH_CMDS_MAX, &n)) {
        return false;
    }
    if (!at_eol(data, le, j)) {
        return false;
    }
    struct canvas_path2d *__single p = canvas_path2d();
    if (!p) {
        return false;  // OOM while rebuilding: stop replay
    }
    if (n == 0) {
        b->paths[id] = p;  // an empty path: nothing more to wait for
        return true;
    }
    b->pend_path = p;
    b->pend_id = (int)id;
    b->pend_cmds = (int)n;
    b->pend_left = (int)n;
    return true;
}

// One command line of the pending path block.  The builders never fail loudly
// (an OOM drops the command, the public best-effort posture), so the install
// step verifies the rebuilt path holds exactly the declared command count --
// a silent drop becomes a parse failure, not a silently-different path.
static bool replay_path_cmd(struct replay_blocks *__single b,
                            char const *__counted_by(le) data, size_t le,
                            size_t cs, size_t cl, size_t j) {
    struct canvas_path2d *__single p = b->pend_path;
    float f[7];
    bool w;
    if (tok_eq(data, le, cs, cl, "m")) {
        if (!read_floats(data, le, &j, f, 2)) return false;
        canvas_path2d_move_to(p, f[0], f[1]);
    } else if (tok_eq(data, le, cs, cl, "l")) {
        if (!read_floats(data, le, &j, f, 2)) return false;
        canvas_path2d_line_to(p, f[0], f[1]);
    } else if (tok_eq(data, le, cs, cl, "q")) {
        if (!read_floats(data, le, &j, f, 4)) return false;
        canvas_path2d_quadratic_curve_to(p, f[0], f[1], f[2], f[3]);
    } else if (tok_eq(data, le, cs, cl, "c")) {
        if (!read_floats(data, le, &j, f, 6)) return false;
        canvas_path2d_bezier_curve_to(p, f[0], f[1], f[2], f[3], f[4], f[5]);
    } else if (tok_eq(data, le, cs, cl, "a")) {
        if (!read_floats(data, le, &j, f, 5) || !read_bool(data, le, &j, &w)) return false;
        canvas_path2d_arc(p, f[0], f[1], f[2], f[3], f[4], w);
    } else if (tok_eq(data, le, cs, cl, "e")) {
        if (!read_floats(data, le, &j, f, 7) || !read_bool(data, le, &j, &w)) return false;
        canvas_path2d_ellipse(p, f[0], f[1], f[2], f[3], f[4], f[5], f[6], w);
    } else if (tok_eq(data, le, cs, cl, "t")) {
        if (!read_floats(data, le, &j, f, 5)) return false;
        canvas_path2d_arc_to(p, f[0], f[1], f[2], f[3], f[4]);
    } else if (tok_eq(data, le, cs, cl, "r")) {
        if (!read_floats(data, le, &j, f, 4)) return false;
        canvas_path2d_rect(p, f[0], f[1], f[2], f[3]);
    } else if (tok_eq(data, le, cs, cl, "rr")) {
        if (!read_floats(data, le, &j, f, 5)) return false;
        canvas_path2d_round_rect(p, f[0], f[1], f[2], f[3], f[4]);
    } else if (tok_eq(data, le, cs, cl, "z")) {
        canvas_path2d_close_path(p);
    } else {
        return false;  // not a path command
    }
    if (!at_eol(data, le, j)) {
        return false;
    }
    b->pend_left -= 1;
    if (b->pend_left == 0) {
        if (p->ncmds != b->pend_cmds) {
            return false;  // a builder dropped a command (OOM): stop replay
        }
        b->paths[b->pend_id] = p;
        b->pend_path = NULL;  // ownership moved to the table
        b->pend_id = 0;
        b->pend_cmds = 0;
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

// bits <base64> -- one chunk of the pending bitmap block's deflated stream.
// Strict: only legal while a bitmap block is pending, one token of 4-char
// base64 groups, '=' padding only in the final group of the block's final
// line, the decoded bytes never exceeding the declared zlen -- and the
// block's last line must land zlen exactly, at which point the stream must
// inflate (cnvs_zlib_inflate, itself strict: header, Huffman structure,
// adler, trailing garbage, overflow all reject) to exactly w*h*4 bytes.  Any
// lie -- a truncated stream padded back to length, a valid stream of the
// wrong decoded size -- fails the parse here.  The inflated capture is handed
// to the cache (which takes ownership; an existing entry wins, the usual
// best-effort posture).
static bool replay_bits(struct canvas *__single cv, struct replay_blocks *__single b,
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
        if (fill + nbytes > b->bm_zlen) {
            return false;  // more bytes than the header declared
        }
        uint32_t const v = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) |
                           ((uint32_t)v2 <<  6) |  (uint32_t)v3       ;
        uint8_t const by[3] = { (uint8_t)(v >> 16), (uint8_t)((v >> 8) & 0xFFu),
                                (uint8_t)(v & 0xFFu) };
        for (int k = 0; k < nbytes; k++) {
            b->bm[fill + k] = by[k];
        }
        fill += nbytes;
    }
    b->bm_fill = fill;
    b->bm_lines -= 1;
    if (b->bm_lines == 0) {
        if (fill != b->bm_zlen) {
            return false;  // short block: fewer deflated bytes than declared
        }
        // The whole declared stream landed: now (and only now) allocate the
        // pixels and require the inflate to fill them exactly.
        uint8_t *px = malloc((size_t)b->bm_total);
        if (!px) {
            return false;  // OOM while rebuilding: stop replay
        }
        if (cnvs_zlib_inflate(px, b->bm_total, b->bm, b->bm_zlen) !=
            b->bm_total) {
            free(px);
            return false;  // malformed zlib, or a valid stream of the wrong size
        }
        if (b->bm_img >= 0) {
            // An `image` block: the canvas adopts the pixels (patterns borrow
            // them beyond replay), and the table keeps a borrowed view for
            // the ops that reference the id.
            if (!cnvs_canvas_own_image(cv, px, b->bm_total)) {
                free(px);
                return false;  // OOM while rebuilding: stop replay
            }
            b->img[b->bm_img].px = px;
            b->img[b->bm_img].len = b->bm_total;
            b->img[b->bm_img].w = b->bm_w;
            b->img[b->bm_img].h = b->bm_h;
            b->img[b->bm_img].ct = b->bm_ct;
            b->img[b->bm_img].at = b->bm_at;
            b->img[b->bm_img].cs = b->bm_cs;
            b->img[b->bm_img].mips = false;
        } else if (b->bm_fid >= 0) {
            cnvs_text_cache_put_capture(cnvs_canvas_text_cache(cv), b->bm_fid,
                                        (uint16_t)b->bm_gid, px, b->bm_total,
                                        b->bm_w, b->bm_h, b->bm_ink[0],
                                        b->bm_ink[1], b->bm_ink[2],
                                        b->bm_ink[3]);
        } else {
            free(px);  // fully validated; a degraded intern has nowhere to land
        }
        bitmap_drop(b);
    }
    return true;
}

// shaping <size_px> <rtl 0|1> <ls> <ws> <utf16-len> <nruns> <byte-len>
// <text...> -- begin one shaped line; its `run` lines must follow immediately.
// rtl is the paragraph direction the line was shaped under, ls/ws the
// letterSpacing/wordSpacing -- the other three halves of the cache key, since
// the same bytes shape (and space) differently under each.  ls/ws are always
// present (the recorder writes them even when 0); the spacing is already baked
// into the run advances, so they serve only to key the rebuilt line.  The text
// is exactly byte-len raw bytes after a single separating space (it is the cache
// key, byte for byte).  Strict: finite size/ls/ws, utf16-len <= byte-len (every
// UTF-16 unit costs at least one UTF-8 byte), and the byte count exactly fills
// the line.
static bool replay_shaping(struct canvas *__single cv, struct replay_blocks *__single b,
                         char const *__counted_by(le) data, size_t le, size_t j) {
    float size = 0.0f, ls = 0.0f, ws = 0.0f;
    bool rtl = false;
    long t16 = 0, nruns = 0, blen = 0;
    if (!read_float(data, le, &j, &size) || !isfinite(size) || size < 0.0f) {
        return false;
    }
    if (!read_bool(data, le, &j, &rtl)) {
        return false;
    }
    if (!read_float(data, le, &j, &ls) || !isfinite(ls) ||
        !read_float(data, le, &j, &ws) || !isfinite(ws)) {
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
    struct cnvs_shaped *s = calloc(1, sizeof *s);
    if (!s) {
        return false;
    }
    s->size_px = size;
    s->utf16s = (int)t16;
    if (nruns > 0) {
        struct cnvs_glyph_run *runs = calloc((size_t)nruns, sizeof *runs);
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
    b->rtl = rtl;
    b->ls = ls;
    b->ws = ws;
    b->text = txt;
    b->text_len = (int)blen;
    if (nruns == 0) {
        blocks_finish_shaping(cv, b);  // nothing more to wait for
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
static bool replay_run(struct canvas *__single cv, struct replay_blocks *__single b,
                       char const *__counted_by(le) data, size_t le, size_t j) {
    if (!b->s) {
        return false;  // no shaping block pending
    }
    int name_id = -1;
    size_t j0 = j, ts = 0, tl = 0;
    if (!read_token(data, le, &j, &ts, &tl)) {
        return false;
    }
    if (!tok_eq(data, le, ts, tl, "-1")) {  // -1 = an unkeyed (or color) run
        long file_id = 0;
        j = j0;
        if (!read_uint(data, le, &j, CNVS_FONT_INTERN_N - 1, &file_id) ||
            !b->seen[file_id]) {
            return false;
        }
        name_id = b->map[file_id];  // -1 when interning degraded
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
                 read_uint(data, le, &j, (long)b->s->utf16s - 1, &cluster);
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
    struct cnvs_glyph_run *run = &b->s->run[b->runs_done];
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
        blocks_finish_shaping(cv, b);
    }
    return true;
}

// Next token parsed as a declared image-block id: the table entry to draw or
// tile from.  False for an out-of-range or never-declared id.
static bool read_image_id(struct replay_blocks *__single b,
                          char const *__counted_by(le) data, size_t le,
                          size_t *__single jp, int *__single out) {
    long id = 0;
    if (!read_uint(data, le, jp, CNVS_REC_IMAGES_MAX - 1, &id) ||
        !b->img[id].px) {
        return false;
    }
    *out = (int)id;
    return true;
}

// An f16 image block is carried as bytes (the format's image buffers are all
// byte buffers, read element-by-element); the f16 putImageData API speaks
// aligned _Float16.  These copy the block's w*h*4*2 bytes into an aligned
// _Float16 buffer (element count len == bytes/2) and call the API, the byte
// buffer never reinterpret-cast.  False on OOM (replay stops, the canvas stays
// valid -- the recorder's caps bound the block at CNVS_REC_IMAGE_BYTES_MAX).
static bool replay_put_f16(struct canvas *__single cv, struct replay_image const *__single im,
                           int dx, int dy) {
    int const len = im->len / (int)sizeof(_Float16);
    _Float16 *__counted_by_or_null(len) px = malloc((size_t)im->len);
    if (!px) {
        return false;
    }
    memcpy(px, im->px, (size_t)im->len);
    canvas_put_image_data_f16(cv, im->cs, px, len, im->w, im->h, dx, dy);
    free(px);
    return true;
}

static bool replay_put_dirty_f16(struct canvas *__single cv,
                                 struct replay_image const *__single im,
                                 int dx, int dy,
                                 int dirty_x, int dirty_y, int dirty_w, int dirty_h) {
    int const len = im->len / (int)sizeof(_Float16);
    _Float16 *__counted_by_or_null(len) px = malloc((size_t)im->len);
    if (!px) {
        return false;
    }
    memcpy(px, im->px, (size_t)im->len);
    canvas_put_image_data_dirty_f16(cv, im->cs, px, len, im->w, im->h, dx, dy,
                                    dirty_x, dirty_y, dirty_w, dirty_h);
    free(px);
    return true;
}

static bool replay_line(struct canvas *__single cv, struct replay_blocks *__single blk,
                        char const *__counted_by(le) data, size_t ls, size_t le) {
    size_t j = ls;
    size_t cs, cl;
    if (!read_token(data, le, &j, &cs, &cl)) {
        return blk->s == NULL && blk->bm_lines == 0 &&
               blk->pend_path == NULL;  // blank line (illegal inside a block)
    }
    if (data[cs] == '#') {
        return blk->s == NULL && blk->bm_lines == 0 &&
               blk->pend_path == NULL;  // comment line (ditto)
    }
    if (blk->s && !tok_eq(data, le, cs, cl, "run")) {
        return false;  // a shaping block's run lines must follow it directly
    }
    if (blk->bm_lines > 0 && !tok_eq(data, le, cs, cl, "bits")) {
        return false;  // a bitmap/image block's bits lines must follow directly
    }
    if (blk->pend_path) {
        // A path block's command lines must follow it directly; anything that
        // is not a command rejects there.
        return replay_path_cmd(blk, data, le, cs, cl, j);
    }

    // --- working space (leads the file; reconfigures the fresh canvas) ---
    // Only a non-sRGB program ever writes this line, and only as its first
    // command -- it reconfigures the canvas's immutable working space before any
    // colour interns.  Rejected if anything has been drawn yet (`started`), so a
    // malformed file can't flip the space mid-stream; the canvas is still all-
    // zero transparent here, identical in either space, so the reconfigure is
    // exactly creation-time.  Absence means sRGB (the latched-on-first-op
    // default), keeping every existing program byte-stable.
    if (tok_eq(data, le, cs, cl, "working_space")) {
        if (blk->started) {
            return false;  // must lead the file
        }
        size_t ts, tl;
        if (!read_token(data, le, &j, &ts, &tl) || !at_eol(data, le, j)) {
            return false;
        }
        // Only the two compositing spaces are valid tokens here; Oklab shares
        // the colour-space name table but is not a working space, so its token
        // is rejected like any other unknown name (the strict-parser contract).
        enum canvas_color_space sp;
        if      (tok_eq(data, le, ts, tl, canvas_color_space_name[CANVAS_CS_SRGB])) {
            sp = CANVAS_CS_SRGB;
        } else if (tok_eq(data, le, ts, tl, canvas_color_space_name[CANVAS_CS_LINEAR_SRGB])) {
            sp = CANVAS_CS_LINEAR_SRGB;
        } else {
            return false;
        }
        blk->started = true;
        return cnvs_canvas_set_working_space(cv, sp);
    }
    blk->started = true;  // any other effective command locks the space in

    float f[8];
    bool b;

    // --- no-arg ---
    if (tok_eq(data, le, cs, cl, "save"))                 { canvas_save(cv); }
    else if (tok_eq(data, le, cs, cl, "restore"))         { canvas_restore(cv); }
    else if (tok_eq(data, le, cs, cl, "reset_transform")) { canvas_reset_transform(cv); }
    else if (tok_eq(data, le, cs, cl, "begin_path"))      { canvas_begin_path(cv); }
    else if (tok_eq(data, le, cs, cl, "close_path"))      { canvas_close_path(cv); }
    else if (tok_eq(data, le, cs, cl, "stroke"))          { canvas_stroke(cv); }
    else if (tok_eq(data, le, cs, cl, "set_filter_none")) { canvas_set_filter_none(cv); }
    else if (tok_eq(data, le, cs, cl, "reset")) {
        // reset clears the text cache, and with it every interned font id:
        // the file's font-id space restarts too (the recorder re-interns from
        // 0 after its own reset), so forget the declarations.  The numbered
        // images and paths are file-scoped, not canvas drawing state, and
        // keep their ids.
        canvas_reset(cv);
        for (int k = 0; k < CNVS_FONT_INTERN_N; k++) {
            blk->map[k] = -1;
            blk->seen[k] = false;
        }
    }

    // --- resize (records only when it succeeded, so dims always validate;
    //     it resets, restarting the font-id space like `reset`) ---
    else if (tok_eq(data, le, cs, cl, "resize")) {
        long w = 0, h = 0;
        if (!read_uint(data, le, &j, REPLAY_RESIZE_DIM_MAX, &w) || w < 1 ||
            !read_uint(data, le, &j, REPLAY_RESIZE_DIM_MAX, &h) || h < 1) {
            return false;
        }
        if (!canvas_resize(cv, (int)w, (int)h)) {
            return false;  // dims pre-validated: only allocation can fail
        }
        for (int k = 0; k < CNVS_FONT_INTERN_N; k++) {
            blk->map[k] = -1;
            blk->seen[k] = false;
        }
    }

    // --- 1 float ---
    else if (tok_eq(data, le, cs, cl, "rotate"))                { if (!read_floats(data, le, &j, f, 1)) return false; canvas_rotate(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "set_global_alpha"))      { if (!read_floats(data, le, &j, f, 1)) return false; canvas_set_global_alpha(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "set_line_width"))        { if (!read_floats(data, le, &j, f, 1)) return false; canvas_set_line_width(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "set_miter_limit"))       { if (!read_floats(data, le, &j, f, 1)) return false; canvas_set_miter_limit(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "set_line_dash_offset"))  { if (!read_floats(data, le, &j, f, 1)) return false; canvas_set_line_dash_offset(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "set_font_size"))         { if (!read_floats(data, le, &j, f, 1)) return false; canvas_set_font_size(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "set_shadow_blur"))       { if (!read_floats(data, le, &j, f, 1)) return false; canvas_set_shadow_blur(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "set_shadow_offset_x"))   { if (!read_floats(data, le, &j, f, 1)) return false; canvas_set_shadow_offset_x(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "set_shadow_offset_y"))   { if (!read_floats(data, le, &j, f, 1)) return false; canvas_set_shadow_offset_y(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "add_filter_blur"))       { if (!read_floats(data, le, &j, f, 1)) return false; canvas_add_filter_blur(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "add_filter_brightness")) { if (!read_floats(data, le, &j, f, 1)) return false; canvas_add_filter_brightness(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "add_filter_contrast"))   { if (!read_floats(data, le, &j, f, 1)) return false; canvas_add_filter_contrast(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "add_filter_grayscale"))  { if (!read_floats(data, le, &j, f, 1)) return false; canvas_add_filter_grayscale(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "add_filter_hue_rotate")) { if (!read_floats(data, le, &j, f, 1)) return false; canvas_add_filter_hue_rotate(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "add_filter_invert"))     { if (!read_floats(data, le, &j, f, 1)) return false; canvas_add_filter_invert(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "add_filter_opacity"))    { if (!read_floats(data, le, &j, f, 1)) return false; canvas_add_filter_opacity(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "add_filter_saturate"))   { if (!read_floats(data, le, &j, f, 1)) return false; canvas_add_filter_saturate(cv, f[0]); }
    else if (tok_eq(data, le, cs, cl, "add_filter_sepia"))      { if (!read_floats(data, le, &j, f, 1)) return false; canvas_add_filter_sepia(cv, f[0]); }

    // --- 2 float ---
    else if (tok_eq(data, le, cs, cl, "translate")) { if (!read_floats(data, le, &j, f, 2)) return false; canvas_translate(cv, f[0], f[1]); }
    else if (tok_eq(data, le, cs, cl, "scale"))     { if (!read_floats(data, le, &j, f, 2)) return false; canvas_scale(cv, f[0], f[1]); }
    else if (tok_eq(data, le, cs, cl, "move_to"))   { if (!read_floats(data, le, &j, f, 2)) return false; canvas_move_to(cv, f[0], f[1]); }
    else if (tok_eq(data, le, cs, cl, "line_to"))   { if (!read_floats(data, le, &j, f, 2)) return false; canvas_line_to(cv, f[0], f[1]); }
    // --- 3 float ---
    else if (tok_eq(data, le, cs, cl, "set_fill_conic_gradient"))   { if (!read_floats(data, le, &j, f, 3)) return false; canvas_set_fill_conic_gradient(cv, f[0], f[1], f[2]); }
    else if (tok_eq(data, le, cs, cl, "set_stroke_conic_gradient")) { if (!read_floats(data, le, &j, f, 3)) return false; canvas_set_stroke_conic_gradient(cv, f[0], f[1], f[2]); }

    // --- 4 float ---
    else if (tok_eq(data, le, cs, cl, "rect"))                       { if (!read_floats(data, le, &j, f, 4)) return false; canvas_rect(cv, f[0], f[1], f[2], f[3]); }
    else if (tok_eq(data, le, cs, cl, "clear_rect"))                 { if (!read_floats(data, le, &j, f, 4)) return false; canvas_clear_rect(cv, f[0], f[1], f[2], f[3]); }
    else if (tok_eq(data, le, cs, cl, "fill_rect"))                  { if (!read_floats(data, le, &j, f, 4)) return false; canvas_fill_rect(cv, f[0], f[1], f[2], f[3]); }
    else if (tok_eq(data, le, cs, cl, "stroke_rect"))                { if (!read_floats(data, le, &j, f, 4)) return false; canvas_stroke_rect(cv, f[0], f[1], f[2], f[3]); }
    else if (tok_eq(data, le, cs, cl, "set_fill_rgba"))              { enum canvas_color_space sp; if (!read_floats(data, le, &j, f, 4) || !read_opt_cs(data, le, &j, &sp)) return false; canvas_set_fill_rgba(cv, sp, f[0], f[1], f[2], f[3]); return true; }
    else if (tok_eq(data, le, cs, cl, "set_stroke_rgba"))            { enum canvas_color_space sp; if (!read_floats(data, le, &j, f, 4) || !read_opt_cs(data, le, &j, &sp)) return false; canvas_set_stroke_rgba(cv, sp, f[0], f[1], f[2], f[3]); return true; }
    else if (tok_eq(data, le, cs, cl, "set_shadow_color_rgba"))      { enum canvas_color_space sp; if (!read_floats(data, le, &j, f, 4) || !read_opt_cs(data, le, &j, &sp)) return false; canvas_set_shadow_color_rgba(cv, sp, f[0], f[1], f[2], f[3]); return true; }
    else if (tok_eq(data, le, cs, cl, "quadratic_curve_to"))         { if (!read_floats(data, le, &j, f, 4)) return false; canvas_quadratic_curve_to(cv, f[0], f[1], f[2], f[3]); }
    else if (tok_eq(data, le, cs, cl, "set_fill_linear_gradient"))   { if (!read_floats(data, le, &j, f, 4)) return false; canvas_set_fill_linear_gradient(cv, f[0], f[1], f[2], f[3]); }
    else if (tok_eq(data, le, cs, cl, "set_stroke_linear_gradient")) { if (!read_floats(data, le, &j, f, 4)) return false; canvas_set_stroke_linear_gradient(cv, f[0], f[1], f[2], f[3]); }

    // --- 5 float ---
    else if (tok_eq(data, le, cs, cl, "round_rect"))            { if (!read_floats(data, le, &j, f, 5)) return false; canvas_round_rect(cv, f[0], f[1], f[2], f[3], f[4]); }
    else if (tok_eq(data, le, cs, cl, "arc_to"))                { if (!read_floats(data, le, &j, f, 5)) return false; canvas_arc_to(cv, f[0], f[1], f[2], f[3], f[4]); }
    else if (tok_eq(data, le, cs, cl, "add_fill_color_stop"))   { enum canvas_color_space sp; if (!read_floats(data, le, &j, f, 5) || !read_opt_cs(data, le, &j, &sp)) return false; canvas_add_fill_color_stop(cv, sp, f[0], f[1], f[2], f[3], f[4]); return true; }
    else if (tok_eq(data, le, cs, cl, "add_stroke_color_stop")) { enum canvas_color_space sp; if (!read_floats(data, le, &j, f, 5) || !read_opt_cs(data, le, &j, &sp)) return false; canvas_add_stroke_color_stop(cv, sp, f[0], f[1], f[2], f[3], f[4]); return true; }

    // --- 6 float ---
    else if (tok_eq(data, le, cs, cl, "transform"))                  { if (!read_floats(data, le, &j, f, 6)) return false; canvas_transform(cv, f[0], f[1], f[2], f[3], f[4], f[5]); }
    else if (tok_eq(data, le, cs, cl, "set_transform"))              { if (!read_floats(data, le, &j, f, 6)) return false; canvas_set_transform(cv, f[0], f[1], f[2], f[3], f[4], f[5]); }
    else if (tok_eq(data, le, cs, cl, "bezier_curve_to"))            { if (!read_floats(data, le, &j, f, 6)) return false; canvas_bezier_curve_to(cv, f[0], f[1], f[2], f[3], f[4], f[5]); }
    else if (tok_eq(data, le, cs, cl, "set_fill_radial_gradient"))   { if (!read_floats(data, le, &j, f, 6)) return false; canvas_set_fill_radial_gradient(cv, f[0], f[1], f[2], f[3], f[4], f[5]); }
    else if (tok_eq(data, le, cs, cl, "set_stroke_radial_gradient")) { if (!read_floats(data, le, &j, f, 6)) return false; canvas_set_stroke_radial_gradient(cv, f[0], f[1], f[2], f[3], f[4], f[5]); }

    // --- 7 float ---
    else if (tok_eq(data, le, cs, cl, "add_filter_drop_shadow")) {
        enum canvas_color_space sp;
        if (!read_floats(data, le, &j, f, 7) || !read_opt_cs(data, le, &j, &sp)) return false;
        canvas_add_filter_drop_shadow(cv, sp, f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
        return true;
    }

    // --- 12 float ---
    else if (tok_eq(data, le, cs, cl, "round_rect_radii")) {
        float rr[12];
        if (!read_floats(data, le, &j, rr, 12)) return false;
        canvas_round_rect_radii(cv, rr[0], rr[1], rr[2], rr[3], rr[4], rr[5],
                                rr[6], rr[7], rr[8], rr[9], rr[10], rr[11]);
    }

    // --- float run + trailing bool ---
    else if (tok_eq(data, le, cs, cl, "arc")) {
        if (!read_floats(data, le, &j, f, 5) || !read_bool(data, le, &j, &b)) return false;
        canvas_arc(cv, f[0], f[1], f[2], f[3], f[4], b);
    }
    else if (tok_eq(data, le, cs, cl, "ellipse")) {
        if (!read_floats(data, le, &j, f, 7) || !read_bool(data, le, &j, &b)) return false;
        canvas_ellipse(cv, f[0], f[1], f[2], f[3], f[4], f[5], f[6], b);
    }
    else if (tok_eq(data, le, cs, cl, "set_image_smoothing_enabled")) {
        if (!read_bool(data, le, &j, &b)) return false;
        canvas_set_image_smoothing_enabled(cv, b);
    }

    // --- enums by name ---
    else if (tok_eq(data, le, cs, cl, "fill") || tok_eq(data, le, cs, cl, "clip")) {
        bool fill = data[cs] == 'f';
        size_t ts, tl;
        if (!read_token(data, le, &j, &ts, &tl)) return false;
        enum canvas_fill_rule rule;
        if (tok_eq(data, le, ts, tl, "nonzero"))      rule = CANVAS_NONZERO;
        else if (tok_eq(data, le, ts, tl, "evenodd")) rule = CANVAS_EVENODD;
        else return false;
        if (fill) canvas_fill(cv, rule);
        else      canvas_clip(cv, rule);
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
    else if (tok_eq(data, le, cs, cl, "set_direction")) {
        size_t ts, tl;
        if (!read_token(data, le, &j, &ts, &tl)) return false;
        if (tok_eq(data, le, ts, tl, "ltr"))      canvas_set_direction(cv, CANVAS_DIRECTION_LTR);
        else if (tok_eq(data, le, ts, tl, "rtl")) canvas_set_direction(cv, CANVAS_DIRECTION_RTL);
        else return false;
    }
    else if (tok_eq(data, le, cs, cl, "set_fill_gradient_interpolation") ||
             tok_eq(data, le, cs, cl, "set_stroke_gradient_interpolation")) {
        bool const fill = data[cs + 4] == 'f';  // "set_[f]ill" vs "set_[s]troke"
        size_t ts, tl;
        // SPACE token: any of the three colour spaces is a valid interp space.
        if (!read_token(data, le, &j, &ts, &tl)) return false;
        enum canvas_color_space space;
        if (tok_eq(data, le, ts, tl, canvas_color_space_name[CANVAS_CS_SRGB])) {
            space = CANVAS_CS_SRGB;
        } else if (tok_eq(data, le, ts, tl, canvas_color_space_name[CANVAS_CS_LINEAR_SRGB])) {
            space = CANVAS_CS_LINEAR_SRGB;
        } else if (tok_eq(data, le, ts, tl, canvas_color_space_name[CANVAS_CS_OKLAB])) {
            space = CANVAS_CS_OKLAB;
        } else {
            return false;
        }
        // ALPHA token: premultiply the colour coords before the lerp, or not.
        if (!read_token(data, le, &j, &ts, &tl)) return false;
        enum canvas_alpha_type alpha;
        if (tok_eq(data, le, ts, tl, "unpremul"))    alpha = CANVAS_ALPHA_UNPREMUL;
        else if (tok_eq(data, le, ts, tl, "premul")) alpha = CANVAS_ALPHA_PREMUL;
        else return false;
        if (fill) canvas_set_fill_gradient_interpolation(cv, space, alpha);
        else      canvas_set_stroke_gradient_interpolation(cv, space, alpha);
    }
    else if (tok_eq(data, le, cs, cl, "set_image_smoothing_quality")) {
        size_t ts, tl;
        if (!read_token(data, le, &j, &ts, &tl)) return false;
        if (tok_eq(data, le, ts, tl, "low"))         canvas_set_image_smoothing_quality(cv, CANVAS_SMOOTHING_LOW);
        else if (tok_eq(data, le, ts, tl, "medium")) canvas_set_image_smoothing_quality(cv, CANVAS_SMOOTHING_MEDIUM);
        else if (tok_eq(data, le, ts, tl, "high"))   canvas_set_image_smoothing_quality(cv, CANVAS_SMOOTHING_HIGH);
        else return false;
    }
    else if (tok_eq(data, le, cs, cl, "set_global_composite_operation")) {
        size_t ts, tl;
        if (!read_token(data, le, &j, &ts, &tl)) return false;
        int op = -1;
        for (int k = 0; k < (int)(sizeof cnvs_composite_name / sizeof cnvs_composite_name[0]); k++) {
            if (tok_eq(data, le, ts, tl, cnvs_composite_name[k])) { op = k; break; }
        }
        if (op < 0) return false;
        canvas_set_global_composite_operation(cv, (enum canvas_composite_op)op);
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
    else if (tok_eq(data, le, cs, cl, "shaping")) { return replay_shaping(cv, blk, data, le, j); }
    else if (tok_eq(data, le, cs, cl, "run"))    { return replay_run(cv, blk, data, le, j); }

    // --- image blocks + the ops that reference them by id ---
    else if (tok_eq(data, le, cs, cl, "image"))  { return replay_image(blk, data, le, j); }
    else if (tok_eq(data, le, cs, cl, "image_mips")) {
        int id;
        if (!read_image_id(blk, data, le, &j, &id)) return false;
        blk->img[id].mips = true;
    }

    // --- path blocks + the Path2D ops that reference them by id ---
    else if (tok_eq(data, le, cs, cl, "path"))   { return replay_path(blk, data, le, j); }
    else if (tok_eq(data, le, cs, cl, "fill_path") ||
             tok_eq(data, le, cs, cl, "clip_path")) {
        bool fill = data[cs] == 'f';
        long id = 0;
        size_t ts, tl;
        if (!read_uint(data, le, &j, CNVS_REC_PATHS_MAX - 1, &id) ||
            !blk->paths[id] || !read_token(data, le, &j, &ts, &tl)) return false;
        enum canvas_fill_rule rule;
        if (tok_eq(data, le, ts, tl, "nonzero"))      rule = CANVAS_NONZERO;
        else if (tok_eq(data, le, ts, tl, "evenodd")) rule = CANVAS_EVENODD;
        else return false;
        if (fill) canvas_fill_path(cv, blk->paths[id], rule);
        else      canvas_clip_path(cv, blk->paths[id], rule);
    }
    else if (tok_eq(data, le, cs, cl, "stroke_path")) {
        long id = 0;
        if (!read_uint(data, le, &j, CNVS_REC_PATHS_MAX - 1, &id) ||
            !blk->paths[id]) return false;
        canvas_stroke_path(cv, blk->paths[id]);
    }
    else if (tok_eq(data, le, cs, cl, "draw_image")) {
        int id;
        if (!read_image_id(blk, data, le, &j, &id) ||
            !read_floats(data, le, &j, f, 2)) return false;
        struct replay_image const im = blk->img[id];
        cnvs_canvas_draw_block(cv, im.px, im.len, im.w, im.h, im.ct,
                               im.at, im.cs, im.mips, 0, 0.0f, 0.0f, (float)im.w,
                               (float)im.h, f[0], f[1], (float)im.w,
                               (float)im.h);
    }
    else if (tok_eq(data, le, cs, cl, "draw_image_scaled")) {
        int id;
        if (!read_image_id(blk, data, le, &j, &id) ||
            !read_floats(data, le, &j, f, 4)) return false;
        struct replay_image const im = blk->img[id];
        cnvs_canvas_draw_block(cv, im.px, im.len, im.w, im.h, im.ct,
                               im.at, im.cs, im.mips, 1, 0.0f, 0.0f, (float)im.w,
                               (float)im.h, f[0], f[1], f[2], f[3]);
    }
    else if (tok_eq(data, le, cs, cl, "draw_image_subrect")) {
        int id;
        if (!read_image_id(blk, data, le, &j, &id) ||
            !read_floats(data, le, &j, f, 8)) return false;
        struct replay_image const im = blk->img[id];
        cnvs_canvas_draw_block(cv, im.px, im.len, im.w, im.h, im.ct,
                               im.at, im.cs, im.mips, 2, f[0], f[1], f[2], f[3],
                               f[4], f[5], f[6], f[7]);
    }
    else if (tok_eq(data, le, cs, cl, "put_image_data")) {
        int id;
        long v[2];
        if (!read_image_id(blk, data, le, &j, &id) ||
            !read_ints(data, le, &j, INT_MAX, v, 2)) return false;
        // putImageData is straight (unpremultiplied) by contract; the colour
        // TYPE on the block picks the API.  The input's colour space rode the
        // block's optional colour-space tag (absent == sRGB), so replay
        // re-applies whatever the block declared -- an untagged sRGB unorm8 put
        // stays byte-identical, a tagged or f16 one round-trips.  An alpha type
        // other than UNPREMUL, or a colour type that is neither flavour, is a
        // malformed file.
        struct replay_image const im = blk->img[id];
        if (im.at != CANVAS_ALPHA_UNPREMUL) return false;
        if (im.ct == CANVAS_COLOR_UNORM8) {
            canvas_put_image_data(cv, im.cs, im.px, im.len, im.w, im.h,
                                  (int)v[0], (int)v[1]);
        } else if (im.ct == CANVAS_COLOR_F16) {
            if (!replay_put_f16(cv, &blk->img[id], (int)v[0], (int)v[1])) return false;
        } else {
            return false;
        }
    }
    else if (tok_eq(data, le, cs, cl, "put_image_data_dirty")) {
        int id;
        long v[6];
        if (!read_image_id(blk, data, le, &j, &id) ||
            !read_ints(data, le, &j, INT_MAX, v, 6)) return false;
        // The colour type picks the API and the colour space rode the block's
        // optional tag, exactly as for put_image_data above.
        struct replay_image const im = blk->img[id];
        if (im.at != CANVAS_ALPHA_UNPREMUL) return false;
        if (im.ct == CANVAS_COLOR_UNORM8) {
            canvas_put_image_data_dirty(cv, im.cs, im.px, im.len, im.w, im.h,
                                        (int)v[0], (int)v[1], (int)v[2], (int)v[3],
                                        (int)v[4], (int)v[5]);
        } else if (im.ct == CANVAS_COLOR_F16) {
            if (!replay_put_dirty_f16(cv, &blk->img[id], (int)v[0], (int)v[1],
                                      (int)v[2], (int)v[3], (int)v[4], (int)v[5]))
                return false;
        } else {
            return false;
        }
    }
    else if (tok_eq(data, le, cs, cl, "set_fill_pattern") ||
             tok_eq(data, le, cs, cl, "set_stroke_pattern")) {
        bool fill = data[cs + 4] == 'f';  // set_[f]ill vs set_[s]troke
        int id;
        size_t ts, tl;
        if (!read_image_id(blk, data, le, &j, &id) ||
            !read_token(data, le, &j, &ts, &tl)) return false;
        // Patterns borrow straight unorm8; same malformed-file posture as
        // put_image_data above.
        if (blk->img[id].ct != CANVAS_COLOR_UNORM8 ||
            blk->img[id].at != CANVAS_ALPHA_UNPREMUL) return false;
        int rep = -1;
        for (int k = 0; k < (int)(sizeof cnvs_repeat_name / sizeof cnvs_repeat_name[0]); k++) {
            if (tok_eq(data, le, ts, tl, cnvs_repeat_name[k])) { rep = k; break; }
        }
        if (rep < 0) return false;
        // The block's colour-space tag rides through to the pattern setter, so
        // a non-sRGB pattern round-trips (the sampler converts the resolved
        // sample to the working space on deposit).
        if (fill) {
            canvas_set_fill_pattern(cv, blk->img[id].cs, blk->img[id].px,
                                    blk->img[id].w, blk->img[id].h,
                                    (enum canvas_pattern_repeat)rep);
        } else {
            canvas_set_stroke_pattern(cv, blk->img[id].cs, blk->img[id].px,
                                      blk->img[id].w, blk->img[id].h,
                                      (enum canvas_pattern_repeat)rep);
        }
    }

    // --- text tail (rest of line, UTF-8) ---
    else if (tok_eq(data, le, cs, cl, "fill_text") || tok_eq(data, le, cs, cl, "stroke_text")) {
        bool fill = data[cs] == 'f';
        if (!read_floats(data, le, &j, f, 2)) return false;
        while (j < le && is_ws(data[j])) { j++; }
        // The text is the rest of the line: hand the slice straight to the
        // length-counted text API -- no copy, no NUL.
        int n = (int)(le - j);  // le - j <= line cap < INT_MAX
        if (fill) canvas_fill_text_n(cv, data + j, n, f[0], f[1]);
        else      canvas_stroke_text_n(cv, data + j, n, f[0], f[1]);
        return true;  // text consumed the rest of the line
    }
    else if (tok_eq(data, le, cs, cl, "fill_text_max") ||
             tok_eq(data, le, cs, cl, "stroke_text_max")) {
        // Like fill_text, plus a max_width float between the pen and the text.
        bool const fill = data[cs] == 'f';
        if (!read_floats(data, le, &j, f, 3)) return false;
        while (j < le && is_ws(data[j])) { j++; }
        int const n = (int)(le - j);  // le - j <= line cap < INT_MAX
        if (fill) canvas_fill_text_max_n(cv, data + j, n, f[0], f[1], f[2]);
        else      canvas_stroke_text_max_n(cv, data + j, n, f[0], f[1], f[2]);
        return true;  // text consumed the rest of the line
    }

    else {
        return false;  // unknown command
    }

    return at_eol(data, le, j);  // fixed-arity commands: nothing should follow
}

bool cnvs_replay_text(struct canvas *__single cv, char const *__counted_by(len) data, size_t len) {
    struct replay_blocks b = { .s = NULL, .runs_done = 0, .size_px = 0.0f,
                               .rtl = false,
                               .text_len = 0, .text = NULL, .bm = NULL,
                               .bm_zlen = 0, .bm_total = 0,
                               .bm_fill = 0,
                               .bm_lines = 0, .bm_fid = -1, .bm_img = -1,
                               .bm_gid = 0,
                               .bm_w = 0, .bm_h = 0, .bm_ink = { 0 },
                               .img = { { .px = NULL, .len = 0, .w = 0, .h = 0 } },
                               .paths = { NULL }, .pend_path = NULL,
                               .pend_id = 0, .pend_cmds = 0, .pend_left = 0 };
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
    if (b.s || b.bm_lines > 0 || b.pend_path) {
        ok = false;  // truncated shaping/bitmap/path block: its run/bits/command
    }                // lines never arrived
    blocks_drop(&b);
    bitmap_drop(&b);
    paths_drop(&b);
    return ok;
}

bool canvas_replay_from(struct canvas *__single cv, char const *__null_terminated path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return false;
    }
    long const sz = ftell(f);
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
    size_t const got = fread(buf, 1, n, f);
    (void)fclose(f);
    bool const ok = cnvs_replay_text(cv, buf, got);
    free(buf);
    return ok;
}
