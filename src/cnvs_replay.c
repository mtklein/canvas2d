// Text canvas-program parser + replayer (the cnvs_replay.h core, and
// canvas_replay_from()).  Parses untrusted text by index over a __counted_by
// buffer -- the bounds-safe-parsing exercise -- and dispatches to the public
// canvas_* API.  No forges and no __null_terminated: numbers are parsed in place
// by index (no strtof), and the text tail is passed as a slice to the
// length-counted canvas_*_text_n; the parser stays entirely in the indexable
// world.  (docs/bounds-safety.md walks through what that took.)

#include "cnvs_replay.h"

#include "canvas.h"

#include <ptrcheck.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPLAY_LINE_MAX 4096u        // over-long line -> reject (DoS guard)
#define REPLAY_FILE_MAX (64u << 20)  // 64 MiB file cap
#define REPLAY_DASH_MAX 64           // max dash segments from one line

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

// Next token parsed as a float, in place by index -- no strtof, no
// __null_terminated, no forge.  Stricter than strtof: the whole token must be a
// number (sign? digits, optional .fraction, optional e[+-]exp).
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
        while (i < te && data[i] >= '0' && data[i] <= '9') { eexp = eexp * 10 + (data[i] - '0'); i++; eany = true; }
        if (!eany) {
            return false;
        }
        if (eneg) { eexp = -eexp; }
    }
    if (i != te) {
        return false;  // trailing junk in the token (e.g. "1.5.2", "1x")
    }
    // Scale by 10^(fexp+eexp).  Exact for canvas-range values; for extreme
    // exponents this loop drifts where strtof is correctly-rounded (a power table
    // would fix it), but such inputs are clamped by the canvas API anyway.
    int e = fexp + eexp;
    double scale = 1.0, base = e < 0 ? 0.1 : 10.0;
    for (int k = 0, n = e < 0 ? -e : e; k < n; k++) { scale *= base; }
    *out = (float)((neg ? -mant : mant) * scale);
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

// Composite-op names in enum order (canvas_composite_op).
static char const *const k_composite[] = {
    "source-over", "source-in", "source-out", "source-atop", "destination-over",
    "destination-in", "destination-out", "destination-atop", "xor", "lighter",
    "copy", "multiply", "screen", "overlay", "darken", "lighten", "color-dodge",
    "color-burn", "hard-light", "soft-light", "difference", "exclusion", "hue",
    "saturation", "color", "luminosity",
};

static bool replay_line(canvas *__single cv, char const *__counted_by(le) data,
                        size_t ls, size_t le) {
    size_t j = ls;
    size_t cs, cl;
    if (!read_token(data, le, &j, &cs, &cl)) {
        return true;  // blank line
    }
    if (data[cs] == '#') {
        return true;  // comment line
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
    size_t i = 0;
    while (i < len) {
        size_t le = i;
        while (le < len && data[le] != '\n') { le++; }
        if (le - i >= REPLAY_LINE_MAX) {
            return false;  // over-long line
        }
        if (!replay_line(cv, data, i, le)) {
            return false;
        }
        i = le + 1;  // past the '\n' (or past end)
    }
    return true;
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
