# Memo: gradient evaluation — the 1024-entry ramp vs brute-force stop iteration, vectorized across pixels

**Scope read:** `src/canvas2d_gradient.{h,c}` (the shipping path: a 1024-entry colour ramp built
per fill, nearest-entry lookup per pixel; the README documents it as "within 1/255 of the
exact piecewise-linear colour"), `src/canvas.c` (paint_tile: the ramp threshold + per-pixel
loop), `src/canvas2d_planar.h` (the house planar/SoA kernel vocabulary), `docs/decisions/
color-axis.md` (both addenda — the f16 stop lerp and the planar layout this memo's kernel
extends to the gradient), `docs/decisions/opt-level.md` (paired-bench methodology),
`bench/bench_gradient{,_fill}.c`, `tests/test_gradient_solve.c` (the tolerance-0 ramp
identity).
**Measured (this machine, Apple M4 Max, Darwin 25.5, Apple clang 21.0.0, hyperfine 1.20):**
6 paired benches, one hyperfine invocation per bench (`-N --warmup 3 --min-runs 20`), both
binaries built first and **copied out of the tree to `/tmp/gradeval/`**, no compiler running
during timing.  A re-pairing of the two flagship benches at sweep end reproduced their ratios
to 0.5 %; a sentinel re-run of `bench_gradient_fill` drifted +4 % in absolute time but its
*paired ratio* moved 1.242 → 1.23 — the ratio, not the wall clock, is what's quoted.  A
15-cell stop-count × fill-size sweep ran as a separate /tmp harness against a library copy
whose only delta is `CANVAS2D_MAX_STOPS` raised to 32 (the pathological case doesn't fit the
shipping cap).  Quality: ~11M-sample parameter sweeps per gradient (dense uniform t plus
every stop offset and its f32 neighbours and every LUT cell boundary) against an exact
double-precision piecewise-linear reference over the f16 stop colours; gallery pixels via a
/tmp PNG decoder diffing each re-rendered scene against its committed bytes.

**The premise being tested.**  Mike's request, verbatim: *"I'd like to run a comparison of our
LUT gradients against brute force iteration through the stops (vectorized across pixels of
course).  I have a hunch that we can probably get a win/win there in both quality and
performance."*  The shipping path quantizes: every fill ≥1024 px builds a 1024-entry colour
ramp (1024 scalar stop-scan evaluations), then each pixel indexes its nearest entry.  The
challenger evaluates each pixel exactly — find the surrounding stop pair, lerp — with the
stop search vectorized across eight pixels in the house planar style: compares + lane
selects over the sorted stops, no gathers, no table.

## 1. The two designs

**(L) the LUT, shipping:** per gradient fill, `canvas2d_gradient_build_ramp` runs the scalar
stop scan 1024 times into an 8 KB `canvas2d_unpremul` array living in the canvas struct; the
paint loop turns each solved parameter into `ramp[(int)(t * 1023 + 0.5)]` — a scalar
multiply-add, a convert, an 8-byte load.  Fills under 1024 px skip the build and scan stops
per pixel, scalar.  Colour error: nearest-entry quantization, documented ≤1/255.

**(R) brute force, the prototype:** `canvas2d_gradient_color_row` — after the existing 8-wide
parameter solve, a second row kernel evaluates colours eight pixels per step.  Per stop
(interior stops only): one f32 compare of the eight parameters against the stop offset, the
mask narrowed once to 16 bits, then ten bitwise lane selects advance each lane's
lo/hi offset (f32) and lo/hi colour planes (f16, four channels each).  The epilogue is the
scalar lerp's exact arithmetic eight wide — u in f32 with the same true divide and the same
1e-9 span guard, narrowed once to f16, one multiply-add per channel plane — plus three
selects for the first/last/outside edge lanes, then a `vst4q_f16` store through a
`__counted_by(8)` seam (one bounds check per block, the canvas2d_planar.h idiom).  The result is
**bit-identical to `canvas2d_gradient_color_at`, lane for lane** — verified at zero mismatches
over every sweep below, including stop offsets, their ±1-ulp neighbours, coincident-offset
ties, and the n%8 scalar tail — so the scalar scan stays the semantic reference and the row
kernel computes the same semantics eight at a time.  No setup, no table, no threshold.

## 2. Wall clock

Median ± σ in ms, paired (one hyperfine invocation per row, both binaries from /tmp).

