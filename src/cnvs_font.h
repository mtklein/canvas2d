#pragma once

// System-font glyph outlines.  This header is the bounds-safe boundary; the
// implementation (cnvs_font_ct.c) is the project's one *C* translation unit built
// without -fbounds-safety -- the Core Text / Core Graphics FFI seam, isolated
// exactly like the Objective-C Metal compositor shim.  The system framework
// headers predate -fbounds-safety and carry no bounds attributes, so binding them
// from checked code would mean forging every opaque handle and fighting the
// strict function-pointer check on CGPathApply's callback; instead the seam lives
// in an unchecked TU and everything it produces flows back as an ordinary
// device-space cnvs_path.  See docs/bounds-safety.md.

#include "cnvs_math.h"
#include "cnvs_path.h"

typedef struct cnvs_font cnvs_font;

// Create a font for `name` (e.g. "Libian TC") at `size_px`; NULL on failure.
cnvs_font *__single cnvs_font_create(char const *__null_terminated name, float size_px);
void cnvs_font_destroy(cnvs_font *__single f);

// Append the outlines of `text` (UTF-8) to `out` as device-space subpaths: place
// the baseline origin at user-space (ox, oy), advance +x, flip Core Text's y-up
// glyph space to canvas y-down, map user->device with `to_device`, and flatten
// curves at `tol` device px.  Returns the advance width in user px.
float cnvs_font_outline(cnvs_font *__single f, char const *__null_terminated text,
                        float ox, float oy, cnvs_mat to_device, float tol,
                        cnvs_path *__single out);

// Advance width of `text` (UTF-8) in user px (Canvas measureText().width).
float cnvs_font_advance(cnvs_font *__single f, char const *__null_terminated text);
