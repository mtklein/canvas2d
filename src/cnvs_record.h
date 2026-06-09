#pragma once

// Text canvas-program *recorder* -- the write side of the .canvas text format
// that cnvs_replay.c reads.  canvas_record_to() (canvas.h) installs a recorder on
// a canvas; canvas.c then calls these emit helpers from each recordable public
// op, so a live drawing session is written out as one command per line.  The
// command spellings, enum names, and argument order match the replay parser
// value-for-value -- record_to and replay_from are inverses over the text-format
// subset (tests/test_record.c pins the round-trip).  Numbers are written with
// %.9g (enough to round-trip a float32); fill_text/stroke_text write their text
// verbatim as the rest of the line.

#include "canvas.h"

#include <ptrcheck.h>

typedef struct cnvs_recorder cnvs_recorder;

// Open `path` for recording (truncating any existing file).  NULL on failure.
cnvs_recorder *__single cnvs_recorder_open(char const *__null_terminated path);
// Flush and close; safe on NULL.
void cnvs_recorder_close(cnvs_recorder *__single r);

// Suspend / resume emission, reference-counted.  A compound op (arc, round_rect,
// arc_to) records its own command line, then brackets the public sub-calls it
// makes with enter/leave so those sub-calls don't also record -- the file keeps
// the op the caller actually issued, not its expansion.  Both are no-ops on NULL.
void cnvs_rec_enter(cnvs_recorder *__single r);
void cnvs_rec_leave(cnvs_recorder *__single r);

// Emit one command line.  `name` is the canvas_* function name without the
// canvas_ prefix.  All are no-ops while suspended or when r is NULL.
void cnvs_rec_op(cnvs_recorder *__single r, char const *__null_terminated name);
void cnvs_rec_floats(cnvs_recorder *__single r, char const *__null_terminated name,
                     float const *__counted_by(n) v, int n);
void cnvs_rec_floats_bool(cnvs_recorder *__single r, char const *__null_terminated name,
                          float const *__counted_by(n) v, int n, bool flag);
void cnvs_rec_text(cnvs_recorder *__single r, char const *__null_terminated name,
                   float x, float y, char const *__counted_by(len) text, int len);

// Enum-valued setters, written by name (the spellings the parser accepts).
void cnvs_rec_fill_rule(cnvs_recorder *__single r, canvas_fill_rule rule);
void cnvs_rec_line_join(cnvs_recorder *__single r, canvas_line_join join);
void cnvs_rec_line_cap(cnvs_recorder *__single r, canvas_line_cap cap);
void cnvs_rec_composite(cnvs_recorder *__single r, canvas_composite_op op);
