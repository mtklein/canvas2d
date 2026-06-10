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
#include "cnvs_text.h"

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

// fill_text_max's op line: like cnvs_rec_text, but with the max_width float
// between the (x, y) pen and the verbatim text tail (max_width <x> <y> <w>
// <text...>).  The font/glyph/shape blocks are emitted ahead of it exactly as
// for fill_text (the shaped line keys on size+text alone; max_width only
// condenses the x axis at paint time, so it rides the op line, not the cache).
void cnvs_rec_text_max(cnvs_recorder *__single r, char const *__null_terminated name,
                       float x, float y, float max_width,
                       char const *__counted_by(len) text, int len);

// File-local numbered-object id spaces, shared with the replay parser: the
// recorder never emits an id at or past the cap, and the parser rejects one.
// A recording that uses more distinct images/paths than this stops carrying
// the extras (their op lines are skipped too, so the file stays well-formed)
// -- the recorder's usual best-effort posture.  The byte cap bounds one image
// block's decoded allocation on replay; it is validated before either of the
// block's buffers is allocated, and an incompressible image much past it
// could not fit the 64 MiB file cap anyway.
enum {
    CNVS_REC_IMAGES_MAX = 256,
    CNVS_REC_IMAGE_BYTES_MAX = 64 << 20,  // w*h*4 cap per image block
    CNVS_REC_PATHS_MAX = 256,
};

// Serialize one RGBA8 image (w*h*4 == len bytes, top row first) as an `image`
// block -- deflated (cnvs_zlib) and base64-chunked into `bits` lines exactly
// like an emoji capture -- returning its file-local id.  Deduplicated by
// CONTENT within the recording (the recorder keeps its own copy of each
// emitted image; the caller's buffer is borrowed and may be freed or mutated
// between ops), so a pattern plus several draw_image of one buffer cost one
// block.  Returns -1, emitting nothing, when the image cannot be carried:
// dimensions outside the format's caps, the id space exhausted, or an
// allocation failure -- the caller skips its op line too.
int cnvs_rec_image(cnvs_recorder *__single r,
                   uint8_t const *__counted_by(len) px, int len, int w, int h);

// One op line referencing an image block: `name <image-id> <args...>`, the
// args as floats (draw_image / draw_image_scaled / draw_image_subrect) or as
// integers (put_image_data / put_image_data_dirty, whose placement and dirty
// rect are int-typed in the API).
void cnvs_rec_image_floats(cnvs_recorder *__single r,
                           char const *__null_terminated name, int id,
                           float const *__counted_by(n) v, int n);
void cnvs_rec_image_ints(cnvs_recorder *__single r,
                         char const *__null_terminated name, int id,
                         int const *__counted_by(n) v, int n);

// `set_fill_pattern <image-id> <repeat-name>` (and the stroke twin): the
// pattern's pixels ride the image block; the repeat mode is written by name.
void cnvs_rec_pattern(cnvs_recorder *__single r,
                      char const *__null_terminated name, int id,
                      canvas_pattern_repeat repeat);

// Serialize one Path2D's command list as a numbered `path` block -- a
// `path <id> <ncmds>` header, then one verb line per command (m/l/q/c with
// their points, a/e with a trailing winding bool, t/r/rr, z) -- returning its
// file-local id.  Deduplicated by CONTENT within the recording (the recorder
// keeps its own copy of the command list; the caller's object may be mutated
// or destroyed between draws), so a path stamped under many transforms costs
// one block.  Returns -1, emitting nothing, when the path cannot be carried
// (id space exhausted, or an allocation failure) -- the caller skips its op
// line too.
int cnvs_rec_path(cnvs_recorder *__single r,
                  canvas_path2d const *__single p);

// One op line referencing a path block: `stroke_path <path-id>`, or
// fill_path/clip_path with their explicit rule appended by name.
void cnvs_rec_path_op(cnvs_recorder *__single r,
                      char const *__null_terminated name, int id);
void cnvs_rec_path_rule(cnvs_recorder *__single r,
                        char const *__null_terminated name, int id,
                        canvas_fill_rule rule);

// Serialize the derived text data a fill_text/stroke_text op is about to use --
// interned fonts (with their size-1.0 vmetrics), canonical glyph curves + ink
// bounds, color-glyph captures, and the shaped line -- as `font` / `glyph` /
// `bitmap`+`bits` / `shape`+`run` block lines ahead of the op line, so the
// recorded program is self-contained: replay pre-populates the text cache from
// the blocks and never crosses the text boundary, emoji included.
// Deduplicated against what this recording already wrote via the cache slots'
// `emitted` marks (canvas_record_to clears them), so a repeated string costs
// one block set per recording.  `text`/`len`/`size_px`/`rtl` name the cached
// shaped line (the live lookup has already run; rtl is the paragraph direction
// half of its key); when it isn't cached (shaping failed) nothing is emitted
// and replay degrades to live shaping.
void cnvs_rec_text_blocks(cnvs_recorder *__single r, cnvs_text_cache *__single c,
                          float size_px, bool rtl,
                          char const *__counted_by(len) text, int len);

// `name <ints...>` -- the int-typed op lines with no block reference (resize).
void cnvs_rec_ints(cnvs_recorder *__single r, char const *__null_terminated name,
                   int const *__counted_by(n) v, int n);

// Enum-valued setters, written by name (the spellings the parser accepts).
void cnvs_rec_fill_rule(cnvs_recorder *__single r, canvas_fill_rule rule);
void cnvs_rec_smoothing_quality(cnvs_recorder *__single r,
                                canvas_image_smoothing_quality quality);
void cnvs_rec_line_join(cnvs_recorder *__single r, canvas_line_join join);
void cnvs_rec_line_cap(cnvs_recorder *__single r, canvas_line_cap cap);
void cnvs_rec_composite(cnvs_recorder *__single r, canvas_composite_op op);
void cnvs_rec_text_align(cnvs_recorder *__single r, canvas_text_align align);
void cnvs_rec_text_baseline(cnvs_recorder *__single r,
                            canvas_text_baseline baseline);
void cnvs_rec_direction(cnvs_recorder *__single r, canvas_direction dir);
