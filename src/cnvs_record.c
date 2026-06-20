// Text canvas-program recorder (cnvs_record.h).  Writes the same one-command-per-
// line format cnvs_replay.c parses, straight to a FILE*; canvas.c calls these from
// each recordable public op.  Everything here is plain bounds-safe C -- counted
// float runs, __null_terminated names/text -- so the write side plays by the same
// -fbounds-safety rules as the read side.

#include "cnvs_record.h"

#include "cnvs_path2d.h"
#include "cnvs_zlib.h"

#include <ptrcheck.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One content-deduped `image` block already in this file: an owned copy of
// the pixels for the content compare (the caller's buffer is borrowed and may
// be freed or mutated between the ops that reference it).  The entry's index
// is its file-local id.
struct rec_image {
    uint8_t *__counted_by(len) px;
    int len;
    int w, h;
    enum canvas_color_type ct;  // the block's colour type, written by name
    enum canvas_alpha_type at;  // ...and its alpha type, likewise
    enum canvas_color_space cs; // ...and its colour-space tag, likewise
    bool mips_done;  // an `image_mips` line has been emitted for this id
};

// Likewise one `path` block: an owned copy of the Path2D's command list --
// content, not pointer identity, is the key, since the caller's object may be
// mutated (or freed and another allocated at the same address) between draws.
struct rec_path {
    p2d_cmd *__counted_by(ncmds) cmds;
    int ncmds;
};

// The text format's enum spellings (cnvs_record.h): written here, parsed by
// cnvs_replay.c, one table for both directions.
char const *const cnvs_composite_name[CANVAS_OP_LUMINOSITY + 1] = {
    "source-over", "source-in", "source-out", "source-atop", "destination-over",
    "destination-in", "destination-out", "destination-atop", "xor", "lighter",
    "copy", "multiply", "screen", "overlay", "darken", "lighten", "color-dodge",
    "color-burn", "hard-light", "soft-light", "difference", "exclusion", "hue",
    "saturation", "color", "luminosity",
};
char const *const cnvs_repeat_name[CANVAS_NO_REPEAT + 1] = {
    "repeat", "repeat-x", "repeat-y", "no-repeat",
};
// One table for every colour-space token in the format -- the working_space
// line, the colour-op lines, the gradient-interpolation lines, and the image
// blocks all draw their names here, indexed by enum value.  The spellings are
// the on-disk contract.
char const *const canvas_color_space_name[CANVAS_CS_OKLAB + 1] = {
    "srgb", "linear", "oklab",
};
// The alpha-mode tokens, indexed by enum value, mirroring the colour-space
// table.  A gradient op line carries one of these unconditionally (the four
// image-block formats spell the same tokens inline, the equal-peers posture).
char const *const cnvs_alpha_type_name[CANVAS_ALPHA_PREMUL + 1] = {
    "unpremul", "premul",
};

struct cnvs_recorder {
    FILE *__single f;
    int suspend;  // >0 while a compound op's sub-calls are being swallowed
    // The working space leads the file, written unconditionally.  It is PENDING
    // until the first other line is recorded: record_to latches the canvas's
    // creation space, and a leading replay `working_space` line (the only thing
    // that reconfigures the immutable space, and only before any draw) updates
    // the pending value -- so a replay-while-recording records the space the
    // file actually replays in, not the recording canvas's as-created one.  The
    // line is flushed (once) the instant any other op is recorded.
    enum canvas_color_space pending_space;
    bool wrote_ws;  // the `working_space` line has been flushed for this file
    struct rec_image img[CNVS_REC_IMAGES_MAX];  // [0, nimg) are this file's image blocks
    int nimg;
    struct rec_path path[CNVS_REC_PATHS_MAX];   // [0, npath) are its path blocks
    int npath;
};

struct cnvs_recorder *__single cnvs_recorder_begin(char const *__null_terminated path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return NULL;
    }
    struct cnvs_recorder *__single r = calloc(1, sizeof *r);
    if (!r) {
        (void)fclose(f);
        return NULL;
    }
    r->f = f;
    r->suspend = 0;
    return r;
}

void cnvs_recorder_end(struct cnvs_recorder *__single r) {
    if (!r) {
        return;
    }
    (void)fclose(r->f);
    for (int i = 0; i < r->nimg; i++) {
        free(r->img[i].px);
    }
    for (int i = 0; i < r->npath; i++) {
        free(r->path[i].cmds);
    }
    free(r);
}

void cnvs_rec_enter(struct cnvs_recorder *__single r) {
    if (r) {
        r->suspend++;
    }
}

void cnvs_rec_leave(struct cnvs_recorder *__single r) {
    if (r) {
        r->suspend--;
    }
}

// Flush the pending working_space line, the file's leading line.  Called at the
// head of every line-writing entry point: the first call writes the line and
// latches wrote_ws, every later call is a no-op.  An unnameable pending space
// (no caller produces one -- only the two compositing spaces reach a canvas)
// writes nothing, leaving the file headerless rather than emitting a token the
// strict parser would reject.
static void cnvs_rec_flush_ws(struct cnvs_recorder *__single r) {
    if (r->wrote_ws) {
        return;
    }
    r->wrote_ws = true;  // latch first: a malformed space still settles the head
    unsigned const i = (unsigned)r->pending_space;
    if (i >= sizeof canvas_color_space_name / sizeof canvas_color_space_name[0]) {
        return;
    }
    fputs("working_space ", r->f);
    fputs(canvas_color_space_name[i], r->f);
    fputc('\n', r->f);
}

