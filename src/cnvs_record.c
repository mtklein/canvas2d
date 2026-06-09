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
