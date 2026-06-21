#pragma once

// Text canvas-program *recorder* -- the write side of the .canvas text format
// that canvas2d_replay.c reads.  canvas2d_record_to() (canvas.h) installs a recorder on
// a canvas; canvas.c then calls these emit helpers from each recordable public
// op, so a live drawing session is written out as one command per line.  The
// command spellings, enum names, and argument order match the replay parser
// value-for-value -- record_to and replay_from are inverses over the text-format
// subset (tests/test_record.c pins the round-trip).  Numbers are written with
// %.9g (enough to round-trip a float32); fill_text/stroke_text write their text
// verbatim as the rest of the line.

#include "canvas2d.h"
#include "canvas2d_text.h"

#include <ptrcheck.h>

struct canvas2d_recorder;

// Open `path` for recording (truncating any existing file).  NULL on failure.
struct canvas2d_recorder *__single canvas2d_recorder_begin(char const *__null_terminated path);
// Flush and close; safe on NULL.
void canvas2d_recorder_end(struct canvas2d_recorder *__single r);

// Suspend / resume emission, reference-counted.  A compound op (arc, round_rect,
// arc_to) records its own command line, then brackets the public sub-calls it
// makes with enter/leave so those sub-calls don't also record -- the file keeps
// the op the caller actually issued, not its expansion.  Both are no-ops on NULL.
void canvas2d_rec_enter(struct canvas2d_recorder *__single r);
void canvas2d_rec_leave(struct canvas2d_recorder *__single r);

// Emit one command line.  `name` is the canvas2d_* function name without the
// canvas2d_ prefix.  All are no-ops while suspended or when r is NULL.
void canvas2d_rec_op(struct canvas2d_recorder *__single r, char const *__null_terminated name);
void canvas2d_rec_floats(struct canvas2d_recorder *__single r, char const *__null_terminated name,
                     float const *__counted_by(n) v, int n);
void canvas2d_rec_floats_bool(struct canvas2d_recorder *__single r, char const *__null_terminated name,
                          float const *__counted_by(n) v, int n, bool flag);
// A colour op line (set_fill_rgba and its siblings, gradient stops, drop
// shadow): the n floats, then a REQUIRED trailing colour-space token naming
// `space`.  The three spaces are peers, so the token is always written.  Mirrors
// the image block's colour-space tag.
void canvas2d_rec_floats_cs(struct canvas2d_recorder *__single r, char const *__null_terminated name,
                        float const *__counted_by(n) v, int n,
                        enum canvas2d_color_space space);
void canvas2d_rec_text(struct canvas2d_recorder *__single r, char const *__null_terminated name,
                   float x, float y, char const *__counted_by(len) text, int len);

// fill_text_max's op line: like canvas2d_rec_text, but with the max_width float
// between the (x, y) pen and the verbatim text tail (max_width <x> <y> <w>
// <text...>).  The font/glyph/shape blocks are emitted ahead of it exactly as
// for fill_text (the shaped line keys on size+text alone; max_width only
// condenses the x axis at paint time, so it rides the op line, not the cache).
void canvas2d_rec_text_max(struct canvas2d_recorder *__single r, char const *__null_terminated name,
                       float x, float y, float max_width,
                       char const *__counted_by(len) text, int len);

// The enum spellings of the text format, indexed by enum value -- one table
// serving both sides: the recorder writes these names and the replay parser
// accepts exactly them, so the two cannot drift.  Defined in canvas2d_record.c.
extern char const *const canvas2d_composite_name[CANVAS2D_OP_LUMINOSITY + 1];
extern char const *const canvas2d_repeat_name[CANVAS2D_NO_REPEAT + 1];
// One table for every colour-space token (working_space + gradient interp),
// indexed by enum value: "srgb", "linear", "oklab".  The tokens are the
// on-disk contract; both record and replay name through this table.
extern char const *const canvas2d_color_space_name[CANVAS2D_CS_OKLAB + 1];
// The alpha-mode tokens, indexed by enum value: "unpremul", "premul".  A
// gradient op line carries one of these (the same table the image-block format
// names its alpha type through); both record and replay name through it.
extern char const *const canvas2d_alpha_type_name[CANVAS2D_ALPHA_PREMUL + 1];

// `working_space <name>` -- written unconditionally at record start (sRGB and
// linear are peers, so every .canvas leads with its space), before any drawing
// op; the replay parser applies it to the fresh canvas before the first colour
// interns.  A no-op on NULL/suspend.

// File-local numbered-object id spaces, shared with the replay parser: the
// recorder never emits an id at or past the cap, and the parser rejects one.
// A recording that uses more distinct images/paths than this stops carrying
// the extras (their op lines are skipped too, so the file stays well-formed)
// -- the recorder's usual best-effort posture.  The byte cap bounds one image
// block's decoded allocation on replay; it is validated before either of the
// block's buffers is allocated, and an incompressible image much past it
// could not fit the 64 MiB file cap anyway.
enum {
    CANVAS2D_REC_IMAGES_MAX = 256,
    CANVAS2D_REC_IMAGE_BYTES_MAX = 64 << 20,  // w*h*4 cap per image block
    CANVAS2D_REC_PATHS_MAX = 256,
};