// Append " <v>" for each of the n floats; %.9g round-trips a float32's value.
static void put_floats(FILE *__single f, float const *__counted_by(n) v, int n) {
    for (int i = 0; i < n; i++) {
        fprintf(f, " %.9g", (double)v[i]);
    }
}

void cnvs_rec_op(struct cnvs_recorder *__single r, char const *__null_terminated name) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    fputs(name, r->f);
    fputc('\n', r->f);
}

void cnvs_rec_floats(struct cnvs_recorder *__single r, char const *__null_terminated name,
                     float const *__counted_by(n) v, int n) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    fputs(name, r->f);
    put_floats(r->f, v, n);
    fputc('\n', r->f);
}

void cnvs_rec_floats_bool(struct cnvs_recorder *__single r, char const *__null_terminated name,
                          float const *__counted_by(n) v, int n, bool flag) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    fputs(name, r->f);
    put_floats(r->f, v, n);
    fputs(flag ? " 1" : " 0", r->f);
    fputc('\n', r->f);
}

// A colour op line -- the n channel/offset floats, then the colour-space token,
// written unconditionally: sRGB, linear-sRGB, and Oklab are equal peers, so
// every colour line carries its space by name rather than leaning on an
// "absent == sRGB" convention.  The token rides the line by name through the
// shared colour-space table.  An unnameable enum cannot round-trip -- skip the
// whole line rather than write a token the strict parser would reject (no
// caller produces one).
void cnvs_rec_floats_cs(struct cnvs_recorder *__single r, char const *__null_terminated name,
                        float const *__counted_by(n) v, int n,
                        enum canvas_color_space space) {
    if (!r || r->suspend != 0) {
        return;
    }
    unsigned const i = (unsigned)space;
    if (i >= sizeof canvas_color_space_name / sizeof canvas_color_space_name[0]) {
        return;
    }
    cnvs_rec_flush_ws(r);
    fputs(name, r->f);
    put_floats(r->f, v, n);
    fputc(' ', r->f);
    fputs(canvas_color_space_name[i], r->f);
    fputc('\n', r->f);
}

void cnvs_rec_text(struct cnvs_recorder *__single r, char const *__null_terminated name,
                   float x, float y, char const *__counted_by(len) text, int len) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    fputs(name, r->f);
    fprintf(r->f, " %.9g %.9g ", (double)x, (double)y);
    // The text is the rest of the line, verbatim (UTF-8): exactly `len` bytes,
    // not NUL-stopped.  fillText is single-line, so this is faithful; a string
    // containing a newline can't be represented in the line-based format (the
    // parser documents the same).
    if (len > 0) {
        fwrite(text, 1, (size_t)len, r->f);
    }
    fputc('\n', r->f);
}

void cnvs_rec_text_max(struct cnvs_recorder *__single r, char const *__null_terminated name,
                       float x, float y, float max_width,
                       char const *__counted_by(len) text, int len) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    fputs(name, r->f);
    // x, y, max_width, then the text verbatim (the rest of the line, UTF-8):
    // the parser reads three floats and takes the remainder as the string.
    fprintf(r->f, " %.9g %.9g %.9g ", (double)x, (double)y, (double)max_width);
    if (len > 0) {
        fwrite(text, 1, (size_t)len, r->f);
    }
    fputc('\n', r->f);
}

// How many control points one canonical verb consumes, and its token spelling;
// NULL for a byte that is not a verb.  The cached curve stream is boundary
// data, so the emission walk stays as defensive as the drawing walk
// (walk_curves in cnvs_text.c): a bad byte or a short point array stops the
// walk instead of being trusted.
static char const *__null_terminated verb_token(enum cnvs_glyph_verb v,
                                                int *__single npts) {
    switch (v) {
        case CNVS_GLYPH_MOVE:  *npts = 1; return "m";
        case CNVS_GLYPH_LINE:  *npts = 1; return "l";
        case CNVS_GLYPH_QUAD:  *npts = 2; return "q";
        case CNVS_GLYPH_CUBIC: *npts = 3; return "c";
        case CNVS_GLYPH_CLOSE: *npts = 0; return "z";
    }
    *npts = 0;
    return NULL;
}

// The glyph key for one run, color (emoji) runs included -- captures key by
// (font name, weight, style) exactly as curves do: a replay-built run carries
// its interned id; a live run interns through the boundary (idempotent).  The
// line's weight/style join the intern key so a synthesized bold/italic gets a
// distinct id from the regular face it shares a resolved name with.
static int run_fid(struct cnvs_text_cache *__single c, struct cnvs_glyph_run const *__single run,
                   int weight, bool italic) {
    if (run->name_id >= 0) {
        return run->name_id;
    }
    return cnvs_text_cache_font(c, run->font, weight, italic);
}

// Deflated-capture bytes per `bits` line: divisible by 3, so every line but
// the last encodes to base64 without padding; the 16384-char line stays well
// inside the parser's 64 KiB line cap.  A 160x160 capture deflates to between
// a third and a half of its 102400 bytes -- a handful of lines.
enum { CNVS_REC_BITS_PER_LINE = 12288 };

