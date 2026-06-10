#pragma once

// Separable box blur, radius r (window 2r+1), in two flavours: a single-channel
// 8-bit pass for coverage/shadow masks (edges clamped), and an RGBA16F pass for
// premultiplied colour tiles (filter blur(); out-of-tile samples are transparent).
// The u8 passes were a probe of how -fbounds-safety interacts with stencil access
// patterns -- specifically the contrast between the horizontal pass (stride 1,
// contiguous) and the vertical pass (stride w, a row apart); the study, including
// a since-retired __builtin_prefetch experiment, is docs/stencil-blur.md.
//
// Each pass is a running-sum box blur: O(1) per pixel regardless of r (add the
// entering sample, subtract the leaving one).  dst and src must not alias.

#include "cnvs_math.h"  // cnvs_premul

#include <ptrcheck.h>
#include <stdint.h>

// Blur along x: each output row reads its own row, contiguous (stride 1).
void blur_box_h(uint8_t *__counted_by(w * h) dst,
                uint8_t const *__counted_by(w * h) src, int w, int h, int r);

// Blur along y: each output column reads down a column, stride w (a row apart).
void blur_box_v(uint8_t *__counted_by(w * h) dst,
                uint8_t const *__counted_by(w * h) src, int w, int h, int r);

// The RGBA16F variants, for a premultiplied colour tile (filter blur()): the
// same running-sum structure over all four channels at once, but out-of-tile
// samples are transparent black (zeros) rather than edge-clamped -- a filtered
// drawing blurs against transparency (CSS filter semantics), so the running
// sum simply adds nothing outside.  Accumulation is f32 (every _Float16 widens
// exactly); each pass rounds back to the tile's own _Float16 precision.
// Blurring all four channels with one kernel preserves the premultiplied
// invariant rgb <= a (a weighted average of pointwise inequalities).
void blur_box_h_f16(cnvs_premul *__counted_by(w * h) dst,
                    cnvs_premul const *__counted_by(w * h) src,
                    int w, int h, int r);
void blur_box_v_f16(cnvs_premul *__counted_by(w * h) dst,
                    cnvs_premul const *__counted_by(w * h) src,
                    int w, int h, int r);
