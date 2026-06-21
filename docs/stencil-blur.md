# Stencils under `-fbounds-safety`: a separable blur, x vs y

The [pixel-pipelines](pixel-pipelines.md) study probed *dispatch*. This one
changes the axis to *memory-access pattern*: a separable box blur — the
canonical stencil, and the shape behind shadow/`filter` blur. A stencil is
relevant to `-fbounds-safety` for two reasons: it reads a *window* around each
pixel (so it indexes outside the current position, into the flag's forward-only
`__counted_by` model), and its two passes have different memory behaviour
(horizontal is contiguous; vertical strides a full row at a time).

[../src/blur.c](../src/blur.c) is a running-sum box blur of an 8-bit mask, radius
`r`, edges clamped: each pass adds the entering sample and subtracts the leaving
one, so it is O(1) per pixel regardless of `r`. `canvas2d_blur_box_h` walks each row
(stride 1); `canvas2d_blur_box_v` walks each column (stride `w`); a third variant,
`blur_box_v_pf`, added `__builtin_prefetch` to the strided reads (since retired
— see below). [../tests/test_blur.c](../tests/test_blur.c) holds both passes to a
brute-force reference, bit for bit.

## Reading a window backward is checked and free to express

The window reads `src[x-r .. x+r]`, which goes *negative* at the left edge. A
`__counted_by(n)` pointer is forward-only — `p[-1]` is out of its view — but the
pointer actually indexed with is `__bidi_indexable`, which a local gets by
default when derived from a counted parameter:

```c
unsigned char const *mid = src + 1;   // bidi: keeps src's lower AND upper bound
... mid[x - 1] ...                    // x-1 == -1 at x=0 -> src[0], in range, checked
```

`mid[x-1]` is bounds-checked against `src`'s real extent (both ends), no forge,
and the same source compiles with and without the flag (in the text `mid` is a
plain `unsigned char *`). In practice the blur keeps the clamp-folded index form
(`src[base + clamp(x+k, 0, w-1)]`) so the edges replicate, but centered/backward
stencil windows are not where the flag bites.

## Where it bites