// One `bits` line: `n` deflated bytes as standard base64 (padding only when n
// is not a multiple of 3 -- the block's final line).
static void put_bits_line(FILE *__single f, uint8_t const *__counted_by(n) p,
                          int n) {
    static char const k[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"  // 64 values + NUL
                              "abcdefghijklmnopqrstuvwxyz0123456789+/";
    fputs("bits ", f);
    for (int i = 0; i < n; i += 3) {
        int const m = n - i;
        uint32_t v =      (uint32_t)p[i]     << 16;
        if (m > 1) { v |= (uint32_t)p[i + 1] <<  8; }
        if (m > 2) { v |= (uint32_t)p[i + 2]      ; }
        fputc(        k[(v >> 18) & 63u]      , f);
        fputc(        k[(v >> 12) & 63u]      , f);
        fputc(m > 1 ? k[(v >>  6) & 63u] : '=', f);
        fputc(m > 2 ? k[ v        & 63u] : '=', f);
    }
    fputc('\n', f);
}

// The shaped line itself: everything struct cnvs_shaped carries, so replay
// rebuilds it without shaping.  weight/style are the font weight/style keys
// (written unconditionally, even at the default 400/upright): they pick a
// different real or synthesized face, so they are part of the cache key and the
// glyph identity.  kerning/rendering/variant-caps/stretch/lang are the
// shaping-toggle keys (written
// unconditionally, even at the AUTO/AUTO/NORMAL/NORMAL/"" default): they change
// the runs' advances/glyphs, so they are part of the cache key.  The lang tag is
// length-prefixed bytes (the same shape as the family token), the family token
// is the requested typeface, the cache key's
// family part (length-prefixed bytes, written unconditionally even for the
// default family): two families of the same bytes are distinct lines, so replay
// must key its insert with it.  The rtl token is the paragraph direction
// half of the cache key -- the same bytes shape differently under ltr and rtl,
// so replay must key its insert with it or the two would alias.  ls/ws are the
// letterSpacing/wordSpacing keys (written unconditionally, even when 0): the
// spacing is already baked into the run advances, so replay needs them only to
// key the rebuilt line under the same (family, size, rtl, ls, ws, weight, style,
// kerning, rendering, variant-caps, stretch, lang, text) tuple a live lookup
// uses.  The canvas's
// family/weight/style/spacing/toggle state -- the values a fill_text keys its
// lookup by -- are carried by the set_font_family/set_font_weight/
// set_font_style/set_letter_spacing/set_word_spacing/set_font_kerning/
// set_text_rendering/set_font_variant_caps/set_font_stretch/set_lang ops, not by
// this block.  The text is
// length-prefixed raw bytes to end of line (the key, byte for byte).
static void cnvs_rec_shaping_line(struct cnvs_recorder *__single r,
                                  struct cnvs_text_cache *__single c,
                                  struct cnvs_shaped const *__single s,
                                  char const *__counted_by(fam_len) family, int fam_len,
                                  float size_px, bool rtl, float ls, float ws,
                                  int weight, bool italic, int kerning, int rendering,
                                  int variant_caps, int stretch,
                                  char const *__counted_by(lang_len) lang, int lang_len,
                                  char const *__counted_by(len) text, int len) {
    fprintf(r->f, "shaping %.9g %d %.9g %.9g %d %d %d %d %d %d %d ", (double)size_px,
            rtl ? 1 : 0, (double)ls, (double)ws, weight, italic ? 1 : 0,
            kerning, rendering, variant_caps, stretch, lang_len);
    if (lang_len > 0) {
        fwrite(lang, 1, (size_t)lang_len, r->f);
    }
    fprintf(r->f, " %d ", fam_len);
    if (fam_len > 0) {
        fwrite(family, 1, (size_t)fam_len, r->f);
    }
    fprintf(r->f, " %d %d %d ", s->utf16s, s->nruns, len);
    if (len > 0) {
        fwrite(text, 1, (size_t)len, r->f);
    }
    fputc('\n', r->f);
    for (int ri = 0; ri < s->nruns; ri++) {
        struct cnvs_glyph_run const *__single run = &s->run[ri];
        fprintf(r->f, "run %d %d %d %d", run_fid(c, run, weight, italic),
                run->rtl ? 1 : 0, run->is_color ? 1 : 0, run->count);
        for (int i = 0; i < run->count; i++) {
            fprintf(r->f, " %u %.9g %d", (unsigned)run->glyph[i],
                    (double)run->xadv[i], run->cluster[i]);
        }
        fputc('\n', r->f);
    }
}

void cnvs_rec_text_blocks(struct cnvs_recorder *__single r, struct cnvs_text_cache *__single c,
                          char const *__counted_by(fam_len) family, int fam_len,
                          float size_px, bool rtl, float ls, float ws,
                          int weight, bool italic, int kerning, int rendering,
                          int variant_caps, int stretch,
                          char const *__counted_by(lang_len) lang, int lang_len,
                          char const *__counted_by(len) text, int len) {
    if (!r || r->suspend != 0 || !c) {
        return;
    }
    cnvs_rec_flush_ws(r);
    struct cnvs_shaping_slot *__single slot = cnvs_text_cache_shaping_slot(c, family, fam_len,
                                                  size_px, rtl, ls, ws, weight, italic,
                                                  kerning, rendering, variant_caps, stretch,
                                                  lang, lang_len, text, len);
    if (!slot) {
        return;  // not cached (shaping failed: nothing to carry)
    }
    struct cnvs_shaped const *__single s = slot->s;
    if (slot->emitted) {
        return;  // this shaping block is already in the file
    }
    // Intern every run's font first, so the font blocks land (in id order,
    // which is intern order: declared before use) ahead of any glyph or run
    // line that references them.
    for (int ri = 0; ri < s->nruns; ri++) {
        (void)run_fid(c, &s->run[ri], weight, italic);
    }
    for (int i = 0; i < c->nfonts; i++) {
        if (c->font[i].emitted) {
            continue;
        }
        // weight/style ride the font block: a synthesized bold/italic shares the
        // regular face's NAME, so the id (and the glyph key) keys on them too.
        fprintf(r->f, "font %d %.9g %.9g %d %d ", i, (double)c->font[i].asc1,
                (double)c->font[i].desc1, c->font[i].weight,
                c->font[i].italic ? 1 : 0);
        fwrite(c->font[i].name, 1, (size_t)c->font[i].len, r->f);
        fputc('\n', r->f);
        c->font[i].emitted = true;
    }
    // Glyph blocks: one per (font, glyph) not yet in this recording.  The
    // lookup is the same one the draw is about to take, so a fresh glyph costs
    // its one boundary fetch here and hits from then on.
    for (int ri = 0; ri < s->nruns; ri++) {
        struct cnvs_glyph_run const *__single run = &s->run[ri];
        int fid = run_fid(c, run, weight, italic);
        if (run->is_color || fid < 0) {
            continue;  // emoji carry bitmap blocks below; unkeyable runs carry
        }              // nothing and replay degrades them to blank advances
        for (int i = 0; i < run->count; i++) {
            struct cnvs_glyph_slot *__single g =
                cnvs_text_cache_glyph(c, fid, run->font, run->glyph[i], size_px);
            if (!g || g->emitted) {
                continue;
            }
            fprintf(r->f, "glyph %d %u %.9g %.9g %.9g %.9g %.9g", fid,
                    (unsigned)run->glyph[i], (double)g->upem, (double)g->ink_x0,
                    (double)g->ink_y0, (double)g->ink_x1, (double)g->ink_y1);
            int ip = 0;
            for (int iv = 0; iv < g->nverbs; iv++) {
                int k = 0;
                char const *__null_terminated tok = verb_token(g->verb[iv], &k);
                if (!tok || ip + k > g->npts) {
                    break;  // not a verb, or short points: stop, don't trust
                }
                fputc(' ', r->f);
                fputs(tok, r->f);
                for (int p = 0; p < k; p++) {
                    fprintf(r->f, " %.9g %.9g", (double)g->pt[ip + p].x,
                            (double)g->pt[ip + p].y);
                }
                ip += k;
            }
            fputc('\n', r->f);
            g->emitted = true;
        }
    }
    // Bitmap blocks: one per color (emoji) glyph not yet in this recording --
    // the canonical capture, fetched here if the draw hasn't yet (the same
    // lookup, so it still costs one boundary rasterization per glyph ever),
    // deflated (cnvs_zlib) and the COMPRESSED stream chunked into base64
    // `bits` lines.  The chunking is forced by the parser's 64 KiB line cap
    // (the raw capture, 160x160x4 = 102400 bytes, would be ~137 KB of base64);
    // the deflate is what pays for file size -- captures compress to between
    // a third and a half, gradient-heavy emoji art being noisy input for the
    // greedy fixed-Huffman compressor.  The header carries the deflated byte
    // count;
    // the decoded size stays w*h*4, derivable from the dims, and replay
    // validates both.  A blank color glyph (no ink) has no capture and
    // serializes nothing -- replay draws it as the blank advance it is.  A
    // failed deflate (its scratch allocation) skips the block un-emitted: the
    // recording stays well-formed and the glyph degrades to a blank advance on
    // replay, the cache's usual best-effort posture.  The derived mip pyramid
    // is deliberately NOT serialized: the capture alone is canonical, and
    // replay re-derives the levels at no format cost.
    for (int ri = 0; ri < s->nruns; ri++) {
        struct cnvs_glyph_run const *__single run = &s->run[ri];
        int const fid = run_fid(c, run, weight, italic);
        if (!run->is_color || fid < 0) {
            continue;
        }
        for (int i = 0; i < run->count; i++) {
            struct cnvs_glyph_slot *__single g =
                cnvs_text_cache_color(c, fid, run->font, run->glyph[i]);
            if (!g || g->emitted || g->capture_w <= 0) {
                continue;
            }
            int const zcap = cnvs_zlib_bound(g->capture_size);
            uint8_t *__counted_by_or_null(zcap) z = malloc((size_t)zcap);
            if (!z) {
                continue;  // OOM: skip the block, leave the glyph un-emitted
            }
            int const zn = cnvs_zlib_deflate(z, zcap, g->capture, g->capture_size);
            if (zn < 0) {
                free(z);
                continue;  // deflate's own scratch allocation failed: ditto
            }
            int const nlines =
                (zn + CNVS_REC_BITS_PER_LINE - 1) / CNVS_REC_BITS_PER_LINE;
            fprintf(r->f, "bitmap %d %u %d %d %.9g %.9g %.9g %.9g %d %d\n",
                    fid, (unsigned)run->glyph[i], g->capture_w, g->capture_h,
                    (double)g->ink_x0, (double)g->ink_y0, (double)g->ink_x1,
                    (double)g->ink_y1, zn, nlines);
            for (int off = 0; off < zn; off += CNVS_REC_BITS_PER_LINE) {
                int const rem = zn - off;
                put_bits_line(r->f, z + off,
                              rem < CNVS_REC_BITS_PER_LINE ? rem
                                                           : CNVS_REC_BITS_PER_LINE);
            }
            free(z);
            g->emitted = true;
        }
    }
    // The shaped line itself (and its run lines), keyed by the family, weight,
    // style, the shaping toggles, and the spacing it bakes.
    cnvs_rec_shaping_line(r, c, s, family, fam_len, size_px, rtl, ls, ws,
                          weight, italic, kerning, rendering, variant_caps, stretch,
                          lang, lang_len, text, len);
    slot->emitted = true;
}

int cnvs_rec_image(struct cnvs_recorder *__single r,
                   uint8_t const *__counted_by(len) px, int len, int w, int h,
                   enum canvas_color_type ct, enum canvas_alpha_type at,
                   enum canvas_color_space cs) {
    if (!r || r->suspend != 0) {
        return -1;
    }
    int const bpp = ct == CANVAS_COLOR_F16 ? 8 : 4;
    if (w < 1 || h < 1 || (int64_t)w * h * bpp != (int64_t)len ||
        len > CNVS_REC_IMAGE_BYTES_MAX) {
        return -1;  // outside the format's caps: the op degrades un-recorded
    }
    // Content dedupe: a repeated buffer (a pattern reused per repeat mode, an
    // atlas drawn per subrect) references the block already in the file.  The
    // format is part of the identity: the same bytes mean different colours
    // in a different format -- the colour-space tag included.
    for (int i = 0; i < r->nimg; i++) {
        if (r->img[i].w == w && r->img[i].h == h && r->img[i].len == len &&
            r->img[i].ct == ct && r->img[i].at == at && r->img[i].cs == cs &&
            memcmp(r->img[i].px, px, (size_t)len) == 0) {
            return i;
        }
    }
    if (r->nimg >= CNVS_REC_IMAGES_MAX) {
        return -1;  // id space exhausted: skip the block (and its op)
    }
    int const n = len;  // a counted local cannot bind a parameter's count
    uint8_t *__counted_by_or_null(n) copy = malloc((size_t)n);
    if (!copy) {
        return -1;  // OOM: nothing emitted, the file stays well-formed
    }
    memcpy(copy, px, (size_t)n);
    // Deflate + base64-chunk exactly like an emoji capture (the same `bits`
    // lines, the same line cap arithmetic); the header carries the deflated
    // byte count and the decoded size stays w*h*4, derivable from the dims.
    int const zcap = cnvs_zlib_bound(len);
    uint8_t *__counted_by_or_null(zcap) z = malloc((size_t)zcap);
    if (!z) {
        free(copy);
        return -1;
    }
    int const zn = cnvs_zlib_deflate(z, zcap, px, len);
    if (zn < 0) {
        free(z);
        free(copy);
        return -1;  // deflate's own scratch allocation failed
    }
    int const id = r->nimg;
    int const nlines = (zn + CNVS_REC_BITS_PER_LINE - 1) / CNVS_REC_BITS_PER_LINE;
    // The format axes ride the line by name, like every other enum in the
    // format -- all four ct/at combinations are peers, none the unmarked
    // default.  The colour-space tag is written the same way, unconditionally:
    // sRGB, linear-sRGB, and Oklab are equal peers, so every block carries its
    // space by name (appended after the bits-line count).  An unnameable enum
    // cannot round-trip -- this never happens (every block carries a real
    // space), but the guard keeps a token the strict parser would reject off
    // disk.
    unsigned const csi = (unsigned)cs;
    if (csi >= sizeof canvas_color_space_name / sizeof canvas_color_space_name[0]) {
        free(z);
        free(copy);
        return -1;
    }
    cnvs_rec_flush_ws(r);  // the working_space line leads the file, ahead of any block
    fprintf(r->f, "image %d %s %s %d %d %d %d", id,
            ct == CANVAS_COLOR_F16 ? "f16" : "unorm8",
            at == CANVAS_ALPHA_PREMUL ? "premul" : "unpremul",
            w, h, zn, nlines);
    fputc(' ', r->f);
    fputs(canvas_color_space_name[csi], r->f);
    fputc('\n', r->f);
    for (int off = 0; off < zn; off += CNVS_REC_BITS_PER_LINE) {
        int const rem = zn - off;
        put_bits_line(r->f, z + off,
                      rem < CNVS_REC_BITS_PER_LINE ? rem : CNVS_REC_BITS_PER_LINE);
    }
    free(z);
    r->img[id].len = len;
    r->img[id].px = copy;
    r->img[id].w = w;
    r->img[id].h = h;
    r->img[id].ct = ct;
    r->img[id].at = at;
    r->img[id].cs = cs;
    r->img[id].mips_done = false;
    r->nimg = id + 1;
    return id;
}

void cnvs_rec_image_mips(struct cnvs_recorder *__single r, int id) {
    if (!r || r->suspend != 0 || id < 0 || id >= r->nimg ||
        r->img[id].mips_done) {
        return;
    }
    cnvs_rec_flush_ws(r);
    fprintf(r->f, "image_mips %d\n", id);
    r->img[id].mips_done = true;
}

void cnvs_rec_image_floats(struct cnvs_recorder *__single r,
                           char const *__null_terminated name, int id,
                           float const *__counted_by(n) v, int n) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    fputs(name, r->f);
    fprintf(r->f, " %d", id);
    put_floats(r->f, v, n);
    fputc('\n', r->f);
}

