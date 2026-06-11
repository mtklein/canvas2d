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
};

// Likewise one `path` block: an owned copy of the Path2D's command list --
// content, not pointer identity, is the key, since the caller's object may be
// mutated (or freed and another allocated at the same address) between draws.
struct rec_path {
    p2d_cmd *__counted_by(len) cmds;
    int len;
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

struct cnvs_recorder {
    FILE *__single f;
    int suspend;  // >0 while a compound op's sub-calls are being swallowed
    struct rec_image img[CNVS_REC_IMAGES_MAX];  // [0, nimg) are this file's image blocks
    int nimg;
    struct rec_path path[CNVS_REC_PATHS_MAX];   // [0, npath) are its path blocks
    int npath;
};

struct cnvs_recorder *__single cnvs_recorder_open(char const *__null_terminated path) {
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

void cnvs_recorder_close(struct cnvs_recorder *__single r) {
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
    fputs(name, r->f);
    fputc('\n', r->f);
}

void cnvs_rec_floats(struct cnvs_recorder *__single r, char const *__null_terminated name,
                     float const *__counted_by(n) v, int n) {
    if (!r || r->suspend != 0) {
        return;
    }
    fputs(name, r->f);
    put_floats(r->f, v, n);
    fputc('\n', r->f);
}

void cnvs_rec_floats_bool(struct cnvs_recorder *__single r, char const *__null_terminated name,
                          float const *__counted_by(n) v, int n, bool flag) {
    if (!r || r->suspend != 0) {
        return;
    }
    fputs(name, r->f);
    put_floats(r->f, v, n);
    fputs(flag ? " 1" : " 0", r->f);
    fputc('\n', r->f);
}

void cnvs_rec_text(struct cnvs_recorder *__single r, char const *__null_terminated name,
                   float x, float y, char const *__counted_by(len) text, int len) {
    if (!r || r->suspend != 0) {
        return;
    }
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
// font name exactly as curves do: a replay-built run carries its interned id;
// a live run interns through the boundary (idempotent).
static int run_fid(struct cnvs_text_cache *__single c, cnvs_glyph_run const *__single run) {
    if (run->name_id >= 0) {
        return run->name_id;
    }
    return cnvs_text_cache_font(c, run->font);
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

void cnvs_rec_text_blocks(struct cnvs_recorder *__single r, struct cnvs_text_cache *__single c,
                          float size_px, bool rtl,
                          char const *__counted_by(len) text, int len) {
    if (!r || r->suspend != 0 || !c) {
        return;
    }
    struct cnvs_shape_slot *__single slot = cnvs_text_cache_shape_slot(c, size_px, rtl,
                                                                text, len);
    if (!slot || slot->emitted) {
        return;  // not cached (shaping failed: nothing to carry), or this
    }            // recording already wrote this line's blocks
    struct cnvs_shaped const *__single s = slot->s;
    // Intern every run's font first, so the font blocks land (in id order,
    // which is intern order: declared before use) ahead of any glyph or run
    // line that references them.
    for (int ri = 0; ri < s->nruns; ri++) {
        (void)run_fid(c, &s->run[ri]);
    }
    for (int i = 0; i < c->nfonts; i++) {
        if (c->font[i].emitted) {
            continue;
        }
        fprintf(r->f, "font %d %.9g %.9g ", i, (double)c->font[i].asc1,
                (double)c->font[i].desc1);
        fwrite(c->font[i].name, 1, (size_t)c->font[i].len, r->f);
        fputc('\n', r->f);
        c->font[i].emitted = true;
    }
    // Glyph blocks: one per (font, glyph) not yet in this recording.  The
    // lookup is the same one the draw is about to take, so a fresh glyph costs
    // its one boundary fetch here and hits from then on.
    for (int ri = 0; ri < s->nruns; ri++) {
        cnvs_glyph_run const *__single run = &s->run[ri];
        int fid = run_fid(c, run);
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
    // replay re-derives the levels in checked C at no format cost.
    for (int ri = 0; ri < s->nruns; ri++) {
        cnvs_glyph_run const *__single run = &s->run[ri];
        int fid = run_fid(c, run);
        if (!run->is_color || fid < 0) {
            continue;
        }
        for (int i = 0; i < run->count; i++) {
            struct cnvs_glyph_slot *__single g =
                cnvs_text_cache_color(c, fid, run->font, run->glyph[i]);
            if (!g || g->emitted || g->cap_w <= 0) {
                continue;
            }
            int const zcap = cnvs_zlib_bound(g->cap_len);
            uint8_t *__counted_by_or_null(zcap) z = malloc((size_t)zcap);
            if (!z) {
                continue;  // OOM: skip the block, leave the glyph un-emitted
            }
            int const zn = cnvs_zlib_deflate(z, zcap, g->capture, g->cap_len);
            if (zn < 0) {
                free(z);
                continue;  // deflate's own scratch allocation failed: ditto
            }
            int const nlines =
                (zn + CNVS_REC_BITS_PER_LINE - 1) / CNVS_REC_BITS_PER_LINE;
            fprintf(r->f, "bitmap %d %u %d %d %.9g %.9g %.9g %.9g %d %d\n",
                    fid, (unsigned)run->glyph[i], g->cap_w, g->cap_h,
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
    // The shaped line itself: everything struct cnvs_shaped carries, so replay
    // rebuilds it without shaping.  The rtl token is the paragraph direction
    // half of the cache key -- the same bytes shape differently under ltr and
    // rtl, so replay must key its insert with it or the two would alias.  The
    // text is length-prefixed raw bytes to end of line (the key, byte for
    // byte).
    fprintf(r->f, "shape %.9g %d %d %d %d ", (double)size_px, rtl ? 1 : 0,
            s->text_len, s->nruns, len);
    if (len > 0) {
        fwrite(text, 1, (size_t)len, r->f);
    }
    fputc('\n', r->f);
    for (int ri = 0; ri < s->nruns; ri++) {
        cnvs_glyph_run const *__single run = &s->run[ri];
        fprintf(r->f, "run %d %d %d %d", run_fid(c, run), run->rtl ? 1 : 0,
                run->is_color ? 1 : 0, run->count);
        for (int i = 0; i < run->count; i++) {
            fprintf(r->f, " %u %.9g %d", (unsigned)run->glyph[i],
                    (double)run->xadv[i], run->cluster[i]);
        }
        fputc('\n', r->f);
    }
    slot->emitted = true;
}

int cnvs_rec_image(struct cnvs_recorder *__single r,
                   uint8_t const *__counted_by(len) px, int len, int w, int h) {
    if (!r || r->suspend != 0) {
        return -1;
    }
    if (w < 1 || h < 1 || (int64_t)w * h * 4 != (int64_t)len ||
        len > CNVS_REC_IMAGE_BYTES_MAX) {
        return -1;  // outside the format's caps: the op degrades un-recorded
    }
    // Content dedupe: a repeated buffer (a pattern reused per repeat mode, an
    // atlas drawn per subrect) references the block already in the file.
    for (int i = 0; i < r->nimg; i++) {
        if (r->img[i].w == w && r->img[i].h == h && r->img[i].len == len &&
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
    fprintf(r->f, "image %d %d %d %d %d\n", id, w, h, zn, nlines);
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
    r->nimg = id + 1;
    return id;
}

void cnvs_rec_image_floats(struct cnvs_recorder *__single r,
                           char const *__null_terminated name, int id,
                           float const *__counted_by(n) v, int n) {
    if (!r || r->suspend != 0) {
        return;
    }
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
    unsigned i = (unsigned)repeat;
    if (i >= sizeof cnvs_repeat_name / sizeof cnvs_repeat_name[0]) {
        return;  // out of range; the setter stored it but no draw reads past
    }            // the enum, and the parser accepts only the four names
    fputs(name, r->f);
    fprintf(r->f, " %d ", id);
    fputs(cnvs_repeat_name[i], r->f);
    fputc('\n', r->f);
}

// One Path2D command's verb token and float-argument count; NULL for a value
// that is not an enum p2d_op.  The list comes from our own builders, but the
// serializer validates it anyway (the glyph emission posture): an invalid
// command means the whole block is skipped, never a short block.
static char const *__null_terminated p2d_token(enum p2d_op op, int *__single nargs) {
    switch (op) {
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
        if (a[i].op != b[i].op || a[i].ccw != b[i].ccw ||
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
    int const n = p->len;
    for (int iv = 0; iv < n; iv++) {
        int k = 0;
        if (!p2d_token(p->cmds[iv].op, &k)) {
            return -1;  // not a command: emit nothing rather than a short block
        }
    }
    // Content dedupe: the same petal stamped under twelve transforms costs
    // one block, however many fill_path/stroke_path reference it.
    for (int i = 0; i < r->npath; i++) {
        if (r->path[i].len == n && path_cmds_eq(r->path[i].cmds, p->cmds, n)) {
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
    fprintf(r->f, "path %d %d\n", id, n);
    for (int i = 0; i < n; i++) {
        int k = 0;
        char const *__null_terminated tok = p2d_token(p->cmds[i].op, &k);
        fputs(tok, r->f);
        put_floats(r->f, p->cmds[i].a, k);
        if (p->cmds[i].op == P2D_ARC || p->cmds[i].op == P2D_ELLIPSE) {
            fputs(p->cmds[i].ccw ? " 1" : " 0", r->f);
        }
        fputc('\n', r->f);
    }
    r->path[id].cmds = copy;
    r->path[id].len = n;
    r->npath = id + 1;
    return id;
}

void cnvs_rec_path_op(struct cnvs_recorder *__single r,
                      char const *__null_terminated name, int id) {
    if (!r || r->suspend != 0) {
        return;
    }
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
    fputs(name, r->f);
    for (int i = 0; i < n; i++) {
        fprintf(r->f, " %d", v[i]);
    }
    fputc('\n', r->f);
}

void cnvs_rec_fill_rule(struct cnvs_recorder *__single r, enum canvas_fill_rule rule) {
    if (!r || r->suspend != 0) {
        return;
    }
    fputs("set_fill_rule ", r->f);
    fputs(rule == CANVAS_EVENODD ? "evenodd" : "nonzero", r->f);
    fputc('\n', r->f);
}

void cnvs_rec_smoothing_quality(struct cnvs_recorder *__single r,
                                enum canvas_image_smoothing_quality quality) {
    if (!r || r->suspend != 0) {
        return;
    }
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
    fputs("set_direction ", r->f);
    fputs(dir == CANVAS_DIRECTION_RTL ? "rtl" : "ltr", r->f);
    fputc('\n', r->f);
}

void cnvs_rec_composite(struct cnvs_recorder *__single r, enum canvas_composite_op op) {
    if (!r || r->suspend != 0) {
        return;
    }
    unsigned i = (unsigned)op;
    if (i >= sizeof cnvs_composite_name / sizeof cnvs_composite_name[0]) {
        return;  // out of range; the caller's setter ignored it too (the hook
                 // sits after that guard), so the recording stays consistent.
    }
    fputs("set_global_composite_operation ", r->f);
    fputs(cnvs_composite_name[i], r->f);
    fputc('\n', r->f);
}