// Serialize one RGBA image (w*h*bpp == len bytes, top row first; bpp is 4
// for unorm8 channels, 8 for f16) as an `image` block whose colour and alpha
// types ride the line by name -- the four formats are peers, none the
// unmarked default -- deflated (canvas2d_zlib) and
// base64-chunked into `bits` lines exactly like an emoji capture, returning
// its file-local id.  `cs` is the source's colour-space tag (honoured on draw:
// the resolved sample converts to the working space on deposit): it rides the
// line as a REQUIRED trailing token, written for every space (the spaces are
// peers).  Deduplicated by
// CONTENT + the format (ct, at, cs) within the recording (the recorder keeps
// its own copy of each emitted image; the caller's buffer is borrowed and may
// be freed or mutated between ops), so a pattern plus several draw_image of
// one buffer cost one block.  Returns -1, emitting nothing, when the image
// cannot be carried: dimensions outside the format's caps, the id space
// exhausted, or an allocation failure -- the caller skips its op line too.
int canvas2d_rec_image(struct canvas2d_recorder *__single r,
                   uint8_t const *__counted_by(len) px, int len, int w, int h,
                   enum canvas2d_color_type ct, enum canvas2d_alpha_type at,
                   enum canvas2d_color_space cs);

// `image_mips <id>`: from this point the block's draws have mip-chain
// semantics -- a bitmap entry point (which rebuilds a chain per minifying
// draw) emits it as soon as its block is declared; an image-object draw
// emits it only once canvas2d_image_build_mips has run, so a mip-less image's
// bilinear-fallback draws replay faithfully.  Emitted at most once per id.
void canvas2d_rec_image_mips(struct canvas2d_recorder *__single r, int id);

// One op line referencing an image block: `name <image-id> <args...>`, the
// args as floats (draw_image / draw_image_scaled / draw_image_subrect) or as
// integers (put_image_data / put_image_data_dirty, whose placement and dirty
// rect are int-typed in the API).
void canvas2d_rec_image_floats(struct canvas2d_recorder *__single r,
                           char const *__null_terminated name, int id,
                           float const *__counted_by(n) v, int n);
void canvas2d_rec_image_ints(struct canvas2d_recorder *__single r,
                         char const *__null_terminated name, int id,
                         int const *__counted_by(n) v, int n);

// `set_fill_pattern <image-id> <repeat-name>` (and the stroke twin): the
// pattern's pixels ride the image block; the repeat mode is written by name.
void canvas2d_rec_pattern(struct canvas2d_recorder *__single r,
                      char const *__null_terminated name, int id,
                      enum canvas2d_pattern_repeat repeat);

// Serialize one Path2D's command list as a numbered `path` block -- a
// `path <id> <ncmds>` header, then one verb line per command (m/l/q/c with
// their points, a/e with a trailing winding bool, t/r/rr, z) -- returning its
// file-local id.  Deduplicated by CONTENT within the recording (the recorder
// keeps its own copy of the command list; the caller's object may be mutated
// or destroyed between draws), so a path stamped under many transforms costs
// one block.  Returns -1, emitting nothing, when the path cannot be carried
// (id space exhausted, or an allocation failure) -- the caller skips its op
// line too.
int canvas2d_rec_path(struct canvas2d_recorder *__single r,
                  struct canvas2d_path2d const *__single p);

// One op line referencing a path block: `stroke_path <path-id>`, or
// fill_path/clip_path with their explicit rule appended by name.
void canvas2d_rec_path_op(struct canvas2d_recorder *__single r,
                      char const *__null_terminated name, int id);
void canvas2d_rec_path_rule(struct canvas2d_recorder *__single r,
                        char const *__null_terminated name, int id,
                        enum canvas2d_fill_rule rule);

// Serialize the derived text data a fill_text/stroke_text op is about to use --
// interned fonts (with their size-1.0 vmetrics), canonical glyph curves + ink
// bounds, color-glyph captures, and the shaped line -- as `font` / `glyph` /
// `bitmap`+`bits` / `shape`+`run` block lines ahead of the op line, so the
// recorded program is self-contained: replay pre-populates the text cache from
// the blocks and never crosses the text boundary, emoji included.
// Deduplicated against what this recording already wrote via the cache slots'
// `emitted` marks (canvas2d_record_to clears them), so a repeated string costs
// one block set per recording.  `family`/`text`/`len`/`size_px`/`rtl`/`ls`/`ws`
// name the cached shaped line (the live lookup has already run; family is the
// requested typeface, rtl the paragraph
// direction and ls/ws the letter/word spacing -- all parts of its key); when it
// isn't cached (shaping failed) nothing is emitted and replay degrades to live
// shaping.  The `shaping` block line always carries the family token and `ls ws`
// (the family and the baked-in spacing key the line), so replay reproduces the
// spaced advances from the block alone.
void canvas2d_rec_text_blocks(struct canvas2d_recorder *__single r, struct canvas2d_text_cache *__single c,
                          char const *__counted_by(fam_len) family, int fam_len,
                          float size_px, bool rtl, float ls, float ws,
                          int weight, bool italic, int kerning, int rendering,
                          int variant_caps, int stretch,
                          char const *__counted_by(lang_len) lang, int lang_len,
                          char const *__counted_by(len) text, int len);

