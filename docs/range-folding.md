# What range facts does `-fbounds-safety` fold?

A bounds check can be elided when the compiler can prove the index is in range. The
[gather/LUT probe](gather-lut.md) found one case it *won't* prove — a `uint8_t` index
into a 256-entry table keeps a check that can never fail. This probe mapped the boundary
properly, using a circular buffer (`ring.c`, since retired — see the epilogue) where the
index is computed, not a raw loop counter.

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
the loop is compute-bound (load, add), so there are no memory stalls to hide the
checks behind — they land on the critical path. Compare the memory-bound cases
where the same kind of check was nearly free: the data-dependent LUT (1.05×) and
the vertical blur (~1.0×).

A model for the cost across these probes:

> **cost of a bounds check ≈ (is it elided?) × (is it hidden by a stall?)**

A register-resident or loop-induction access pays nothing because the check is elided.
A memory-bound access pays little because the check hides in a cache-miss shadow. A
*computed-index, compute-bound* access — a ring buffer, a hash probe — loses on both
counts and pays the full ~1.5–1.6×. That, not raw arithmetic, is where
`-fbounds-safety` costs the most; the escape (a forge) is the same checked-domain
exit that [bit-stealing](tagptr.md) needed.

## Across optimization levels: the check blocks the vectorizer

The project ships `-Os`. Re-running at `-O2`/`-O3` splits into two clean answers.

**Elision is identical at every level.** The in-loop check counts above don't move
between `-Os`, `-O2`, and `-O3` — the pass folds the same loop-induction bounds and
leaves the same computed-index checks in. Higher optimization does *not* add the
value-range analysis that would fold the mask or the modulo.

**But the cost explodes, because the check blocks auto-vectorization.** Same sums,
release (`-fbounds-safety`) vs unsafe, per level:

| check cost | `-Os` | `-O2` | `-O3` |
|---|---|---|---|
| masked | 1.7× | **3.3×** | **3.3×** |
| contiguous runs | 1.6× | **9.6×** | **9.3×** |

The *unsafe* builds vectorize at `-O2`/`-O3` — the contiguous-runs reduction drops
51 → 8.6 ms (~6×), the masked/gather form gets ~1.9× — but the *bounds-checked*
builds don't move (runs: 81 → 83 ms). The per-element trap branch is a data-dependent
exit the vectorizer won't cross, so the checked loop stays scalar. The asm is
unambiguous: at `-O2`, `ring_sum_runs` unsafe has 6 vector loads + 60 vector adds and
no `brk`; the safe build has zero vector ops and one `brk`. The check cost grows from
~1.6× to ~9.6× not because the check got slower but because the baseline got 6× faster
and the checked build couldn't follow.

So the model takes a third factor:

> **cost ≈ (elided?) × (hidden by a stall?) × (blocks vectorization?)**

At `-Os` that third factor is ~1 — the baseline isn't vectorized either — which is why
every probe here measured only a modest flag cost: `-Os` is the favorable case for
`-fbounds-safety`. At `-O2`/`-O3` the third factor dominates for any vectorizable
loop with an un-elided per-element check: the check pins the safe build to scalar
while the baseline pulls away. The register-residency wins ([the pipeline's
channels](pixel-pipelines.md), [the `vtbl` table](gather-lut.md)) are immune — no
per-element check, nothing to block the vectorizer — so "keep the hot index in the
canonical `i < count` form, or move the data into registers" matters more at
production optimization than at `-Os`.

## Explicit SIMD

Autovectorization is unreliable here: it only fires at `-O2`+, only on the simplest
loops, and the flag blocks it for checked code regardless. The alternative is to
vectorize *explicitly* and let `-fbounds-safety` check the loads at a chosen
granularity. `ring_sum_simd` is the same contiguous runs written as a hand SIMD
reduction — a whole-vector bounds-checked block load (`memcpy` of 64 bytes, one
check per 16 elements) into four register accumulators:

| checked form | `-Os` | `-O1` | `-O2` | flag cost |
|---|---|---|---|---|
| scalar runs | 81 ms | 81 ms | 81 ms | (autovec blocked) |
| explicit SIMD | 9 ms | 9 ms | 9 ms | 1.25× |

- **Opt-level-independent.** ~9 ms checked at `-Os`, `-O1`, and `-O2` alike — no need
  for `-O2`, no dependence on the optimizer's mood. That 9 ms equals the `-O2`
  *autovectorized unsafe* build — the one the flag refused to produce for checked code.
- **The flag cost is a knob.** Going from 81 ms to 9 ms re-derived the cost model in
  miniature:

  | explicit SIMD variant | checked | flag cost | why |
  |---|---|---|---|
  | 1 accumulator | 26 ms | 1.18× | latency-bound; checks hidden in the add-dependency stall |
  | 4 accumulators, 4 checks/16 | 19 ms | 2.6× | stall gone → the per-vector checks are exposed |
  | 4 accumulators, 1 check/16 | 9 ms | 1.25× | checks amortized over the block *and* latency hidden |

  So explicit SIMD under the flag wants both **enough ILP to saturate** (several
  accumulators) and **coarse checks** (one bounds-checked block load, then compute in
  registers) — the same amortize-and-keep-it-in-registers principle that made
  [the register pipeline](pixel-pipelines.md) and [the `vtbl` LUT](gather-lut.md) free,
  here applied to check *granularity* rather than eliminating the check.

Summary: `-fbounds-safety` is friendly to *explicit* vectorization (the
whole-vector load is one cheap, amortizable check) and hostile to the
*auto*vectorizer (the per-element check is a barrier it won't cross). Vectorize
explicitly and pick the check granularity; `-O2` does not rescue a checked scalar
loop.

## Epilogue: the probe is retired

The circular buffer this study measured (`ring.c`/`ring.h`, the `ring_sum_masked` /
`ring_sum_runs` / `ring_sum_simd` variants, their test and three benches) was a
self-contained probe, never wired into the renderer. The cost model it built —
*elided? × hidden by a stall? × blocks vectorization?* — applies across the live
kernels: keep the hot index in the canonical `i < count` form or move the data
into registers, and vectorize explicitly to choose the check granularity. The
code has been retired from the tree; git history holds the modules.