void cnvs_rec_image_ints(struct cnvs_recorder *__single r,
                         char const *__null_terminated name, int id,
                         int const *__counted_by(n) v, int n) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    fputs(name, r->f);
    fprintf(r->f, " %d", id);
    for (int i = 0; i < n; i++) {
        fprintf(r->f, " %d", v[i]);
    }
    fputc('\n', r->f);
}

void cnvs_rec_pattern(struct cnvs_recorder *__single r,
                      char const *__null_terminated name, int id,
                      enum canvas_pattern_repeat repeat) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    unsigned const i = (unsigned)repeat;
    if (i >= sizeof cnvs_repeat_name / sizeof cnvs_repeat_name[0]) {
        return;  // out of range; the setter stored it but no draw reads past
    }            // the enum, and the parser accepts only the four names
    fputs(name, r->f);
    fprintf(r->f, " %d ", id);
    fputs(cnvs_repeat_name[i], r->f);
    fputc('\n', r->f);
}

// One Path2D command's verb token and float-argument count; NULL for a value
// that is not an enum p2d_verb.  The list comes from our own builders, but the
// serializer validates it anyway (the glyph emission posture): an invalid
// command means the whole block is skipped, never a short block.
static char const *__null_terminated p2d_token(enum p2d_verb verb, int *__single nargs) {
    switch (verb) {
        case P2D_MOVE:       *nargs = 2; return "m";
        case P2D_LINE:       *nargs = 2; return "l";
        case P2D_QUAD:       *nargs = 4; return "q";
        case P2D_CUBIC:      *nargs = 6; return "c";
        case P2D_ARC:        *nargs = 5; return "a";
        case P2D_ELLIPSE:    *nargs = 7; return "e";
        case P2D_ARC_TO:     *nargs = 5; return "t";
        case P2D_RECT:       *nargs = 4; return "r";
        case P2D_ROUND_RECT: *nargs = 5; return "rr";
        case P2D_CLOSE:      *nargs = 0; return "z";
    }
    *nargs = 0;
    return NULL;
}

