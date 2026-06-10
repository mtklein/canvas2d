#pragma once

// Separable box blur of a single-channel 8-bit image (e.g. a coverage/shadow mask),
// radius r (window 2r+1), edges clamped.  A probe of how -fbounds-safety interacts
// with stencil access patterns -- specifically the contrast between the horizontal
// pass (stride 1, contiguous) and the vertical pass (stride w, a row apart); the
// study, including a since-retired __builtin_prefetch experiment, is
// docs/stencil-blur.md.
//
// Each pass is a running-sum box blur: O(1) per pixel regardless of r (add the
// entering sample, subtract the leaving one).  dst and src must not alias.

#include <ptrcheck.h>
#include <stdint.h>

// Blur along x: each output row reads its own row, contiguous (stride 1).
void blur_box_h(uint8_t *__counted_by(w * h) dst,
                uint8_t const *__counted_by(w * h) src, int w, int h, int r);

// Blur along y: each output column reads down a column, stride w (a row apart).
void blur_box_v(uint8_t *__counted_by(w * h) dst,
                uint8_t const *__counted_by(w * h) src, int w, int h, int r);
