#pragma once

// The text subsystem: fonts, Core Text shaping, glyph outlines/bitmaps, and text
// metrics.  Core Text shapes UTF-8 into glyph runs inside the unsafe boundary TU
// (cnvs_text_ct.c, built without -fbounds-safety to bind the un-annotated CoreText
// headers); that TU copies each run's glyphs, advances, and cluster map into
// checked-owned __counted_by arrays and hands them back.  The checked core
// (cnvs_text.c) then does layout and hit-testing fully bounds-checked.
//
// The run crosses the C<->C boundary by plain (pointer, count) ABI -- no forge --
// because __counted_by(count) ties the bound to the sibling `count` field and adds
// no hidden field.  The only trust placed in the unsafe side is that `count` matches
// the arrays; a bad cluster *value* is caught by an explicit range check in the core.
// See docs/text-boundary.md.

#include "cnvs_math.h"
#include "cnvs_path.h"

#include <ptrcheck.h>
#include <stdint.h>

// One shaped run: a single font and direction.  Glyphs are in visual (left-to-right)
// order; cluster[i] is the logical UTF-16 index in the source for glyph i (so it
// descends across an RTL run, and skips for a ligature that merged several chars).
typedef struct {
    uint16_t *__counted_by(count) glyph;    // glyph ids
    float *__counted_by(count) xadv;         // x advance per glyph, user px
    int32_t *__counted_by(count) cluster;    // source UTF-16 index per glyph (logical)
    int count;
    bool rtl;
    void *__single font;   // opaque CTFontRef for this run (font fallback), retained
} cnvs_glyph_run;

typedef struct {
    cnvs_glyph_run *__counted_by(nruns) run;
    int nruns;
    int text_len;  // source UTF-16 length, the bound for cluster indices
} cnvs_shaped;

// Shape UTF-8 `text` with font `name` at `size_px`.  Runs come back in visual order.
// NULL on failure.  Implemented in the unsafe boundary TU.
cnvs_shaped *__single cnvs_shape(char const *__null_terminated name, float size_px,
                                 char const *__null_terminated text);
void cnvs_shaped_free(cnvs_shaped *__single s);

// Checked-core consumers.
float cnvs_shaped_width(cnvs_shaped const *__single s);                // sum of advances
int cnvs_shaped_index_at_x(cnvs_shaped const *__single s, float x);    // hit-test -> UTF-16 index, or -1

// A visual x range [x0,x1) in user px from the line start (a selection highlight rect).
typedef struct {
    float x0, x1;
} cnvs_xspan;

// Caret: visual x for a logical UTF-16 index (the leading visual edge of that glyph).
float cnvs_shaped_x_at_index(cnvs_shaped const *__single s, int index);

// Selection: visual x-spans covering the logical range [lo,hi).  A bidi range maps to
// non-contiguous visual positions and so splits into multiple spans; writes up to
// `max`, returns the count.  Pure index logic: the cluster map drives it and every
// access is bounds-checked, no forge -- this is where the checked side earns its keep.
int cnvs_shaped_selection(cnvs_shaped const *__single s, int lo, int hi,
                          cnvs_xspan *__counted_by(max) out, int max);

// Copy a run's font name into `buf` (UTF-8, NUL-terminated within `cap`); returns the
// byte length, or -1.  Boundary helper: the opaque font handle goes in, an output
// buffer the boundary fills within `cap` comes back.  Used to confirm fallback (a run
// that fell back to a different font reports a different name).
int cnvs_run_font_name(void const *__single font, char *__counted_by(cap) buf, int cap);

// Outline a shaped line at pen origin (ox,oy) in user space into `out` (device space,
// mapped by to_device, curves flattened at `tol` px).  Layout -- the pen advance --
// runs checked in the core; each glyph's outline is fetched per-glyph by the boundary
// using its run's font.  Returns the advance width.  Color glyphs (emoji) have no
// outline path and contribute only their advance.
float cnvs_shaped_outline(cnvs_shaped const *__single s, float ox, float oy,
                          cnvs_mat to_device, float tol, cnvs_path *__single out);

// Outline one glyph of the opaque `font` at pen origin (ox,oy).  Boundary helper.
void cnvs_glyph_outline(void *__single font, uint16_t glyph, float ox, float oy,
                        cnvs_mat to_device, float tol, cnvs_path *__single out);

// Draw one glyph of the opaque `font` into an RGBA8 premultiplied buffer at (x,y) in
// bitmap space (origin bottom-left, y up).  The rendering path for color glyphs
// (emoji), which have no outline.  The checked core owns the __counted_by(w*h*4)
// buffer and hands it over; the boundary wraps it in a CGBitmapContext and draws
// within (w,h) -- a pixel buffer crossing checked->boundary, mirror of the glyph run.
void cnvs_glyph_draw(void *__single font, uint16_t glyph, float x, float y,
                     uint8_t *__counted_by(w * h * 4) px, int w, int h);

// Whether `font` is a color (bitmap-glyph) font, e.g. the AppleColorEmoji a mixed
// string falls back to.  Its glyphs have no outline and must be drawn with
// cnvs_glyph_draw rather than cnvs_glyph_outline.  Boundary helper.
bool cnvs_run_is_color(void const *__single font);

// One glyph's tight ink bounding box in font px, baseline-relative, y up (x0,y0 =
// bottom-left, x1,y1 = top-right).  All zero for a blank glyph.  Boundary helper:
// sizes and places the buffer cnvs_glyph_draw fills.
void cnvs_glyph_bounds(void *__single font, uint16_t glyph,
                       float *__single x0, float *__single y0,
                       float *__single x1, float *__single y1);

// A primary font handle: a system typeface at a size, for the cheap font-wide
// metrics (ascent/descent) and the single-font measureText path.  NULL on failure.
typedef struct cnvs_font cnvs_font;
cnvs_font *__single cnvs_font_create(char const *__null_terminated name, float size_px);
void cnvs_font_destroy(cnvs_font *__single f);

// `text` is `len` bytes of UTF-8 and need not be NUL-terminated: every walker here
// is length-bounded (it stops at `text + len`, never at a terminator), so a caller
// can hand a slice of a larger buffer directly -- which also hardens the unchecked
// shim against over-reading a missing terminator.  See docs/bounds-safety.md.

// Advance width of `text` in user px (Canvas measureText().width).
float cnvs_font_advance(cnvs_font *__single f, char const *__counted_by(len) text,
                        int len);

// Full text metrics, all in user px, baseline-relative and laid out from a pen
// origin at x = 0 (the Canvas measureText() defaults: textAlign start / left,
// textBaseline alphabetic).  Sign conventions match TextMetrics: *_left/_ascent
// are positive going left/up, *_right/_descent positive going right/down.
typedef struct {
    float width;                  // advance width
    float actual_left, actual_right;     // actual glyph-ink bounding box (this text)
    float actual_ascent, actual_descent;
    float font_ascent, font_descent;     // font-wide ascent/descent
    float em_ascent, em_descent;         // em square split by the ascent/descent ratio
    float alphabetic_baseline;           // 0 (the reference baseline)
    float hanging_baseline, ideographic_baseline;
} cnvs_text_metrics;

void cnvs_font_measure(cnvs_font *__single f, char const *__counted_by(len) text,
                       int len, cnvs_text_metrics *__single out);

// Font vertical metrics in user px: ascent and descent, both positive magnitudes
// from the baseline.  Cheap (no glyph walk) -- for textBaseline positioning.
void cnvs_font_vmetrics(cnvs_font *__single f, float *__single ascent,
                        float *__single descent);