| bench | (L) LUT | (R) brute force | R/L |
|---|---|---|---|
| `bench_gradient_fill` — 4-stop radial, one 384² fill | **11.6 ± 0.3** | 14.4 ± 0.3 | **1.24** |
| `bench_gradient` — per-pixel scalar scan (control: same code both arms) | 70.7 ± 0.9 | 70.4 ± 0.4 | 1.00 |
| **`bench_render`** | 17.8 ± 0.2 | **17.4 ± 0.3** | **0.98** |
| **`bench_render_large`** | 179.2 ± 1.5 | **174.2 ± 1.5** | **0.97** |
| `bench` (e2e, codec-bound) | 44.5 ± 0.4 | 44.6 ± 0.3 | 1.00 |
| `bench_fill` (control: no gradient) | 28.5 ± 0.3 | 28.6 ± 0.3 | 1.00 |

**The flagship renders get faster; the gradient microbench gets slower.**  Both flagship
deltas reproduce on re-pairing (0.978 / 0.973) and sit 3–6σ out; the controls pin the noise
floor at ≤0.5 %.  The two results sample opposite ends of §3's
curve.  `bench_gradient_fill` is the LUT's best case by construction: a many-stop gradient
over one large fill, where the 1024-evaluation build amortizes to near zero and the per-pixel
comparison is an L1 load against a 2-iteration select chain.  The renders are the mixed case —
2-stop gradients over small-to-medium fills, where the build is overhead and the
old sub-1024-px fills ran the stop scan scalar, per pixel.  Checked-vs-unsafe overhead on
the new kernel: **1.01×** (`bench_gradient_fill`), 1.00× (`bench_gradient`) — one
whole-vector bounds check per 8-pixel block, same as every planar kernel.

## 3. Speed by stop count and fill size

/tmp harness, linear gradient (the parameter solve is cheap and identical in both arms, so
the colour stage dominates the delta), 5.9M px total per run — many small fills or few large
ones.  `lut` mirrors the shipping paint loop exactly (per-fill ramp build at ≥1024 px,
in-place lookup); `row` mirrors the new one (colour row buffer, then read back).  Medians, ms:

| stops | 384² fills (1 ramp build) | 64² fills (1,440 builds) | 32² fills (5,766 builds) |
|---|---|---|---|
| 2 | lut 6.7 / row 7.5 — **1.11** | lut 10.5 / row 7.3 — **0.69** | lut 20.4 / row 8.2 — **0.40** |
| 5 | lut 6.9 / row 10.7 — **1.54** | lut 11.9 / row 10.8 — **0.91** | lut 25.1 / row 11.9 — **0.48** |
| 8 | lut 7.0 / row 14.2 — **2.02** | lut 13.2 / row 14.6 — **1.10** | lut 30.1 / row 15.7 — **0.52** |
| 16 | lut 7.1 / row 24.0 — **3.38** | lut 16.9 / row 25.2 — **1.49** | lut 44.4 / row 27.1 — **0.61** |
| 32* | lut 7.3 / row 40.5 — **5.51** | lut 24.3 / row 42.3 — **1.74** | lut 73.8 / row 44.7 — **0.61** |

*32 stops needs `CANVAS2D_MAX_STOPS` raised; the shipping API caps at 16, so the in-API worst
case is the 3.38 row.

The shape follows the algorithm: the row kernel costs O(stops) per pixel (≈0.45 ns/px per
interior stop, flat across fill sizes), the LUT costs O(1) per pixel plus O(stops) per fill
in the build (2.3 µs at 2 stops → 13.0 µs at 32, §5).  Typical gradients — 2–5 stops, the
range everything in this tree uses — put the crossover at: row wins everywhere
except a many-stop gradient covering one large area in a single fill.  At 32² fills (the
old ramp threshold boundary) the build is half the LUT's total time and row wins 1.6–2.5×.

**The pathological end, and "would binary search win?"**  Two alternative no-gather
strategies were measured.  A *saturating-sum* kernel (every segment contributes
`Δcolour · clamp01((t-o)/span)`; cheaper per stop, no selects) ties the chain at 2 stops and
loses progressively — 6 % at 5 stops, 17 % at 32 — because its clamp + f32→f16 convert + four
plane multiply-adds per segment outweigh the chain's bsl selects, and it gives up
bit-exactness (f16 accumulation across segments).  Lane-wise *binary search* was not built:
without gathers, materializing each lane's stop pair still takes O(stops) value selects —
the search masks are not the cost — so it cannot beat the linear chain.  Past ~8 stops on
large fills the faster algorithm is a table, i.e. the LUT itself; there is no
vectorized-search advantage there.

