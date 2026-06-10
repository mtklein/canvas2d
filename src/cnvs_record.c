// Text canvas-program recorder (cnvs_record.h).  Writes the same one-command-per-
// line format cnvs_replay.c parses, straight to a FILE*; canvas.c calls these from
// each recordable public op.  Everything here is plain bounds-safe C -- counted
// float runs, __null_terminated names/text -- so the write side plays by the same
// -fbounds-safety rules as the (forge-free) read side.

#include "cnvs_record.h"

#include <ptrcheck.h>
#include <stdio.h>
#include <stdlib.h>

struct cnvs_recorder {
    FILE *__single f;
    int suspend;  // >0 while a compound op's sub-calls are being swallowed
};

cnvs_recorder *__single cnvs_recorder_open(char const *__null_terminated path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return NULL;
    }
    cnvs_recorder *__single r = calloc(1, sizeof *r);
    if (!r) {
        (void)fclose(f);
        return NULL;
    }
    r->f = f;
    r->suspend = 0;
    return r;
}

void cnvs_recorder_close(cnvs_recorder *__single r) {
    if (!r) {
        return;
    }
    (void)fclose(r->f);
    free(r);
}

void cnvs_rec_enter(cnvs_recorder *__single r) {
    if (r) {
        r->suspend++;
    }
}

