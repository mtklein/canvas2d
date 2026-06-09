#pragma once

// System-font glyph outlines.  The implementation (cnvs_font_ct.c) is built without
// -fbounds-safety to bind the un-annotated Core Text / Core Graphics headers, and
// hands back ordinary device-space cnvs_paths.  See docs/bounds-safety.md.

#include "cnvs_math.h"
#include "cnvs_path.h"

typedef struct cnvs_font cnvs_font;

// Create a font for `name` (e.g. "Libian TC") at `size_px`; NULL on failure.
cnvs_font *__single cnvs_font_create(char const *__null_terminated name, float size_px);
void cnvs_font_destroy(cnvs_font *__single f);

// `text` is `len` bytes of UTF-8 and need not be NUL-terminated: every walker
// here is length-bounded (it stops at `text + len`, never at a terminator), so a
// caller can hand a slice of a larger buffer directly.  This also hardens the
// shim itself -- built without -fbounds-safety, it can no longer over-read past a
// missing terminator even in that unchecked TU.  See docs/bounds-safety.md.

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
