# What range facts does `-fbounds-safety` fold?

A bounds check can be elided when the compiler can prove the index is in range. The
[gather/LUT probe](gather-lut.md) found one case it *won't* prove — a `uint8_t` index
into a 256-entry table keeps a check that can never fail. This probe maps the boundary
properly, using a circular buffer ([../src/ring.c](../src/ring.c)) where the index is
computed, not a raw loop counter.

## Only the canonical loop-induction bound is folded

Summing a `__counted_by(cap)` buffer five ways, counting the in-loop bounds-check
branches in the `-Os` output:

| access | in-loop checks | why |
|---|---|---|
| `buf[i]`, `i < cap` | **0** | folded — `i` is the induction variable, bound *is* the count |
| `buf[i & (cap-1)]` | 1 | not folded, though `i & (cap-1) <= cap-1 < cap` always |
| `buf[i % cap]` | 1 | not folded |
| `buf[(uint8_t)i]` into `[256]` | 1 | not folded (the LUT finding) |
| `for i<k` where `k<=cap` | 1 | not folded — it won't chain `i < k <= cap` |
| `for j=tail; j<cap`, signed `tail` | 1 (lower) | `tail`'s sign is unknown, so `j>=0` isn't proven |

So the elision is narrow: the pass folds `i < count` when `i` is the loop's induction
variable and the bound is literally the buffer's count. It does **not** reason about
what masking (`x & m <= m`), modulo (`x % m < m`), an integer type's range, or an
inequality chain guarantee. A computed index keeps its check even when the value is
obviously in range.

Two corollaries that bit while probing:

- **An input guard doesn't propagate.** Adding `if (tail < 0 || tail >= cap) return;`
  up front does not let the pass treat `tail` as non-negative inside a later loop —
  the signed-start lower-bound check stays.
- **The bound must equal the count, not merely be `<=` it.** A loop `for i<k` with `k`
  provably `<= cap` is still checked; only `i < cap` (the count itself) folds.

## So a wrapped ring read is irreducibly checked

The natural "dodge the checks by splitting the wrap into two contiguous runs" doesn't
work. A read of `n` elements from `tail` splits into runs of length `cap-tail` and
`n-(cap-tail)` — **neither equals `cap`**, so neither inner loop's bound matches the
count, and both keep their checks. `ring_sum_masked` and `ring_sum_runs` are both
fully checked; the only check-free form is sweeping the *whole* buffer (`i < cap`),
which reads more than you asked for. Short of `__unsafe_forge`, ring/hash-probe access
pays the check.

## And here the check is expensive

4096-element sum, `-Os`, release (`-fbounds-safety`) vs unsafe, `A`-vs-`A` 1.01×:

| | bounds-safe | unsafe | cost of checks |
|---|---|---|---|
| masked | 95 ms | 58 ms | **1.64×** |
| contiguous runs | 84 ms | 56 ms | **1.49×** |

This is the opposite extreme from the earlier probes. The buffer is L1-resident and
the loop is compute-bound (load, add), so there are **no memory stalls to hide the
checks behind** — they land squarely on the critical path. Compare the memory-bound
cases where the same kind of check was nearly free: the data-dependent LUT (1.05×) and
the vertical blur (~1.0×).

That gives a compact model for the whole exploration:

> **cost of a bounds check ≈ (is it elided?) × (is it hidden by a stall?)**

A register-resident or loop-induction access pays nothing because the check is elided.
A memory-bound access pays little because the check hides in a cache-miss shadow. A
*computed-index, compute-bound* access — a ring buffer, a hash probe — loses on both
counts and pays the full ~1.5–1.6×. That, not raw arithmetic, is where
`-fbounds-safety` actually costs you; and the escape (a forge) is the same
checked-domain exit that [bit-stealing](tagptr.md) needed.