void cnvs_rec_leave(cnvs_recorder *__single r) {
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

void cnvs_rec_op(cnvs_recorder *__single r, char const *__null_terminated name) {
    if (!r || r->suspend != 0) {
        return;
    }
    fputs(name, r->f);
    fputc('\n', r->f);
}

void cnvs_rec_floats(cnvs_recorder *__single r, char const *__null_terminated name,
                     float const *__counted_by(n) v, int n) {
    if (!r || r->suspend != 0) {
        return;
    }
    fputs(name, r->f);
    put_floats(r->f, v, n);
    fputc('\n', r->f);
}

void cnvs_rec_floats_bool(cnvs_recorder *__single r, char const *__null_terminated name,
                          float const *__counted_by(n) v, int n, bool flag) {
    if (!r || r->suspend != 0) {
        return;
    }
    fputs(name, r->f);
    put_floats(r->f, v, n);
    fputs(flag ? " 1" : " 0", r->f);
    fputc('\n', r->f);
}

void cnvs_rec_text(cnvs_recorder *__single r, char const *__null_terminated name,
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

// How many control points one canonical verb consumes, and its token spelling;
// NULL for a byte that is not a verb.  The cached curve stream is boundary
// data, so the emission walk stays as defensive as the drawing walk
// (walk_curves in cnvs_text.c): a bad byte or a short point array stops the
// walk instead of being trusted.
static char const *__null_terminated verb_token(cnvs_glyph_verb v,
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
static int run_fid(cnvs_text_cache *__single c, cnvs_glyph_run const *__single run) {
    if (run->name_id >= 0) {
        return run->name_id;
    }
    return cnvs_text_cache_font(c, run->font);
}

// Capture bytes per `bits` line: divisible by 3, so every line but the last
// encodes to base64 without padding; the 16384-char line stays well inside
// the parser's 64 KiB line cap.  A 160x160 capture (102400 bytes) takes nine
// lines.
enum { CNVS_REC_BITS_PER_LINE = 12288 };

// One `bits` line: `n` capture bytes as standard base64 (padding only when n
// is not a multiple of 3 -- the block's final line).
static void put_bits_line(FILE *__single f, uint8_t const *__counted_by(n) p,
                          int n) {
    static char const k[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"  // 64 values + NUL
                              "abcdefghijklmnopqrstuvwxyz0123456789+/";
    fputs("bits ", f);
    for (int i = 0; i < n; i += 3) {
        int const m = n - i;
        uint32_t v = (uint32_t)p[i] << 16;
        if (m > 1) { v |= (uint32_t)p[i + 1] << 8; }
        if (m > 2) { v |= (uint32_t)p[i + 2]; }
        fputc(k[(v >> 18) & 63u], f);
        fputc(k[(v >> 12) & 63u], f);
        fputc(m > 1 ? k[(v >> 6) & 63u] : '=', f);
        fputc(m > 2 ? k[v & 63u] : '=', f);
    }
    fputc('\n', f);
}

void cnvs_rec_text_blocks(cnvs_recorder *__single r, cnvs_text_cache *__single c,
                          float size_px, char const *__counted_by(len) text,
                          int len) {
    if (!r || r->suspend != 0 || !c) {
        return;
    }
    cnvs_shape_slot *__single slot = cnvs_text_cache_shape_slot(c, size_px, text,
                                                                len);
    if (!slot || slot->emitted) {
        return;  // not cached (shaping failed: nothing to carry), or this
    }            // recording already wrote this line's blocks
    cnvs_shaped const *__single s = slot->s;
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
            cnvs_glyph_slot *__single g =
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
    // chunked into base64 `bits` lines because the raw capture (160x160x4 =
    // 102400 bytes, ~137 KB base64) cannot fit the one-line-per-block pattern
    // under the parser's line cap.  A blank color glyph (no ink) has no
    // capture and serializes nothing -- replay draws it as the blank advance
    // it is.  The derived mip pyramid is deliberately NOT serialized: the
    // capture alone is canonical, and replay re-derives the levels in checked
    // C at no format cost.
    for (int ri = 0; ri < s->nruns; ri++) {
        cnvs_glyph_run const *__single run = &s->run[ri];
        int fid = run_fid(c, run);
        if (!run->is_color || fid < 0) {
            continue;
        }
        for (int i = 0; i < run->count; i++) {
            cnvs_glyph_slot *__single g =
                cnvs_text_cache_color(c, fid, run->font, run->glyph[i]);
            if (!g || g->emitted || g->cap_w <= 0) {
                continue;
            }
            int const nlines =
                (g->cap_len + CNVS_REC_BITS_PER_LINE - 1) / CNVS_REC_BITS_PER_LINE;
            fprintf(r->f, "bitmap %d %u %d %d %.9g %.9g %.9g %.9g %d\n", fid,
                    (unsigned)run->glyph[i], g->cap_w, g->cap_h,
                    (double)g->ink_x0, (double)g->ink_y0, (double)g->ink_x1,
                    (double)g->ink_y1, nlines);
            for (int off = 0; off < g->cap_len; off += CNVS_REC_BITS_PER_LINE) {
                int const rem = g->cap_len - off;
                put_bits_line(r->f, g->capture + off,
                              rem < CNVS_REC_BITS_PER_LINE ? rem
                                                           : CNVS_REC_BITS_PER_LINE);
            }
            g->emitted = true;
        }
    }
    // The shaped line itself: everything cnvs_shaped carries, so replay
    // rebuilds it without shaping.  The text is length-prefixed raw bytes to
    // end of line (it is the cache key, byte for byte).
    fprintf(r->f, "shape %.9g %d %d %d ", (double)size_px, s->text_len, s->nruns,
            len);
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

void cnvs_rec_fill_rule(cnvs_recorder *__single r, canvas_fill_rule rule) {
    if (!r || r->suspend != 0) {
        return;
    }
    fputs("set_fill_rule ", r->f);
    fputs(rule == CANVAS_EVENODD ? "evenodd" : "nonzero", r->f);
    fputc('\n', r->f);
}

void cnvs_rec_line_join(cnvs_recorder *__single r, canvas_line_join join) {
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

void cnvs_rec_line_cap(cnvs_recorder *__single r, canvas_line_cap cap) {
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

void cnvs_rec_text_align(cnvs_recorder *__single r, canvas_text_align align) {
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

void cnvs_rec_text_baseline(cnvs_recorder *__single r,
                            canvas_text_baseline baseline) {
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

// Composite-op names in canvas_composite_op order (matches cnvs_replay.c).
static char const *const k_composite[] = {
    "source-over", "source-in", "source-out", "source-atop", "destination-over",
    "destination-in", "destination-out", "destination-atop", "xor", "lighter",
    "copy", "multiply", "screen", "overlay", "darken", "lighten", "color-dodge",
    "color-burn", "hard-light", "soft-light", "difference", "exclusion", "hue",
    "saturation", "color", "luminosity",
};

void cnvs_rec_composite(cnvs_recorder *__single r, canvas_composite_op op) {
    if (!r || r->suspend != 0) {
        return;
    }
    unsigned i = (unsigned)op;
    if (i >= sizeof k_composite / sizeof k_composite[0]) {
        return;  // out of range; the caller's setter ignored it too (the hook
                 // sits after that guard), so the recording stays consistent.
    }
    fputs("set_global_composite_operation ", r->f);
    fputs(k_composite[i], r->f);
    fputc('\n', r->f);
}