## 4. Quality

Max / mean channel error vs the double reference, in 1/255 units, ~11M samples per gradient
(the gallery's actual stop sets plus edge cases):

| gradient | LUT max | LUT mean | brute max | brute mean |
|---|---|---|---|---|
| gradients/linear-2stop | 0.18 | 0.026 | 0.13 | 0.022 |
| gradients/radial-3stop | 0.33 | 0.044 | 0.14 | 0.021 |
| gradients/rainbow-4stop | 0.38 | 0.041 | 0.16 | 0.017 |
| bench/radial-4stop | 0.46 | 0.063 | 0.15 | 0.022 |
| conic/wheel-13stop | 0.45 | 0.095 | 0.08 | 0.015 |
| **conic/pie-10stop-coincident** | **196.42** | 0.090 | **0.00** | 0.000 |
| conic/medallion-5stop | 0.39 | 0.096 | 0.15 | 0.024 |
| edge/16-stop-steep | **0.99** | 0.108 | 0.12 | 0.016 |
| edge/all-coincident | **255.00** | 0.000 | 0.00 | 0.000 |
| edge/translucent-3stop | 0.33 | 0.067 | 0.14 | 0.026 |

Two findings.  On smooth gradients the LUT errs up to
0.46/255 (the documented ~0.4 quantization + ~0.2 f16 lerp) and brute force errs at most
**0.156/255** — f16 lerp rounding, stop-count-independent, less than a third of the
8-bit rounding step.  Second: **the README's "within 1/255" claim does not hold for the
gradients that use coincident stops.**  A hard stop is a colour discontinuity; nearest-entry
lookup moves it to the nearest 1/1023 grid line, so every pixel in the misattributed band
gets the *other sector's colour* — 196/255 on the conic pie's palette, 255/255
constructible.  It also exceeds 1/255 on steep gradients (0.99/255 at 16
stops).  Brute force is *exact* at hard stops (no lerp runs — the segment select is the
discontinuity) and bit-identical to the scalar scan everywhere: **zero mismatches in every
sweep**, which becomes the new tolerance-0 invariant in `test_gradient_solve` (replacing the
ramp identity, whose subject is deleted), alongside a permanent ≤0.25/255
brute-vs-double-reference bound (measured 0.156 + margin).

**Gallery pixels moved: 10 of 33 scenes,** re-rendered both ways and diffed:

| scene | px changed | % of px | max channel Δ | note |
|---|---|---|---|---|
| blend | 12,551 | 4.4 % | 1 | |
| **conic** | 5,032 | 6.5 % | **196** | 13 px at Δ120–196 + 1 px at Δ3: the pie's sector edges land where the offsets actually are; rest Δ1 |
| filters | 10,703 | 3.7 % | 1 | |
| gradients | 2,183 | 6.1 % | 1 | |
| path2d | 382 | 0.3 % | 1 | |
| roundrect | 3,938 | 3.1 % | 1 | |
| rtl | 27 | 0.02 % | 1 | |
| shaping | 39 | 0.02 % | 1 | |
| strokerect | 121 | 0.1 % | 1 | |
| text | 239 | 0.3 % | 1 | |

Nine scenes move only by the sub-quantum 1/255 flips of de-quantized colour; `conic`'s
thirteen large-delta pixels are the exact path correcting sector boundaries the LUT had
displaced by up to half a ramp cell.  No `.canvas` program changes (no gradient scene embeds
readback pixels).

## 5. Setup cost and memory

Ramp build (`canvas2d_gradient_build_ramp`, 1024 scalar evaluations, self-timed median):
**2.3 µs** at 2 stops, 3.0 µs at 5, 3.8 µs at 8, 6.4 µs at 16, 13.0 µs at 32 — paid per
gradient fill ≥1024 px, so a UI-shaped workload of many short-lived gradients pays it over
and over (§3's 32²-fill column is that workload: the build is half the LUT's wall clock).
Plus a permanent 8 KB ramp array in every canvas struct.  Brute force: zero setup, no
threshold logic, and the 8 KB field deleted; the only allocation is a transient
width × 8 B colour-row buffer that grows with (and lives exactly as long as) the existing
parameter-row buffer.

## 6. Options, worked through

| Option | Flagship | Microbench | Quality | Setup/memory | Verdict |
|---|---|---|---|---|---|
| **(L) keep the LUT** | −0 % | fastest on large many-stop fills | ≤0.46/255 smooth, **196/255 at hard stops** | 2.3–6.4 µs/fill + 8 KB/canvas | The hard-stop error rules it out as a default; its speed advantage is on a workload the tree doesn't have |
| **(R) brute force** | **−1.8 / −2.7 %** | 1.24× slower (4-stop 384² radial) | **exact** (bit-equal to the scalar reference; ≤0.156/255 of double) | zero | **Recommended.**  Better on the flagship renders, removes the hard-stop artifact, deletes state |
| (L+R) hybrid: keep both, threshold on stops×area | −1.8/−2.7 % where R runs | LUT speed where L runs | hard-stop error *returns* wherever L runs; two semantics for one paint | both costs + a 2-D threshold | The only fills L would win (≥8 stops, large, smooth) don't occur in the tree; the error returns the day one does |

## 7. Recommendation

**Take (R): replace the ramp with the 8-wide brute-force stop search, re-baseline the ten
moved scenes, and make the exactness the permanent test.**  Reasons:

1. **It improves both quality and performance on the product.**  Quality is
   better on every gradient measured: the LUT's hard-stop
   misattribution (196/255 on a committed scene's palette) is a visible-class artifact, not
   sub-quantum noise.  Performance is better on both flagship renders (−1.8 %/−2.7 %, 3–6σ,
   reproduced) and at worst flat on e2e and every control.
2. **The regression is confined to workloads the tree doesn't run.**  §3 maps it: a
   many-stop gradient over one large fill.  The tree's gradients are 2-stop (renders, blend,
   text, strokes) or many-stop over small conic discs whose wall clock the scalar atan2
   solve dominates.  The in-API worst case (16 stops × one 384² fill) is 3.4× on the colour
   stage alone — and nothing renders it.
3. **It deletes state and a threshold instead of tuning them.**  No 8 KB per canvas, no
   per-fill build, no ≥1024-px special case, no "is the ramp fresh" coupling.  The colour
   path becomes one function with one semantics, bit-equal to the scalar evaluator the tests
   already pin.
4. **The invariants get stronger.**  The ramp identity (tolerance 0 on a
   quantized table) is replaced by row≡scalar bit-equality (tolerance 0 on the *exact*
   evaluator) plus a measured ≤0.25/255 bound against a double reference — the README's
   quality row stops being a claim and becomes a gate.

**The argument against:** `bench_gradient_fill` — the dedicated benchmark of the
exact path being changed — regresses 24 %, and §3 says the gap *grows* with stop count
(3.4× at the API cap).  A canvas workload this tree doesn't have today (full-screen
many-stop gradients: skybox fills, data-viz colour scales, animated multi-stop backgrounds)
would feel it immediately, and the fix would be re-adding a table — i.e.
re-litigating this memo.  The mitigation is that the crossover is *measured* (§3) and
the artifact is *permanent*: if that workload arrives, the move is a table-with-exact-
hard-stops (per-segment LUT, or the lerp-not-nearest variant the old header comment priced)
behind the same exact-semantics tests, not a return to nearest-entry quantization.

