# Memo: pricing the color axis — f32 everywhere, f16 storage + f32 compute, or f16 everywhere?

**Scope read:** `docs/decisions/float16-color-type.md` (whose §6.4(a) "run the f32-tile
production benchmark" is the open item this memo addresses), `docs/decisions/metal-backend.md` (the D1
ruling: Metal and the RTZ hack are gone, so there is no shader left to bit-match — both
non-status-quo arms are clean to run), `docs/decisions/opt-level.md` (methodology),
`src/canvas2d_math.{h,c}`, `src/compositor_cpu.c`, `src/blur.{h,c}`, `src/canvas2d_filter.c`,
`src/canvas2d_gradient.{h,c}`, `src/canvas.c` (paint_tile / read_unpremul / put_image_sub flows),
`bench/` (the 8 reported benches), `tests/test_gradient_solve.c`, `tests/test_replay_gallery.c`.
**Measured (this machine, Apple M4 Max, Darwin 25.5, Apple clang 21.0.0, hyperfine 1.20):**
3 arms × 8 benches, `hyperfine -N --warmup 3 --min-runs 20` (fast benches got 65–290 runs from
hyperfine's 3 s floor), every binary built first and **copied out of the tree to
`/tmp/coloraxis/`**, all timing runs on a quiet machine with no compiler running.  A sentinel
(status-quo `bench_fill`) re-run at sweep end came back **+1.1 %** off its original median —
about one σ on that row and an order of magnitude below the effects reported, so no thermal
correction applied (the four control rows in §2 bound the same noise independently).  `__text`
sizes via `size -m`.  Pixel deltas: all 32 gallery scenes rendered per arm and compared to the
committed PNGs with a /tmp decoder; the tree was `git restore`d after each arm.  Accuracy:
two exhaustive /tmp experiments (65,280 premultiply round-trips; 16.7M source-over triples vs
a double-precision reference) modeling each arm's exact store/compute semantics, plus the
float16 memo's iterated-trail pathology re-run with f16 *compute*.

**The premise being tested.** Since the float16 memo, the internal pipeline has been
`_Float16` storage with f32 compute: every kernel widens at load and narrows at store, and the
8-wide-f16-SIMD argument in the README's orbit has never been realized in production code.
That memo's recommendation rested on one unmeasured claim — that f32 tiles cost performance —
and the roadmap has carried a three-way fork since:

- **(a) f32-everywhere** — storage AND compute in float: doubles tile/target bytes, deletes
  every convert instruction;
- **(b) status quo** — `_Float16` storage, f32 compute: half the bytes, converts at every
  load/store boundary;
- **(c) pervasive f16 compute** — storage and arithmetic in `_Float16`, 8 lanes where the
  kernels currently go 4-wide f32 (Apple Silicon has native fp16 SIMD arithmetic).

## 1. What each prototype actually converted

The numbers only mean what the prototypes did.

**Arm (a)** was a *total* mechanical retype: `_Float16` → `float` across 9 files (46 lines —
`canvas2d_math.{h,c}`, `compositor_cpu.c`, `blur.{h,c}`, `canvas.c`, `canvas2d_filter.c`,
`canvas2d_gradient.{h,c}`).  The two structs, every vector typedef, every cast; the
`__builtin_convertvector` widen/narrow sites become same-type no-ops the compiler deletes.
Nothing was left in f16 — `canvas2d_premul`/`canvas2d_unpremul` are 16 B, the tile, target, and
gradient ramp double in footprint.  Builds `-Weverything`-clean with no other change.

**Arm (c)** was three hand edits (~80 lines), covering the kernels the flagship profile
actually passes through:

- `compositor_blend`'s SRC_OVER kernel: the whole blend in `_Float16` arithmetic, **two
  pixels (8 lanes) per step** where the f32 kernel did one pixel (4 lanes) — load f16, blend
  f16, clamp f16, store f16, zero converts; clip attenuation in f16 too (note: this trades
  away the float32 attenuation the metal-backend ruling kept; see §6's counter-argument);
  odd-width tail does one pixel at 4 lanes.
- `canvas2d_premultiply`: multiply and clamp in f16 directly (was: widen, f32 multiply, narrow).
- `read_unpremul`: un-premultiply divide, clamp, and 255-quantize in f16 (was: f32 divide
  with an f16 double-round before quantizing).

**Left in f32 under arm (c), deliberately:** the generic blend modes (`blend`/`blend_term`/
the non-separable HSL path — still scalar f32 with widen/narrow at the edges), the filter
color-matrix loop (`canvas2d_filter_apply`), the f16 blur passes' f32 accumulation, the gradient
stop interpolation (`canvas2d_gradient_color_at`, which narrows per `canvas2d_unpremul_of` as today),
paint_tile's scalar coverage/alpha fold, and the shadow tint loop.  None of these are
SRC_OVER-shaped hot paths, but the generic modes are not negligible: a `sample` of the
status-quo flagship attributes ~9 % of top-of-stack time to `blend_term` (the MULTIPLY/
SCREEN/LIGHTEN composites in the scene).  So arm (c)'s flagship number is a lower bound —
a full conversion has that headroom left.

For attribution: the same sample puts ~50 % of `bench_render_large`'s top-of-stack time in
the kernels arm (c) converted (`compositor_blend` 34 %, `canvas2d_premultiply` 12 %,
`read_unpremul` 4 %), ~26 % in `paint_tile` (the f32 coverage fold + ramp lookup, untouched),
and the rest in rasterization/gradient/clip, which no arm touches.

## 2. Wall clock

Median ± σ in ms; ratio columns are each arm's median over status quo (b).  The last four
rows never touch a `canvas2d_premul` (the u8 blit, the coverage rasterizer, the u8 blurs, the
ramp+solve loop with f16-storage entries) and serve as controls: their spread (≤1.7 %,
both directions) is the noise floor, and the blur rows in particular confirm the arms'
library-wide retypes didn't perturb untouched kernels.

| bench | (a) f32 | (b) status quo | (c) f16 8-wide | a/b | c/b |
|---|---|---|---|---|---|
| **`bench_render`** | 24.7 ± 0.3 | 25.9 ± 0.3 | **23.7 ± 0.3** | 0.955 | **0.917** |
| **`bench_render_large`** | 261.0 ± 0.9 | 274.0 ± 1.0 | **252.9 ± 1.0** | 0.952 | **0.923** |
| `bench` (e2e) | 45.1 ± 0.4 | 45.6 ± 0.4 | 45.2 ± 0.3 | 0.991 | 0.992 |
| `bench_blit` (control) | 8.1 ± 0.3 | 8.2 ± 0.3 | 8.3 ± 0.3 | 0.988 | 1.010 |
| `bench_fill` (control) | 28.2 ± 0.4 | 28.7 ± 0.3 | 28.9 ± 0.4 | 0.983 | 1.007 |
| `bench_blur_h` (control) | 32.3 ± 0.3 | 32.6 ± 0.3 | 32.4 ± 0.3 | 0.991 | 0.993 |
| `bench_blur_v` (control) | 14.1 ± 0.2 | 14.1 ± 0.2 | 14.1 ± 0.2 | 0.999 | 1.002 |
| `bench_gradient_fill` (control) | 11.9 ± 0.3 | 12.0 ± 0.3 | 12.2 ± 0.3 | 0.990 | 1.015 |
| **geomean** | | | | 0.981 | 0.981 |

**The status quo is slowest on the flagship benches.**  On the two real-pipeline
benches — the project's product — f16-storage-with-f32-compute is the slowest of the three
arms: −4.5 %/−4.8 % to f32-everywhere, −8.3 %/−7.7 % to pervasive f16.  The geomean ties at
0.981 for both arms only because the control rows dilute it; on the rows where the color
pipeline runs at all, the ordering is stable (gaps of 5–8× σ).

The mechanism follows §1's attribution.  Arm (b) pays a convert at every boundary
of every hot loop and gets nothing back for it.  Arm (a) deletes the converts but doubles the
bytes through tile, target, and the readback temp — a net win over (b), so the conversion
overhead was worth more than the bandwidth, the opposite of what the float16 memo's pixvm
proxy suggested for the checked build.  Arm (c) deletes the converts *and* keeps the bytes
*and* doubles the lanes — it stacks both wins and is fastest, bounds checks and all.
The README's "8-wide f16 = 128-bit NEON" argument, unrealized in production code since birth,
is confirmed: this is the measurement behind it.

## 3. Code size

`__text` of the e2e `bench` binary (links the whole library); all three `__TEXT` segments
round to the same 131,072 B.

| arm | `__text` | vs status quo |
|---|---|---|
| (a) f32 | 109,800 B | −0.2 % |
| (b) status quo | 110,028 B | — |
| (c) f16 8-wide | 110,332 B | +0.3 % |

Size is unchanged in either direction.

## 4. Pixels: what a switch costs the gallery

Each arm's 32 freshly-rendered scenes vs the committed PNGs (max channel delta in /255
units, per scene; the tree was restored afterwards):

| | (a) f32 | (c) f16 compute |
|---|---|---|
| scenes changed | **32 of 32** | **32 of 32** |
| max channel delta | 1, except `subrect` = **2** | **1** everywhere |
| pixels changed, median scene | ~65 % | ~66 % |
| pixels changed, range | 0.2 % (`imagedata`) – 97 % (`textgrid`) | 0.2 % – 98 % |

Both arms move pixels essentially everywhere (anything antialiased or translucent shifts by
one quantum), and both stay at the 1/255 scale — the lone 2/255 is arm (a)'s `subrect`, a
scratch-canvas readback redrawn through the pipeline (two quantization regimes compound).
The more precise arm produces the larger committed
diff, because the baseline is not truth, it is f16-storage rounding.

**Re-baseline price, measured:** flipping either arm means one commit of
all **32 PNGs plus 2 `.canvas` programs** (`imagedata.canvas`, `subrect.canvas` — the two
whose recorded image blocks embed readback pixels).  The workflow supports it (`ninja
images` regenerates everything in lockstep, `test_replay_gallery` re-gates determinism), and
the committed-PNG decision memo prices this class of churn as routine.  Review of
that commit relies on the harness (no human inspects 32 diffs at 1/255).

## 5. Tests and the two accuracy experiments

**Unit tests.**  Arm (c) fails exactly one: `test_replay_gallery` (byte-compares replays to
the committed PNGs — §4's re-baseline, not a bug).  Arm (a) fails that plus
`test_gradient_solve`: the test asserts the documented
identity `build_ramp[i] == color_at(i/(N-1))` at **tolerance zero**, but `build_ramp`
computes `t = i * (1/(N-1))` while the test computes `i / (N-1)` — a 1-ULP f32 difference in
`t` that f16 narrowing has been absorbing.  Rounding-scale, fixable
(divide in the ramp loop, or loosen the tolerance), and a finding in its own right: **some of
the tree's "exact" invariants are exact only because f16 storage forgives the last f32 bit.**
Everything else — `test_compositor`'s blend constants, `test_composite`, `test_image`'s
get/put round-trip, `test_metamorphic`, `test_shadow` — passes under **both** arms, including
arm (c)'s f16 source-over, premultiply, and readback.

**Experiment 1 — the float16 memo's round-trip claim, re-checked for f16 *compute*.**  All
65,280 (u8 color × u8 alpha) pairs through premultiply → store → unpremultiply → 8-bit
quantize, modeling each arm exactly (arm (c) does the premultiply *and* the divide *and* the
255-quantize in f16):

| arm | mismatched pairs |
|---|---|
| (a) f32 | 0 |
| (b) f16 store / f32 compute | 0 |
| (c) f16 store / **f16 compute** | **0** |

**The memo's key claim survives compute narrowing.**  f16 arithmetic — not just storage —
round-trips every 8-bit edge value.  The u8-corrupts-half row from the prior memo is
unaffected and still applies to u8; nothing here reopens that.

**Experiment 2 — a representative blend.**  All 16,777,216 (src color × src alpha × dst
color) triples through source-over onto an opaque destination, vs the correctly-rounded
double-precision reference:

| arm | off-by-one results | max delta | exact-8-bit-edge results hit (of 447,436) |
|---|---|---|---|
| (a) f32 | **0 (0.000 %)** | 0 | 447,436 (100 %) |
| (b) status quo | 507,756 (3.03 %) | 1 | 447,436 (100 %) |
| (c) f16 compute | 811,369 (4.84 %) | 1 | 447,436 (100 %) |

Three readings.  First, **arm (a) is bit-exact against the double oracle across the entire
sweep** — f32 compute with f32 storage is the correctly-rounded answer for this kernel, a
property the metal-backend memo's replacement-oracle agenda relies on.
Second, arm (c) widens the off-by-one band over the status quo by 1.8 points of blend space —
monotone with how much f16 rounding accumulates, and below the 1/255 scale.  Third,
**every result that should land exactly on an 8-bit edge does, in all three arms** — the
direct answer to this memo's assigned question: f16 compute does not miss the edges.

**The trail pathology, re-run with f16 compute** (iterated white-at-α over black; limit 255;
the prior memo's worst case for f16, now RNE since the RTZ hack died with Metal):

| α / iterations | (a) f32 | (b) f16 store | (c) f16 compute |
|---|---|---|---|
| 0.05 / 2,000 | 255 | 254 | 255 |
| 0.02 / 5,000 | 255 | 252 | 252 |
| 0.01 / 10,000 | 255 | 249 | 255 |
| 0.004 / 20,000 | 255 | 240 | 245 |

Pervasive f16 is *not worse* than f16 storage here — the f16-arithmetic
fixed point lands at or above the store-rounded one at every tested α.  f32 remains
the only arm that reaches the limit, but arm (c) does not deepen the prior memo's
conceded artifact.

(Modeling caveat for all three experiments: straight-line C mirrors of the kernels, compiled
with the build's flags; vector codegen could contract FMAs differently in corner cases.  The
gallery deltas in §4 — max 1/255 across 3.3M rendered pixels — corroborate the scale.)

## 6. Options, worked through

| Option | Flagship | Memory (tile+target) | Blend accuracy vs oracle | Switch cost | Verdict |
|---|---|---|---|---|---|
| **(b) status quo** | slowest of the three (−0 %) | 8 B/px | 3.0 % off-by-one | — | Neither fastest nor most accurate; the no-churn option |
| **(a) f32** | −4.5/−4.8 % | **16 B/px** (doubles) | **bit-exact** | 32 PNGs + 2 .canvas + 2 test fixes | The correctness arm: correctly rounded by construction, removes ~60 convert sites, deepest pathology headroom — for 2× footprint and second on speed |
| **(c) f16 8-wide** | **−8.3/−7.7 %** | 8 B/px | 4.8 % off-by-one (max 1/255; all exact edges hit) | 32 PNGs + 2 .canvas + 1 test re-render | **Recommended.** Fastest, smallest, round-trip-exact, and makes the README's f16 story accurate |

## 7. Recommendation

**Take arm (c): convert the SRC_OVER blend, premultiply, and readback to pervasive 8-wide
`_Float16` compute, and re-baseline the gallery.**  Ranked reasons:

1. **It is fastest on the workload that matters.**  −8 % on
   both flagship renders is the largest single-change speedup measured on this pipeline since
   the memos began, it comes from the shipping checked build, and §1 shows it is a lower bound
   (the generic modes' ~9 % is still on the table).
2. **The status quo is the worst point on the curve.**  It pays f16's rounding
   *and* f32's conversion traffic and collects neither arm's win.  "f16 storage, f32 compute"
   was the default when the Metal shader dictated the storage format; with Metal
   deleted, it is the slow middle.
3. **The accuracy cost is bounded and the load-bearing claims survive.**  Every 8-bit edge
   round-trips (exp 1), every exactly-representable blend result lands (exp 2), the trail
   pathology doesn't deepen (§5), max delta anywhere is 1/255, and the entire committed test
   suite minus the re-baseline gate passes unmodified.  Conceded: off-by-one on ~5 %
   of translucent blends vs ~3 % today — sub-quantum, with no committed artifact, test,
   or doc claim that can see it.
4. **It matches the README's story to the code instead of rewording it.**  The float16 memo found the
   "lingua franca, native on this hardware" line described code that didn't exist — f16 never
   computed.  Arm (c) is that code.  After the switch, the line is true, and the
   `canvas2d_math.h` comment's "compute happens in f32" clause updates to match.

**Execution notes for the real change (beyond this memo's prototype):** convert the generic
blend modes too (or document the kernel as two-language); decide clip attenuation
deliberately — the prototype attenuates in f16, which is pervasive but reverses the
float32-attenuation choice the metal-backend ruling kept on correctness grounds (keeping the
f32 multiply there costs two converts per clipped pixel; either way, note it in the code);
re-render the gallery (`ninja images`, one commit: 32 PNGs + `imagedata.canvas` +
`subrect.canvas`); and re-run `ninja benchcmp` so the README's overhead table reflects the
new kernels.

**The strongest argument against this recommendation:** arm (a) is the only arm that is
**correctly rounded by construction** — zero error against a double-precision oracle over the
entire 16.7M-triple sweep — and the metal-backend memo's replacement-oracle agenda
points toward that property: a pipeline verifiable against `double` with `==`
rather than `±1`.  Choosing (c) chooses a renderer that is permanently one ulp off on a
twentieth of blend space, forecloses tolerance-0 differential testing against a reference
implementation, and trades for speed (−3 % vs (a)) and footprint (2× vs (a)) — the
latter mattering only if canvases grow (a 1920×1080 target is 16.6 MB vs 33 MB; at 4K
it's 66 vs 133).  If the project's thesis drifts from "fast checked C" toward "provably
correct checked C," (a) was the right fork and this memo's tables — flagship −4.8 %, edge
fidelity perfect, one extra test fix — price that road too.  A second concession:
arm (c)'s win rests on hand-vectorized fp16 NEON; if the code ever targets a machine without
native fp16 arithmetic, the f16 kernels demote to soft-float, where (a) and (b)
degrade gracefully.

**Fork for Mike:**
- **(c) — this memo's pick:** pervasive f16, −8 % flagship, re-baseline 32 PNGs + 2 .canvas,
  one test re-render; accept the 4.8 % off-by-one band and update `canvas2d_math.h`/README to the
  now-accurate story.
- **(a) — the correctness fork:** f32 everywhere, −5 % flagship, same re-baseline churn plus
  the `test_gradient_solve` 1-ULP fix; buy bit-exactness against a double oracle and the
  option of tolerance-0 reference differentials, pay 2× color-buffer memory.
- **(b) — ratify the status quo:** zero churn, keep the committed baseline byte-stable; spend
  the 5–8 % flagship margin on not re-reviewing 32 PNGs.  Defensible only if baseline
  stability outranks the flagship benches.

## 8. What would change my mind

- **A reference-renderer differential lands on the roadmap.**  The moment canvas2d wants
  tolerance-0 comparison against a double-precision oracle (the metal memo's D2 spirit),
  arm (a)'s zero-mismatch column becomes the decisive row and this recommendation flips.
- **The off-by-one band shows up in an artifact.**  No committed test or scene can see ±1/255
  today; if a future scene (long fade loops, repeated filter passes, accumulation buffers)
  surfaces a visible f16-compute artifact the trail experiment missed, re-run §5 with that
  workload before defending (c).
- **Canvas sizes grow past the cache.**  Arm (c) already wins partly on bandwidth; at 4K+
  targets the f32 footprint penalty doubles down and the (a)-vs-(c) gap should widen in (c)'s
  favor — but *measure*, because the readback temp and PNG encode also scale.
- **Portability beyond Apple Silicon materializes.**  Native fp16 SIMD is the load-bearing
  assumption; an x86 target without AVX-512 FP16 demotes arm (c) to scalar soft-float and
  the right answer is probably (a) with 8-wide f32.
- **The generic-mode conversion disappoints.**  If converting `blend_term`/HSL to f16 costs
  accuracy on the non-linear modes (divides and square roots in 11 bits) in a way the
  porterduff/blend scenes can see, the "two-language kernel" residue becomes permanent and
  (c)'s simplicity argument weakens — the §1 attribution caps the residual upside at ~9 %.

## Ruling (2026-06-10)

**Mike ratified arm (c)** — "the coolest, the fastest, the most interesting; portability
doesn't matter right now."  Landed as the full conversion, not just this memo's
prototype scope:

- **Converted to `_Float16` arithmetic:** the SRC_OVER kernel (8-wide, two pixels per
  step), the generic kernel — all 26 composite/blend modes, Porter-Duff and separable
  vectorized over the four channels, the HSL/divide/sqrt modes scalar f16 — the filter
  colour-matrix loop (8-wide, two pixels per step), the gradient stop lerp (the
  parameter solve stays f32: it is geometry, not colour), `canvas2d_premultiply`,
  `canvas2d_unpremultiply`, and `read_unpremul`.  Clip attenuation went f16 per §7's
  execution note, deliberately reversing the float32-attenuation choice the Metal era
  kept (noted in the kernel).
- **Left f32, measured:** the blur passes' running-sum accumulation.  Re-rounding the
  accumulator to f16 at every add/subtract drifts up to ~0.9/255 per pass over a 512 px
  row (f32: 0.06/255), compounding over the three h+v passes — it cannot hold
  test_filter's 2/255 brute-force reference, and no profile puts these passes on a hot
  path that would pay for it (the rationale lives in `canvas2d_blur.h`).  Also left f32:
  paint_tile's scalar coverage/alpha fold and the drop-shadow tint/under-composite
  loops — per-pixel scalar paths off the §1 attribution.
- **§5's gates are now committed tests:** `test_image` runs the exhaustive 65,280-pair
  premultiply round-trip end to end (still 0 mismatches under f16 compute), and
  `test_compositor` sweeps source-over against a correctly rounded double reference
  (3.4M triples, max delta 1/255).  `build_ramp`'s 1-ULP identity fragility (§5's
  finding) is fixed by construction — a true divide per entry — so
  `test_gradient_solve` keeps its tolerance-0 check.
- **The §6 result exceeded the prototype estimate.**  Full-conversion measurement (paired
  hyperfine on copied-aside binaries, medians): `bench_render` 26.3 → 22.4 ms
  (**−14.9 %**), `bench_render_large` 275.0 → 238.8 ms (**−13.2 %**), e2e `bench`
  −2.2 %, all four controls within noise (0.99–1.00).  The §1 lower bound held:
  converting the generic modes roughly doubled the prototype's −8 %.
- **Re-baseline as priced:** all 32 PNGs plus `imagedata.canvas`/`subrect.canvas`,
  max channel delta 1/255 in every scene, committed in lockstep with the kernels that
  moved them; `test_replay_gallery` stayed byte-green throughout.

## Layout addendum (2026-06-10): the f16 kernels went planar

The ruling's kernels were AoS -- pixels interleaved in the vector, alpha splatted
across channel lanes by shuffles, and the HSL quartet + soft-light still scalar f16
because per-pixel branches don't vectorize across interleaved lanes.  The layout
refactor (task #21) re-shaped every bulk f16 kernel to **planar (SoA)**: eight
pixels per step as four 8-lane channel planes, deinterleaved at the buffer seams by
explicit `vld4q_f16`/`vst4q_f16` (and `vld4_u8`/`vst4_u8` at the RGBA8 seams) --
there is no portable spelling for those, and the f16 ruling already spent
portability.  The shared vocabulary lives in `src/canvas2d_planar.h`; stages pass
whole blocks as a four-vector HVA, in registers (q0-q3), so the pipeline stays
factored as small functions with vector ABIs rather than fused into one function.

**Converted:** the SRC_OVER kernel, the generic 26-mode kernel (per-pixel branches
became bitwise lane selects; HSL/dodge/burn/soft-light are now straight-line vector
code with one fsqrt.8h), the filter colour-matrix (its four splat shuffles per step
deleted outright -- a coefficient is a scalar broadcast against a plane), readback
un-premultiply, and putImageData premultiply.  **Left AoS:** the gradient stop lerp
(one colour behind a data-dependent stop search; no natural 8-batch exists) and the
blur passes (f32 running-sum accumulators per this ruling -- planar restructuring
must not re-round an accumulator; never on a hot path).

**Pixels did not move.**  Same arithmetic per lane, different layout: every commit
held all 33 gallery PNGs byte-identical, and a differential sweep of the planar
26-mode kernel against the scalar form (78M random + edge-value pixels, subnormals
and branch thresholds included) found zero bit mismatches.  Two idioms made that
exact: selects are bitwise (an arithmetic `b + (a-b)*m` select poisons on a guarded
divide's inf/NaN), and min/max stay spelled as compare+select, not fminnm/fmaxnm,
which disagree on signed zeros.  (2026-06-11 update: with the scalar reference
retired, the simplification sweep moved min/max to the fminnm/fmaxnm form —
no output byte sees the difference, and none did.  Bitwise selects remain
doctrine; they exist for correctness, not bit-matching.)

**Measured** (paired hyperfine, both binaries in one invocation, copied to /tmp,
quiet machine, medians ± σ; sentinel re-run drifted +1.9%, ~1.3σ):

| bench | AoS (ruling) | planar | planar/AoS |
|---|---|---|---|
| `bench_render` | 21.4 ± 0.3 ms | **17.6 ± 0.2 ms** | **0.825** |
| `bench_render_large` | 237.6 ± 0.7 ms | **178.8 ± 0.8 ms** | **0.752** |
| `bench` (e2e) | 44.6 ± 0.3 ms | 44.6 ± 0.4 ms | 1.001 |
| `bench_fill` | 28.9 ± 0.3 ms | 28.4 ± 0.2 ms | 0.983 |
| `bench_gradient_fill` | 12.0 ± 0.2 ms | 12.1 ± 0.2 ms | 1.003 |
| `bench_blit` (control) | 8.3 ± 0.2 ms | 8.3 ± 0.2 ms | 0.998 |
| `bench_blur_h` (control) | 32.6 ± 0.4 ms | 32.7 ± 0.2 ms | 1.002 |
| `bench_blur_v` (control) | 14.0 ± 0.1 ms | 14.1 ± 0.1 ms | 1.004 |

−17.5% / −24.8% on the flagship renders, controls flat.  The mechanism: 8 px/step
instead of 2 (or 1, for the generic modes), zero shuffles (the AoS kernels' tbl.16b
splats vanish from the disassembly), and the formerly scalar generic/HSL modes --
`bench_render_large`'s scenes lean on MULTIPLY/SCREEN/LIGHTEN composites, which is
why it gains most.  No kernel regressed, so rule "AoS only with receipts" has no
exceptions to record.

**The arm_neon.h × -fbounds-safety seam, documented:** the intrinsics' pointer
parameters are unannotated, and a checked TU may pass them a checked pointer (the
bounds are simply dropped at the call).  The working pattern: wrap each ld4/st4 in
a `static inline` helper taking `__counted_by(8)` (or `(32)` for RGBA8), so the
implicit conversion at every call site IS the bounds check -- one branchy check
per 8-pixel block, the same shape and cost as the AoS kernels' checked-memcpy
idiom, verified in the disassembly (cmp/cmp/cmp + `ld4.8h` straight off the user
pointer; no copy through a local, no `__unsafe_*` anywhere).  The st4 intrinsics
are function-like macros, so a braced compound literal must be bound to a named
local first.