// Field-wise command-list equality: op, winding, and the argument floats
// bit-for-bit (memcmp over the contiguous a[8] -- NaN payloads and -0 compare
// exactly; struct padding is never read).
static bool path_cmds_eq(p2d_cmd const *__counted_by(n) a,
                         p2d_cmd const *__counted_by(n) b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i].verb != b[i].verb || a[i].ccw != b[i].ccw ||
            memcmp(a[i].a, b[i].a, sizeof a[i].a) != 0) {
            return false;
        }
    }
    return true;
}

int cnvs_rec_path(struct cnvs_recorder *__single r, struct canvas_path2d const *__single p) {
    if (!r || r->suspend != 0) {
        return -1;
    }
    int const n = p->ncmds;
    for (int iv = 0; iv < n; iv++) {
        int k = 0;
        if (!p2d_token(p->cmds[iv].verb, &k)) {
            return -1;  // not a command: emit nothing rather than a short block
        }
    }
    // Content dedupe: the same petal stamped under twelve transforms costs
    // one block, however many fill_path/stroke_path reference it.
    for (int i = 0; i < r->npath; i++) {
        if (r->path[i].ncmds == n && path_cmds_eq(r->path[i].cmds, p->cmds, n)) {
            return i;
        }
    }
    if (r->npath >= CNVS_REC_PATHS_MAX) {
        return -1;  // id space exhausted: skip the block (and its op)
    }
    p2d_cmd *__counted_by_or_null(n) copy = NULL;
    if (n > 0) {
        copy = malloc((size_t)n * sizeof *copy);
        if (!copy) {
            return -1;  // OOM: nothing emitted, the file stays well-formed
        }
        memcpy(copy, p->cmds, (size_t)n * sizeof *copy);
    }
    int const id = r->npath;
    cnvs_rec_flush_ws(r);  // the working_space line leads the file, ahead of any block
    fprintf(r->f, "path %d %d\n", id, n);
    for (int i = 0; i < n; i++) {
        int k = 0;
        char const *__null_terminated tok = p2d_token(p->cmds[i].verb, &k);
        fputs(tok, r->f);
        put_floats(r->f, p->cmds[i].a, k);
        if (p->cmds[i].verb == P2D_ARC || p->cmds[i].verb == P2D_ELLIPSE) {
            fputs(p->cmds[i].ccw ? " 1" : " 0", r->f);
        }
        fputc('\n', r->f);
    }
    r->path[id].cmds = copy;
    r->path[id].ncmds = n;
    r->npath = id + 1;
    return id;
}

