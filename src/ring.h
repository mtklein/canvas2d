#pragma once

// Range-folding probe: sum n elements from a power-of-2 circular buffer of `cap`,
// starting at `tail`, wrapping (n <= cap, tail in [0,cap)).  Two ways that read the
// same elements, to show which range facts -fbounds-safety folds:
//
//   ring_sum_masked  buf[(tail+i) & (cap-1)] -- the index is <= cap-1 by the nature
//                    of AND, but the flag does NOT fold that, so each access keeps a
//                    (provably-true) bounds check.
//   ring_sum_runs    the same elements as two contiguous runs, each inner loop
//                    bounded by `j < cap` -- a loop-induction bound the flag DOES
//                    fold, so the per-element checks elide.
//
// Same restructuring that dodges the checks is also the cache-friendly one.

#include <ptrcheck.h>
#include <stdint.h>

long ring_sum_masked(int32_t const *__counted_by(cap) buf, int cap, int tail, int n);
long ring_sum_runs(int32_t const *__counted_by(cap) buf, int cap, int tail, int n);
