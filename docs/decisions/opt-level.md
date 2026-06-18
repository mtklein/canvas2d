# Memo: Should canvas2d compile at -Os, -O1, or -O2?

**Scope read:** `configure.py` (the VARIANTS table), `README.md` §Benchmarking, `bench/` (all 14
benches + `bench_reps.h`), `docs/decisions/metal-backend.md` and `codec-outsourcing.md` (genre +
prior numbers).
**Measured (this machine, Apple M4 Max, Darwin 25.5, Apple clang 21.0.0, hyperfine 1.20):** all 14
benches × 6 configurations — {-Os, -O1, -O2} × {with `-fbounds-safety` ("checked", the release
posture), without ("unsafe")} — `hyperfine -N --warmup 3 --min-runs 12` (fast benches got 100–300
runs from hyperfine's 3 s floor), every binary built first and **copied out of the tree**, all
timing runs on a quiet machine with no compiler running. `__TEXT` section sizes via `size -m`, and
one cold `ninja release unsafe` wall-clock per config. A sentinel (`-Os` checked `bench_fill`)
re-run at the end of the sweep came back −0.1 % off its original median — no thermal or
ordering drift worth correcting for.

**The premise being tested.** The repo has compiled `-Os` everywhere since birth and the
alternatives had not been measured. `-Os` is `-O2`-with-size-bias, so the question is whether
speed is being left on the table for a size win — and since every
README overhead number is measured at `-Os`, whether the thesis numbers would look different at `-O2`.
Only the `-Os`/`-O1`/`-O2` token was varied; `-g` and everything else identical. (`-O3`, `-Oz`,
LTO, and PGO were not measured — see §6.)

## 1. Wall-clock, all six configurations

Median ± σ in ms. **Bold** = the fastest config in that row's safety posture.

| bench | -Os checked | -O1 checked | -O2 checked | -Os unsafe | -O1 unsafe | -O2 unsafe |
|---|---|---|---|---|---|---|
| `bench_blit` | **8.0 ± 0.3** | 8.3 ± 0.3 | 8.2 ± 0.2 | **8.0 ± 0.3** | 8.1 ± 0.3 | 8.2 ± 0.2 |
| `bench_blur_h` | **32.3 ± 0.3** | 32.8 ± 0.4 | 32.8 ± 0.3 | **29.0 ± 0.5** | 29.1 ± 0.5 | **29.0 ± 0.2** |
| `bench_blur_v` | 14.1 ± 0.2 | 14.1 ± 0.2 | **13.8 ± 0.3** | 13.0 ± 0.3 | 12.6 ± 0.2 | **12.5 ± 0.3** |
| `bench_fill` | 29.3 ± 0.3 | 28.3 ± 0.6 | **28.0 ± 0.6** | 27.0 ± 0.3 | **25.7 ± 0.4** | 25.8 ± 0.3 |
| `bench_flatten` | 117.1 ± 0.2 | **110.3 ± 0.3** | 110.4 ± 3.0 | 116.3 ± 0.5 | **103.1 ± 0.2** | 103.4 ± 0.6 |
| `bench_gradient` | **75.1 ± 1.1** | 84.2 ± 0.9 | 81.7 ± 0.8 | **74.7 ± 0.7** | 75.9 ± 0.8 | 82.0 ± 0.8 |
| `bench_gradient_fill` | **13.1 ± 0.2** | 13.8 ± 0.3 | 13.2 ± 0.2 | **13.1 ± 0.2** | **13.1 ± 0.2** | **13.1 ± 0.2** |
| `bench_png` | 15.0 ± 0.3 | 22.2 ± 0.3 | **14.5 ± 0.3** | 10.8 ± 0.2 | 15.5 ± 0.2 | **9.2 ± 0.2** |
| `bench_pngdec` | 18.2 ± 0.3 | 18.6 ± 0.3 | **17.2 ± 0.6** | 17.0 ± 0.3 | 16.3 ± 0.4 | **16.1 ± 0.6** |
| `bench_pngenc` | 44.6 ± 0.7 | 50.9 ± 0.9 | **44.5 ± 0.3** | 34.6 ± 0.8 | 38.2 ± 1.1 | **32.5 ± 0.2** |
| **`bench_render`** | **26.2 ± 0.2** | 26.7 ± 0.3 | 27.2 ± 0.2 | 23.0 ± 0.3 | 22.5 ± 0.2 | **22.3 ± 0.2** |
| **`bench_render_large`** | **277.1 ± 1.3** | 282.7 ± 1.8 | 279.9 ± 0.8 | 241.4 ± 1.0 | **232.1 ± 0.7** | 232.5 ± 1.0 |
| `bench_stroke` | 51.1 ± 0.7 | 50.6 ± 0.3 | **46.0 ± 0.4** | 49.0 ± 0.3 | 48.5 ± 0.5 | **46.2 ± 0.5** |
| `bench` (e2e) | 46.2 ± 0.3 | 49.2 ± 0.3 | **44.9 ± 0.4** | 41.4 ± 0.3 | 43.0 ± 0.3 | **40.1 ± 0.3** |
| **geomean vs -Os, same posture** | 1.000 | 1.055 | **0.987** | 1.000 | 1.017 | **0.964** |

**The flagship is slower at higher opt.** On the gallery-shaped workloads — `bench_render` and
`bench_render_large`, the project's largest scenes — the checked build is fastest at `-Os`
(−2 % to −4 % vs `-O1`/`-O2`), even though the unsafe build gets faster at `-O1`/`-O2` (−3 % to
−4 %). The two postures move in opposite directions, which is the §2 finding.

**Where the ordering flips:**

- **`bench_png`.** `-O1` regresses the deflate matcher (+48 % checked,
  +44 % unsafe vs `-Os`), while `-O2` unsafe is the fastest cell in its row (9.2 ms,
  −15 % vs `-Os`). Checked changes little at `-O2` (14.5 vs 15.0) — so the checked/unsafe gap
  widens to 1.57× there (§2). `bench_pngenc` (real scene, same codec) is the same shape, milder.
- **`bench_stroke` is `-O2`'s win:** −10 % checked (51.1 → 46.0 ms), and the bounds
  overhead vanishes (1.04× → 1.00×). The stroker is the one hot kernel never hand-vectorized,
  i.e. the one place the autovectorizer still had headroom — at `-O2` it takes it,
  checks and all.
- **`bench_gradient` is slower at higher opt:** +9–12 % at `-O1`/`-O2` in both postures.
  (`bench_gradient_fill`, the path the renderer uses, is flat everywhere.)
- **`bench_flatten` is faster at `-O1`/`-O2`** (−6 % checked, −11 % unsafe) — but unequally, so its
  overhead grows 1.01× → 1.07×.
- `-O1` is strictly dominated: geomean-slower than both `-Os` and `-O2` in both postures, and
  never the fastest checked cell on any bench. There is no candidate role for it.

## 2. Does higher opt shrink the bounds-check cost, or amplify it?

Checked/unsafe ratio of medians, same opt level — the README table's metric, now by opt level:

| bench | -Os | -O1 | -O2 |
|---|---|---|---|
| `bench_blit` | 1.00 | 1.02 | 1.01 |
| `bench_gradient_fill` | 1.00 | 1.06 | 1.01 |
| `bench_gradient` | 1.00 | 1.11 | 1.00 |
| `bench_flatten` | 1.01 | 1.07 | 1.07 |
| `bench_stroke` | 1.04 | 1.05 | **1.00** |
| `bench_pngdec` | 1.07 | 1.14 | 1.07 |
| `bench_fill` | 1.08 | 1.10 | 1.08 |
| `bench_blur_v` | 1.09 | 1.12 | 1.10 |
| `bench_blur_h` | 1.11 | 1.13 | 1.13 |
| `bench` (e2e) | 1.12 | 1.14 | 1.12 |
| **`bench_render`** | 1.14 | 1.19 | **1.22** |
| **`bench_render_large`** | 1.15 | 1.22 | 1.20 |
| `bench_pngenc` | 1.29 | 1.33 | 1.37 |
| `bench_png` | 1.40 | 1.43 | **1.57** |
| **geomean** | **1.104** | 1.145 | 1.129 |

**Amplifies.** `-Os` is where the safety tax is lowest, on the geomean and on almost every row.
`-O2` never beats `-Os`'s overhead except where it erases it entirely (`bench_stroke`), and on
the rows that were already the worst — the deflate matcher and the flagship renders — it makes
the ratio worse: `bench_render` 1.14× → 1.22×, `bench_png` 1.40× → 1.57×. The unsafe build
banks `-O2`'s wins; the checked build keeps most of its old time.

The code-size data (§3) indicates the mechanism. At `-Os` and `-O1` the checked binary is ~10–12 %
bigger than unsafe (the checks). At `-O2` it inverts: checked `__text` is 5 % smaller
than unsafe (135 KB vs 143 KB), because `-O2` grows the unsafe build by +43 % over `-Os` (more
unrolling/vectorizing) but grows the checked build by only +23 % — the
check-dependent control flow pins loops the optimizer would otherwise unroll or vectorize wider.
Less transformation, less speedup: the checked build gets less `-O2` than the unsafe
build does. This is consistent with where the project's perf work landed: the hot
kernels (fill resolve/accumulate, blur, gradient solve, blit, match verify) are hand-vectorized
8-wide with one check per block, so the autovectorizer has little left to add at any opt level —
what `-O2` finds is mostly on the unchecked side, plus the one un-hand-touched
kernel (the stroker), where it helps both sides equally.

Takeaway: the published `-Os` overhead numbers are the favorable case — `-Os` is the shipping
flag. "Bounds checks cost ~10 %" would read "~13 %" at `-O2`, and the flagship line would read
1.22× rather than 1.14×. Hand-vectorization with block-level checks keeps the tax low and stable
across opt levels; code that leans on the autovectorizer instead feels `-fbounds-safety` more at
the opt level reached for to go fast.

### Tuned vs tuned: what does choosing bounds safety cost?

Everything above holds the flag fixed to isolate the checks — the comparison for "what do
the checks cost this code". The project's motivating question is one notch up: what does
choosing `-fbounds-safety` cost a project? A project that rejected the flag would tune its own
flag rather than stay at this baseline's `-Os`, and §1 says its tuned flag is
`-O2`. So the comparison is each posture at its own optimum: checked at `-Os` (its flagship/size
pick) vs unsafe at `-O2`. Same medians as §1, cross-divided:

| bench | checked `-Os` / unsafe `-O2` |
|---|---|
| `bench_gradient` | **0.92** |
| `bench_blit` | **0.98** |
| `bench_gradient_fill` | 1.00 |
| `bench_stroke` | 1.11 |
| `bench_blur_h` | 1.11 |
| `bench_blur_v` | 1.13 |
| `bench_pngdec` | 1.13 |
| `bench_flatten` | 1.13 |
| `bench_fill` | 1.14 |
| `bench` (e2e) | 1.15 |
| **`bench_render`** | 1.17 |
| **`bench_render_large`** | 1.19 |
| `bench_pngenc` | 1.37 |
| `bench_png` | 1.63 |
| **geomean** | **1.14** |

The choosing-cost is 1.14× geomean / ~1.18× flagship / 1.63× worst — against the matched-flag
1.10× / 1.14× / 1.40×. Letting the comparator tune per bench (rather than one global `-O2`)
moves the geomean only to ~1.15: `-O2` is already nearly its optimum everywhere. Three rows
favor the checked posture even against the tuned comparator: `gradient` (0.92) and `blit` (0.98),
where `-O2` hurts the unsafe build more than the checks hurt the checked one, and `gradient_fill`
at parity. The comparison is not one-dimensional: the tuned comparator's binary is 143 KB of text
to the checked build's 110 KB — it pays +30 % code for its speed, so the two postures sit at
different points of the speed/size frontier, not the same point minus a tax. Which framing to
quote depends on whether the question is about this codebase (matched flags) or about the
decision to adopt the flag at all (this table).

## 3. Code size

`__text` section (code, exact bytes via `size -m`) of the e2e `bench` binary — it links the whole
library, so it's the library-size proxy; the other 13 binaries track it within ~2 KB. The
`__TEXT` segment numbers (what plain `size` shows) round to 16 KB pages: 128 / 144 / 144 KB
checked, 112 / 128 / 160 KB unsafe.

| config | `__text` | vs -Os same posture | checked / unsafe |
|---|---|---|---|
| -Os checked | 110,028 B | — | 1.10× |
| -O1 checked | 137,316 B | +24.8 % | 1.12× |
| -O2 checked | 135,472 B | +23.1 % | **0.95×** |
| -Os unsafe | 100,312 B | — | |
| -O1 unsafe | 122,380 B | +22.0 % | |
| -O2 unsafe | 143,204 B | +42.8 % | |

`-Os` produces a quarter less code than either alternative. The `-O2` inversion (the
checked binary is the smaller one, the only config where that happens) is the §2 mechanism
made visible.

## 4. Build time

Cold `ninja release unsafe` (the two variants whose flags vary; 202 edges, 14-wide), single run,
**rough** — one sample each, ±0.1 s scheduler noise easily:

| config | wall | user |
|---|---|---|
| -Os | 1.07 s | 9.2 s |
| -O1 | 1.09 s | 9.4 s |
| -O2 | 1.38 s | 10.3 s |

Irrelevant to the decision at this codebase size; recorded for completeness.

## 5. Options, worked through

| Option | Flagship (checked) | Geomean (checked) | Overhead story (§2) | Size | Verdict |
|---|---|---|---|---|---|
| **-Os (status quo)** | fastest (26.2 / 277.1 ms) | 1.000 | best: 1.10× geomean, 1.14× flagship | smallest (110 KB) | Wins three of four dimensions |
| **-O1** | −2 % | 1.055 | 1.145, worst | +25 % | Strictly dominated by both neighbors; never the fastest checked cell anywhere. No role |
| **-O2** | +2–4 % slower | **0.987** | 1.129; amplifies worst rows (png 1.57×, render 1.22×) | +23 % | A 1.3 % geomean win, driven by `bench_stroke` (−10 %), e2e `bench` (−3 %), `bench_pngdec` (−5 %) — but it loses the flagship and inflates the safety tax |

Not measured but adjacent: `-O3` (on this clang, `-O2` plus more vectorization/unrolling — the
trend in §2/§3 indicates it would push the same directions harder), `-Oz`, `-flto`, PGO. Also not
measured: whether `-O1`/`-O2` codegen changes rendered pixels (FP contraction differences are
plausible across opt levels). Since the 32 gallery PNGs and `.canvas` programs are committed
byte-gated build outputs, any flag change carries a possible one-time all-32-PNG churn commit
and a CI runner-image sensitivity — a switching cost on top of the numbers above, and unverified
here because the sweep never ran `ninja images` in a non-`-Os` config.

## 6. Recommendation

**Keep `-Os`. No flag change.** The ranked reasons:

1. **It wins the workload that matters.** The checked flagship renders — the project's product —
   are fastest at `-Os`. `-O2`'s geomean win (1.3 %) comes from rows the renderer doesn't pass
   through per frame (`stroke` partially excepted) and is bought with a 2–4 % flagship loss.
2. **It is where the safety tax is lowest and most stable.** 1.10× geomean at `-Os` vs
   1.13×/1.15×; the README's overhead table is measured there, and §2
   shows higher opt levels amplify the worst rows. The "checked C is cheap" claim is most
   accurate at `-Os`.
3. **Size is a free win.** A quarter less code for performance that is net better on the
   flagship. `-Os` is not a tradeoff being accepted here; it is the faster option on the flagship.
4. **Switching has a cost** — the possible committed-PNG/`.canvas` byte churn and a
   re-pinned CI baseline (§5), for a gain that does not survive reasons 1–2.

**The strongest argument against this recommendation:** `bench_stroke` at `-O2` is
−10 % with the bounds overhead erased — the only bench where `-O2` makes safety free — and the
e2e `bench`, `pngdec`, and `pngenc` rows agree that `-O2` ≥ `-Os` once work shifts codec- or
stroke-ward. If the workload mix drifts that way (stroke-heavy scenes, PNG-heavy pipelines), the
alternative is not a global flag flip but first giving the stroker the blit treatment by hand —
the §2 data says hand-vectorized kernels stop being sensitive to the flag, which would remove `-O2`'s
best argument while keeping `-Os`'s size and flagship wins. The one kernel `-O2` helps most
is the one kernel nobody hand-tuned, which indicates the per-kernel recipe, not the global flag, is
where the remaining gains are.

**Fork for Mike:** (a) ratify `-Os` status quo (this memo's pick); (b) take `-O2` for the ~1.3 %
geomean + stroke win, accepting the flagship regression, +23 % code, a higher overhead
table, and a possible gallery re-baseline; or (c) hold the flag and greenlight a stroker
vectorization pass instead, which the data indicates captures most of (b)'s upside inside (a)'s
posture.

### Addendum (2026-06-10): option (c) executed — the stroker got the blit treatment

Mike ratified (c) and the pass has landed: vertex emission stages triangles in local arrays and
lands whole blocks through one counted-pointer copy (the one-check-per-block idiom, applied to
geometry output), and segments and miter/bevel wedge joins run four per block on x/y planes —
each lane the scalar operation sequence exactly, so all 33 gallery PNGs stayed byte-identical
and no re-baseline was needed.  Paired hyperfine (before+after binaries in one
invocation, copied out of tree, no compiler running):

| `bench_stroke` | before | after |
|---|---|---|
| `-Os` checked | 49.7 ms | **26.1 ms** (1.90×) |
| `-Os` unsafe | 48.2 ms | 26.2 ms (1.85×) |
| checked / unsafe | 1.03× | **1.00×** |

The hand version beats `-O2`'s autovectorized 46.0 ms by ~43 % while staying at `-Os`, and
erases the same overhead `-O2` erased.  §7's second trigger resolves: the new
stroker rebuilt at `-O2` measures 25.6 vs 25.8 ms — a wash (1.01 ± 0.02) — so "`-O2` only helps
un-tuned kernels" stands, no re-sweep needed, and §6's strongest counter-argument is removed.
Spillover: e2e `bench` −5 % checked (its scene strokes every frame; its checked/unsafe ratio
rose 1.08× → 1.11× for the README's Amdahl reason — the stroke share both postures split
evenly shrank, leaving the codec-heavy remainder a larger slice); flagship renders and the
fill/flatten controls unmoved.

Two `-fbounds-safety` codegen lessons from the work, for the next kernel: a variable-index
vector lane read (`v[i]`) round-trips the whole register through the stack at every access —
spill lanes once per block into small arrays, or transpose results to memory order
in-register with constant-index shuffles; and a copy loop that writes through a struct's
`__counted_by` field reloads data/len/cap and re-derives its check every iteration (the stores
may alias the struct) — convert to a counted local once, then loop on that.

## 7. What would change my mind

- **The flagship moves.** If gallery-shaped scenes stop being the product (e.g. a stroke- or
  codec-dominated workload becomes the deliverable), §1's flagship rows lose their veto and the
  geomean — which `-O2` wins — becomes the right scorecard.
- **The stroker gets hand-vectorized and `-O2` still wins it.** That would break the "O2 only
  helps un-tuned kernels" story and force a re-sweep.
- **A clang update changes the codegen.** These numbers are one compiler (Apple clang 21.0.0) on
  one microarchitecture (M4 Max); the `bench_png` `-O1` cliff in particular is likely a
  version-specific inlining/register-pressure artifact. Re-run the sweep (it's ~15 minutes) on
  any toolchain bump that touches the README's numbers anyway.
- **Gallery bytes prove opt-level-stable.** If `ninja images` at `-O2` is verified byte-identical
  to the committed PNGs, the switching-cost argument (§5) drops out and option (b) gets cheaper.
- **`-O3`/LTO/PGO measurements surprise.** This memo sampled three points on the curve; a
  PGO build could recover the checked build's `-O2` losses by laying out
  the check-failure cold paths better. Not yet measured.