void cnvs_rec_path_op(struct cnvs_recorder *__single r,
                      char const *__null_terminated name, int id) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    fputs(name, r->f);
    fprintf(r->f, " %d", id);
    fputc('\n', r->f);
}

void cnvs_rec_path_rule(struct cnvs_recorder *__single r,
                        char const *__null_terminated name, int id,
                        enum canvas_fill_rule rule) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    fputs(name, r->f);
    fprintf(r->f, " %d ", id);
    fputs(rule == CANVAS_EVENODD ? "evenodd" : "nonzero", r->f);
    fputc('\n', r->f);
}

void cnvs_rec_ints(struct cnvs_recorder *__single r, char const *__null_terminated name,
                   int const *__counted_by(n) v, int n) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    fputs(name, r->f);
    for (int i = 0; i < n; i++) {
        fprintf(r->f, " %d", v[i]);
    }
    fputc('\n', r->f);
}

void cnvs_rec_rule(struct cnvs_recorder *__single r,
                   char const *__null_terminated name, enum canvas_fill_rule rule) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    fputs(name, r->f);
    fputc(' ', r->f);
    fputs(rule == CANVAS_EVENODD ? "evenodd" : "nonzero", r->f);
    fputc('\n', r->f);
}

void cnvs_rec_smoothing_quality(struct cnvs_recorder *__single r,
                                enum canvas_image_smoothing_quality quality) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    char const *__null_terminated name = "low";
    switch (quality) {
        case CANVAS_SMOOTHING_LOW:    name = "low";    break;
        case CANVAS_SMOOTHING_MEDIUM: name = "medium"; break;
        case CANVAS_SMOOTHING_HIGH:   name = "high";   break;
    }
    fputs("set_image_smoothing_quality ", r->f);
    fputs(name, r->f);
    fputc('\n', r->f);
}

