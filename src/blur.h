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

// Shift the mask right (down) by k/256 of a pixel, k in [0, 256): translation
// in its convolution form, out[x] = (in[x]*(256-k) + in[x-1]*k + 128) >> 8 --
// the fractional partner of a whole-pixel stamp, so a subpixel shadow offset
// is an integer placement plus one of these per axis.  Exact integer
// arithmetic like the u8 box passes (k = 0 is an exact copy); content
// entering from outside the mask is transparent (zero), not edge-clamped --
// a shifted shadow slides in from nothing.  dst and src must not alias.
void blur_shift_h(uint8_t *__counted_by(w * h) dst,
                  uint8_t const *__counted_by(w * h) src, int w, int h, int k);
void blur_shift_v(uint8_t *__counted_by(w * h) dst,
                  uint8_t const *__counted_by(w * h) src, int w, int h, int k);

// The RGBA16F variants, for a premultiplied colour tile (filter blur()): the
// same running-sum structure over all four channels at once, but out-of-tile
// samples are transparent black (zeros) rather than edge-clamped -- a filtered
// drawing blurs against transparency (CSS filter semantics), so the running
// sum simply adds nothing outside.  Accumulation is f32 (every _Float16 widens
// exactly); each pass rounds back to the tile's own _Float16 precision.  This
// is the one colour kernel deliberately NOT in _Float16 arithmetic under the
// color-axis ruling (docs/decisions/color-axis.md): a running sum is an
// accumulator, and re-rounding it to f16 at every add/subtract drifts,
// compounding over the three h+v passes.
// Blurring all four channels with one kernel preserves the premultiplied
// invariant rgb <= a (a weighted average of pointwise inequalities).
void blur_box_h_f16(cnvs_premul *__counted_by(w * h) dst,
                    cnvs_premul const *__counted_by(w * h) src,
                    int w, int h, int r);
void blur_box_v_f16(cnvs_premul *__counted_by(w * h) dst,
                    cnvs_premul const *__counted_by(w * h) src,
                    int w, int h, int r);
