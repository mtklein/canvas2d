# Stencils under `-fbounds-safety`: a separable blur, x vs y

The [pixel-pipelines](pixel-pipelines.md) study probed *dispatch*. This one changes
the axis to *memory-access pattern*: a separable box blur — the canonical stencil,
and the shape behind shadow/`filter` blur. A stencil is interesting under
`-fbounds-safety` for two reasons: it reads a *window* around each pixel (so it
indexes outside the current position, into the flag's forward-only `__counted_by`
model), and its two passes have very different memory behaviour (horizontal is
contiguous; vertical strides a full row at a time).

[../src/blur.c](../src/blur.c) is a running-sum box blur of an 8-bit mask, radius `r`,
edges clamped: each pass adds the entering sample and subtracts the leaving one, so
it is O(1) per pixel regardless of `r`. `blur_box_h` walks each row (stride 1);
`blur_box_v` walks each column (stride `w`); `blur_box_v_pf` adds `__builtin_prefetch`
to the strided reads. [../tests/test_blur.c](../tests/test_blur.c) checks correctness
and that prefetch never changes the output.

## Reading a window backward is checked and free to express

The window reads `src[x-r .. x+r]`, which goes *negative* at the left edge. A
`__counted_by(n)` pointer is forward-only — `p[-1]` is out of its view — so this
looks like it should fight the flag. It doesn't, because the pointer you actually
index with is **`__bidi_indexable`**, which a local gets by default when derived from
a counted parameter:

```c
unsigned char const *mid = src + 1;   // bidi: keeps src's lower AND upper bound
... mid[x - 1] ...                    // x-1 == -1 at x=0 -> src[0], in range, checked
```

`mid[x-1]` is bounds-checked against `src`'s real extent (both ends), no forge, and
the same source compiles with *and* without the flag (in the text `mid` is a plain
`unsigned char *`). In practice the blur keeps the clamp-folded index form
(`src[base + clamp(x+k, 0, w-1)]`) so the edges replicate, but the takeaway stands:
centered/backward stencil windows are not where the flag bites.

## Where it bites is the opposite of where you'd guess

512×512, `r = 4`, `-Os`, release (`-fbounds-safety`) vs unsafe, `A`-vs-`A` 1.01×:

| pass | memory pattern | bounds-safe | unsafe | cost of checks |
|---|---|---|---|---|
| horizontal | stride 1, contiguous | 54 ms | 32.5 ms | **1.66×** |
| vertical | stride `w`, a row apart | 90 ms | 91 ms | **~1.0× (free)** |

Two things at once:

- **x vs y**: the vertical pass is ~1.65× slower in release and ~2.8× slower unsafe —
  it strides a whole row between samples, so it misses cache constantly and is
  memory-bound. The horizontal pass streams contiguously and the hardware prefetcher
  keeps it fed.
- **The cost of the checks inverts with the pattern.** Same running-sum, same two
  checked loads + one checked store per pixel — yet the contiguous pass pays **1.66×**
  for the checks while the strided pass pays **nothing** (release even edges ahead of
  unsafe, within noise). The reason is scheduling: the strided pass spends most of its
  cycles *stalled on cache misses*, and the bounds-check integer compares execute for
  free in the shadow of those stalls. The contiguous pass has no stalls to hide
  behind, so the checks land on the critical path.

This refines the pixel-pipelines rule (`-fbounds-safety` cost ≈ check-cost ÷
useful-work-per-access) with a scheduling corollary: **a memory-bound loop hides the
checks; a compute/cache-bound loop exposes them.** The counter-intuitive upshot is
that the *slow* axis is the one where safety is free, and the *fast* axis is where it
costs the most — so the parts of a renderer most worth optimizing for cache (the
strided passes) are exactly the parts where bounds-safety is already free.

## `__builtin_prefetch`: transparent to the flag, and no help here

The open question was whether forming a prefetch address gets bounds-checked.
**It doesn't.** `__builtin_prefetch(&src[i], 0, 0)` emits a `prfm` and adds no `brk`:
`blur_box_v` and `blur_box_v_pf` carry the *same* single bounds-check trap site. A
prefetch is a hint, not an access; the address converts to `void const *` (bounds
dropped, always allowed) and nothing is checked. So prefetch composes with
`-fbounds-safety` at zero cost and needs no forge.

It also didn't *help* — `blur_box_v_pf` ran a hair slower than `blur_box_v` (≈92 vs
90 ms). A constant-stride column walk is the easy case the hardware prefetcher
already handles, and the running sum touches one new sample per step, so there is
little memory-level parallelism for an explicit prefetch to expose; the `prfm` is
pure overhead. Prefetch is a reflex worth resisting until the access pattern is
actually irregular (a gather, a pointer chase) — and when it is warranted, the flag
won't stand in the way.