// `set_font_family <name-len> <name-bytes>` -- the fontFamily setter op (the
// name length-prefixed, then the raw bytes to end of line).  Recorded only when
// canvas2d_set_font_family is called, so a program on the default family records
// no such op.  The `shaping` block lines still carry the family regardless (it
// is the cache key).
void canvas2d_rec_font_family(struct canvas2d_recorder *__single r,
                          char const *__counted_by(len) name, int len);

// `set_font_style <normal|italic>` -- the fontStyle setter op, written by token
// like set_direction.  (Weight is an int op via canvas2d_rec_ints.)  Recorded only
// when the setter is called; the shaping/font blocks carry weight/style
// regardless (they are the glyph-cache key for synthesized faces).
void canvas2d_rec_font_style(struct canvas2d_recorder *__single r, enum canvas2d_font_style style);

// `set_font_kerning <auto|normal|none>` / `set_text_rendering
// <auto|optimizeSpeed|optimizeLegibility|geometricPrecision>` -- the shaping-
// toggle setter ops, written by token like set_font_style.  `set_lang <len>
// <bytes>` -- the lang setter op, length-prefixed like set_font_family (an empty
// tag records as len 0).  Recorded only when the setter is called; the `shaping`
// block lines carry the toggles regardless (they are the shaped-line cache key).
void canvas2d_rec_font_kerning(struct canvas2d_recorder *__single r, enum canvas2d_font_kerning kerning);
void canvas2d_rec_text_rendering(struct canvas2d_recorder *__single r, enum canvas2d_text_rendering rendering);
void canvas2d_rec_lang(struct canvas2d_recorder *__single r,
                   char const *__counted_by(len) tag, int len);

// `set_font_variant_caps <normal|small-caps|all-small-caps>` / `set_font_stretch
// <ultra-condensed|...|ultra-expanded>` -- the small-cap / width setter ops,
// written by token like set_font_kerning.  Recorded only when the setter is
// called; the `shaping` block lines carry both regardless (they are the
// shaped-line cache key).
void canvas2d_rec_font_variant_caps(struct canvas2d_recorder *__single r,
                                enum canvas2d_font_variant_caps variant_caps);
void canvas2d_rec_font_stretch(struct canvas2d_recorder *__single r, enum canvas2d_font_stretch stretch);

// `name <ints...>` -- the int-typed op lines with no block reference (resize).
void canvas2d_rec_ints(struct canvas2d_recorder *__single r, char const *__null_terminated name,
                   int const *__counted_by(n) v, int n);

// `name <rule>` -- fill/clip with their per-call fill rule, written by name.
void canvas2d_rec_rule(struct canvas2d_recorder *__single r,
                   char const *__null_terminated name, enum canvas2d_fill_rule rule);

// Enum-valued setters, written by name (the spellings the parser accepts).
void canvas2d_rec_smoothing_quality(struct canvas2d_recorder *__single r,
                                enum canvas2d_image_smoothing_quality quality);
void canvas2d_rec_line_join(struct canvas2d_recorder *__single r, enum canvas2d_line_join join);
void canvas2d_rec_line_cap(struct canvas2d_recorder *__single r, enum canvas2d_line_cap cap);
void canvas2d_rec_composite(struct canvas2d_recorder *__single r, enum canvas2d_composite_op op);
void canvas2d_rec_working_space(struct canvas2d_recorder *__single r,
                           enum canvas2d_color_space space);
void canvas2d_rec_text_align(struct canvas2d_recorder *__single r, enum canvas2d_text_align align);
void canvas2d_rec_text_baseline(struct canvas2d_recorder *__single r,
                            enum canvas2d_text_baseline baseline);
void canvas2d_rec_direction(struct canvas2d_recorder *__single r, enum canvas2d_direction dir);
// `<name> <interp-space> <interp-alpha> <geometry floats...>` -- a
// set_*_gradient op line.  `name` is the op spelling (set_fill_linear_gradient
// and its five siblings); the interpolation rides the line UNCONDITIONALLY,
// `interp_space` named through canvas2d_color_space_name and `interp_alpha`
// through canvas2d_alpha_type_name (the interpolation is required at creation, so
// there is no favoured default to omit), followed by the n geometry floats.
void canvas2d_rec_gradient(struct canvas2d_recorder *__single r,
                       char const *__null_terminated name,
                       enum canvas2d_color_space interp_space,
                       enum canvas2d_alpha_type interp_alpha,
                       float const *__counted_by(n) v, int n);