**Fork for Mike:** (a) ratify (R) as landed — this memo's pick; (b) revert to (L) and accept
the hard-stop artifact + per-fill build for the 24 % microbench gain, re-documenting the
README's "within 1/255" as "within 1/255 except at coincident stops"; (c) greenlight the
hybrid only if a many-stop large-fill workload shows up with receipts.

## 8. What would change my mind

- **A real workload on the wrong side of §3's crossover.**  A scene or bench that fills
  large areas with ≥8-stop gradients per frame moves the microbench row from "synthetic
  corner" to "the product", and option (c)'s exact-table variant should be built and
  measured.
- **The flagship gains disappear on re-measurement.**  They are 2–3 % effects, 3–6σ here and
  reproduced once; if a future toolchain or kernel change erases them, (R) still stands on
  quality + deleted state, but the performance claim narrows to flat and the memo
  should say so.
- **Output goes beyond 8 bits.**  The f16 lerp's 0.156/255 is invisible at 8-bit output;
  at 10/12-bit it is 0.6/2.5 LSB and the colour pipeline's compute type — not this
  kernel's algorithm — becomes the binding constraint (color-axis.md's problem, not this
  memo's).
- **The select chain stops being the best no-gather search.**  If hardware gathers (or a
  cheap permute-by-index idiom for 16-lane f16) arrive in the kernel vocabulary, the
  O(stops) value-materialization argument in §3 dies and binary search deserves a real
  prototype.