512×512, `r = 4`, `-Os`, release (`-fbounds-safety`) vs unsafe, `A`-vs-`A` 1.01×
(both passes as originally written — scalar; each since fixed, see
[the fix](#the-fix-eight-windows-per-step) and
[its sequel](#the-fix-squared-eight-columns-per-step) below):

| pass | memory pattern | bounds-safe | unsafe | cost of checks |
|---|---|---|---|---|
| horizontal | stride 1, contiguous | 54 ms | 32.5 ms | **1.66×** |
| vertical | stride `w`, a row apart | 90 ms | 91 ms | **~1.0× (free)** |

Two things at once:

- **x vs y**: the vertical pass is ~1.65× slower in release and ~2.8× slower
  unsafe — it strides a whole row between samples, so it misses cache constantly
  and is memory-bound. The horizontal pass streams contiguously and the hardware
  prefetcher keeps it fed.
- **The cost of the checks inverts with the pattern.** Same running-sum, same
  two checked loads + one checked store per pixel — yet the contiguous pass pays
  **1.66×** for the checks while the strided pass pays **nothing** (release even
  edges ahead of unsafe, within noise). The reason is scheduling: the strided
  pass spends most of its cycles stalled on cache misses, and the bounds-check
  integer compares execute in the shadow of those stalls. The contiguous pass
  has no stalls to hide behind, so the checks land on the critical path.

This refines the pixel-pipelines rule (`-fbounds-safety` cost ≈ check-cost ÷
useful-work-per-access) with a scheduling corollary: **a memory-bound loop hides
the checks; a compute/cache-bound loop exposes them.** So the *slow* axis is the
one where safety is free, and the *fast* axis is where it costs the most — the
parts of a renderer most worth optimizing for cache (the strided passes) are
exactly the parts where bounds-safety is already free.

## `__builtin_prefetch`: transparent to the flag, no help here

The open question was whether forming a prefetch address gets bounds-checked. It
does not. `__builtin_prefetch(&src[i], 0, 0)` emits a `prfm` and adds no `brk`:
`canvas2d_blur_box_v` and `blur_box_v_pf` carry the same single bounds-check trap site. A
prefetch is a hint, not an access; the address converts to `void const *`
(bounds dropped, always allowed) and nothing is checked. So prefetch composes
with `-fbounds-safety` at zero cost and needs no forge.

It also did not help — `blur_box_v_pf` ran a hair slower than `canvas2d_blur_box_v` (≈92
vs 90 ms). A constant-stride column walk is the case the hardware prefetcher
already handles, and the running sum touches one new sample per step, so there
is little memory-level parallelism for an explicit prefetch to expose; the
`prfm` is overhead. Prefetch is worth reaching for only when the access pattern
is irregular (a gather, a pointer chase) — and when it is warranted, the flag
won't stand in the way.

*(Epilogue: when the vertical pass was later vectorized — see
[the fix, squared](#the-fix-squared-eight-columns-per-step) — the probe was
re-run against the 8-column loop, one `prfm` per eight-pixel step: still a wash,
within noise both checked and unsafe. Both questions answered, the variant was
retired; the finding stands without the code.)*

## The fix: eight windows per step

The contiguous pass pays because it has no stalls to hide the checks behind — so
the cure is the project's standard one (the blit, the gradient fill, the
coverage fill): restructure until one check covers many accesses. The running
sum looks irreducibly serial — each window needs the previous one — but the
recurrence is a prefix sum of entering-minus-leaving samples, and prefix sums
vectorize in-register. Per step, for eight outputs at `x..x+7`:

- the entering samples `src[x+r+1 ..]` and leaving samples `src[x-r ..]` load
  contiguously — **one whole-vector bounds check each**, not sixteen scalar
  ones;
- the *exclusive* 8-lane prefix sum of their difference, added to the carried
  window sum, yields all eight window sums at once (three shift-add steps, the
  coverage resolve's idiom); the carry to the next block is lane arithmetic;
- the `(sum + win/2) / win` quantize runs 8-wide as a float reciprocal multiply
  snapped exact by one remainder comparison — bit-identical to the scalar
  integer division (NEON has no integer divide), held by a brute-force reference
  test.

The clamped edge columns keep the scalar loop, as does the degenerate
`r >= 32768` window, where the float quantize's exactness argument runs out.
Same machine and conditions as above, re-measured the day of the change:

| horizontal pass | bounds-safe | unsafe | cost of checks |
|---|---|---|---|
| scalar (before) | 50.1 ms | 32.4 ms | **1.55×** |
| 8-wide (after) | 34.0 ms | 31.0 ms | **1.10×** |

The *unsafe* build barely moved (32.4 → 31.0 ms — the scalar running sum was
already near the dependency chain's floor without checks), so nearly the entire
32% win in the checked build came from amortizing the checks. Under
`-fbounds-safety`, the restructuring that pays is not always the one that pays
without it — the checks shift the optimization landscape, and a transformation
that looks pointless unchecked (the unsafe delta here is ~4%) can be the
difference between 1.55× and 1.10× checked.

## The fix, squared: eight columns per step

The same recipe then went to the vertical pass, and it is simpler there: columns
are independent, so there is no recurrence to break — eight adjacent columns run
in lanes, each lane carrying its own column's running sum, no prefix sum. Per
step down the image:

- the entering and leaving samples for all eight columns load contiguously from
  one row (`load8_widen` — one whole-vector bounds check each);
- the row-index clamps are *shared by every lane* (they clamp `y`, not `x`), so
  even the edge rows run vectorized — the only scalar work left is the `w%8` tail
  columns and the same degenerate `r >= 32768` window;
- the quantize is the same exact 8-wide `quant8`, so the output is bit-identical,
  held by a brute-force reference test of its own.

Same machine and conditions, same day as the measurement above:

| vertical pass | bounds-safe | unsafe | cost of checks |
|---|---|---|---|
| scalar (before) | 88.9 ms | 90.9 ms | **~1.0× (free)** |
| 8-wide (after) | 15.3 ms | 14.1 ms | **1.09×** |

Two corrections to this doc's earlier conclusions:

- **The "memory-bound" axis sped up ~6×, unsafe included** — it was never pinned
  by DRAM at this size. The blocked walk reuses each 128-byte line across
  sixteen adjacent column blocks, and the full-height stripe of lines it cycles
  through (512 rows × 128 B = 64 KB) sits in L1 — which the scalar walk's lines
  did too. What the scalar loop was spending was per-sample work and latency,
  with enough slack in the shadow of it for the checks to ride free.
- **The checks resurfaced: ~1.0× → 1.09×.** The scheduling corollary cuts both
  ways. Slack hides checks; remove the slack and the bill comes due — now
  amortized to one whole-vector check per load, so it lands at 1.09×, not the
  scalar horizontal's 1.55×.

And the once-slow axis is now the faster one: 15.3 ms vertical vs 34.0 ms
horizontal. Vectorized, the vertical recurrence is cheaper — no prefix sum, no
lane extract on the carry path (the carry between steps is one vector add), and
no scalar edge loops at all.

So the study's earlier conclusion needs an amendment. "The strided passes are
exactly the parts where bounds-safety is already free" was true of the loops as
written, not of the axes: checks being free is a property of a loop with slack,
and slack is a sign the loop has headroom left. The passes where
`-fbounds-safety` costs nothing are the passes not yet optimized — and the
recipe that recovers the cost where it does bite (one whole-vector check
covering eight accesses) is the same one that makes the loop fast.