void cnvs_rec_line_join(struct cnvs_recorder *__single r, enum canvas_line_join join) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    char const *__null_terminated name = "miter";
    if (join == CANVAS_JOIN_ROUND) {
        name = "round";
    } else if (join == CANVAS_JOIN_BEVEL) {
        name = "bevel";
    }
    fputs("set_line_join ", r->f);
    fputs(name, r->f);
    fputc('\n', r->f);
}

void cnvs_rec_line_cap(struct cnvs_recorder *__single r, enum canvas_line_cap cap) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    char const *__null_terminated name = "butt";
    if (cap == CANVAS_CAP_ROUND) {
        name = "round";
    } else if (cap == CANVAS_CAP_SQUARE) {
        name = "square";
    }
    fputs("set_line_cap ", r->f);
    fputs(name, r->f);
    fputc('\n', r->f);
}

void cnvs_rec_text_align(struct cnvs_recorder *__single r, enum canvas_text_align align) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    char const *__null_terminated name = "start";
    switch (align) {
        case CANVAS_ALIGN_START:  name = "start";  break;
        case CANVAS_ALIGN_END:    name = "end";    break;
        case CANVAS_ALIGN_LEFT:   name = "left";   break;
        case CANVAS_ALIGN_RIGHT:  name = "right";  break;
        case CANVAS_ALIGN_CENTER: name = "center"; break;
    }
    fputs("set_text_align ", r->f);
    fputs(name, r->f);
    fputc('\n', r->f);
}

void cnvs_rec_text_baseline(struct cnvs_recorder *__single r,
                            enum canvas_text_baseline baseline) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    char const *__null_terminated name = "alphabetic";
    switch (baseline) {
        case CANVAS_BASELINE_ALPHABETIC:  name = "alphabetic";  break;
        case CANVAS_BASELINE_TOP:         name = "top";         break;
        case CANVAS_BASELINE_HANGING:     name = "hanging";     break;
        case CANVAS_BASELINE_MIDDLE:      name = "middle";      break;
        case CANVAS_BASELINE_IDEOGRAPHIC: name = "ideographic"; break;
        case CANVAS_BASELINE_BOTTOM:      name = "bottom";      break;
    }
    fputs("set_text_baseline ", r->f);
    fputs(name, r->f);
    fputc('\n', r->f);
}

void cnvs_rec_direction(struct cnvs_recorder *__single r, enum canvas_direction dir) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    fputs("set_direction ", r->f);
    fputs(dir == CANVAS_DIRECTION_RTL ? "rtl" : "ltr", r->f);
    fputc('\n', r->f);
}

void cnvs_rec_font_family(struct cnvs_recorder *__single r,
                          char const *__counted_by(len) name, int len) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    // Length-prefixed bytes: the name can contain spaces, so a count + a single
    // separating space frames the raw bytes (the same shape as the shaping
    // line's family token).  The setter has already sanitized len >= 1.
    fprintf(r->f, "set_font_family %d ", len);
    if (len > 0) {
        fwrite(name, 1, (size_t)len, r->f);
    }
    fputc('\n', r->f);
}

void cnvs_rec_font_style(struct cnvs_recorder *__single r, enum canvas_font_style style) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    fputs("set_font_style ", r->f);
    fputs(style == CANVAS_FONT_STYLE_ITALIC ? "italic" : "normal", r->f);
    fputc('\n', r->f);
}

void cnvs_rec_font_kerning(struct cnvs_recorder *__single r, enum canvas_font_kerning kerning) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    char const *__null_terminated tok = "auto";
    switch (kerning) {
        case CANVAS_FONT_KERNING_AUTO:   tok = "auto";   break;
        case CANVAS_FONT_KERNING_NORMAL: tok = "normal"; break;
        case CANVAS_FONT_KERNING_NONE:   tok = "none";   break;
    }
    fprintf(r->f, "set_font_kerning %s\n", tok);
}

void cnvs_rec_text_rendering(struct cnvs_recorder *__single r, enum canvas_text_rendering rendering) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    char const *__null_terminated tok = "auto";
    switch (rendering) {
        case CANVAS_TEXT_RENDERING_AUTO:                tok = "auto";               break;
        case CANVAS_TEXT_RENDERING_OPTIMIZE_SPEED:      tok = "optimizeSpeed";      break;
        case CANVAS_TEXT_RENDERING_OPTIMIZE_LEGIBILITY: tok = "optimizeLegibility"; break;
        case CANVAS_TEXT_RENDERING_GEOMETRIC_PRECISION: tok = "geometricPrecision"; break;
    }
    fprintf(r->f, "set_text_rendering %s\n", tok);
}

void cnvs_rec_lang(struct cnvs_recorder *__single r,
                   char const *__counted_by(len) tag, int len) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    // Length-prefixed bytes, like set_font_family: an empty tag records as len 0
    // (a single trailing space, no bytes), so replay can clear the language.
    fprintf(r->f, "set_lang %d ", len);
    if (len > 0) {
        fwrite(tag, 1, (size_t)len, r->f);
    }
    fputc('\n', r->f);
}

void cnvs_rec_font_variant_caps(struct cnvs_recorder *__single r,
                                enum canvas_font_variant_caps variant_caps) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    char const *__null_terminated tok = "normal";
    switch (variant_caps) {
        case CANVAS_FONT_VARIANT_CAPS_NORMAL:         tok = "normal";          break;
        case CANVAS_FONT_VARIANT_CAPS_SMALL_CAPS:     tok = "small-caps";      break;
        case CANVAS_FONT_VARIANT_CAPS_ALL_SMALL_CAPS: tok = "all-small-caps";  break;
    }
    fprintf(r->f, "set_font_variant_caps %s\n", tok);
}

void cnvs_rec_font_stretch(struct cnvs_recorder *__single r, enum canvas_font_stretch stretch) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    char const *__null_terminated tok = "normal";
    switch (stretch) {
        case CANVAS_FONT_STRETCH_ULTRA_CONDENSED: tok = "ultra-condensed"; break;
        case CANVAS_FONT_STRETCH_EXTRA_CONDENSED: tok = "extra-condensed"; break;
        case CANVAS_FONT_STRETCH_CONDENSED:       tok = "condensed";       break;
        case CANVAS_FONT_STRETCH_SEMI_CONDENSED:  tok = "semi-condensed";  break;
        case CANVAS_FONT_STRETCH_NORMAL:          tok = "normal";          break;
        case CANVAS_FONT_STRETCH_SEMI_EXPANDED:   tok = "semi-expanded";   break;
        case CANVAS_FONT_STRETCH_EXPANDED:        tok = "expanded";        break;
        case CANVAS_FONT_STRETCH_EXTRA_EXPANDED:  tok = "extra-expanded";  break;
        case CANVAS_FONT_STRETCH_ULTRA_EXPANDED:  tok = "ultra-expanded";  break;
    }
    fprintf(r->f, "set_font_stretch %s\n", tok);
}

void cnvs_rec_composite(struct cnvs_recorder *__single r, enum canvas_composite_op op) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    unsigned const i = (unsigned)op;
    if (i >= sizeof cnvs_composite_name / sizeof cnvs_composite_name[0]) {
        return;  // out of range; the caller's setter ignored it too (the hook
                 // sits after that guard), so the recording stays consistent.
    }
    fputs("set_global_composite_operation ", r->f);
    fputs(cnvs_composite_name[i], r->f);
    fputc('\n', r->f);
}

void cnvs_rec_working_space(struct cnvs_recorder *__single r,
                           enum canvas_color_space space) {
    // The working space is written UNCONDITIONALLY -- sRGB and linear are equal
    // peers, so every .canvas file leads with its space rather than leaning on
    // an "absent == sRGB" convention.  Only update the PENDING space: the line
    // is flushed lazily (cnvs_rec_flush_ws) at the first other op, so a leading
    // replay `working_space` line -- which arrives after record_to latched the
    // recording canvas's as-created space but before any draw -- corrects the
    // pending value rather than racing against a line already on disk.  Once the
    // line is flushed, the space is settled and further calls are no-ops (the
    // strict parser, requiring the line to lead, would reject a second one).
    if (!r || r->suspend != 0 || r->wrote_ws) {
        return;
    }
    r->pending_space = space;
}

void cnvs_rec_gradient(struct cnvs_recorder *__single r,
                       char const *__null_terminated name,
                       enum canvas_color_space interp_space,
                       enum canvas_alpha_type interp_alpha,
                       float const *__counted_by(n) v, int n) {
    if (!r || r->suspend != 0) {
        return;
    }
    cnvs_rec_flush_ws(r);
    // `<name> <interp-space> <interp-alpha> <geometry floats...>`: the
    // interpolation is required at creation, so both tokens are written
    // unconditionally (no favoured default to omit), then the geometry.  An
    // unnameable enum cannot round-trip -- skip the whole line rather than
    // write a token the strict parser would reject (no caller produces one).
    unsigned const si = (unsigned)interp_space;
    unsigned const ai = (unsigned)interp_alpha;
    if (si >= sizeof canvas_color_space_name / sizeof canvas_color_space_name[0] ||
        ai >= sizeof cnvs_alpha_type_name / sizeof cnvs_alpha_type_name[0]) {
        return;
    }
    fputs(name, r->f);
    fputc(' ', r->f);
    fputs(canvas_color_space_name[si], r->f);
    fputc(' ', r->f);
    fputs(cnvs_alpha_type_name[ai], r->f);
    put_floats(r->f, v, n);
    fputc('\n', r->f);
}
