# Memo: Is `_Float16` premultiplied RGBA the right universal internal color type?

**Scope read:** `README.md`, `src/canvas2d_math.{h,c}`, `src/compositor.h`, `src/compositor_cpu.c` (360 lines), `src/compositor_metal.m` + `shaders/compositor.metal`, `src/canvas2d_filter.c`, `src/canvas.c` (paint_tile / paint_tile_pattern / shadow / drawImage / read_unpremul paths), `src/blur.h`, `src/canvas2d_gradient.h`, `src/canvas2d_text.{h,c}` (emoji captures + mips), `docs/backend-differential.md`, `docs/pixel-pipelines.md`, `docs/decisions/codec-outsourcing.md`, `tests/test_compositor.c`, `tests/test_image.c` — plus four small numerical experiments (a standalone C program modeling exactly the codebase's store-rounding rules: f32 compute, per-store narrowing under f16-RNE, f16-RTZ, and u8-RNE).

**Premise corrections.**

1. **The "lingua franca" claim overstates what f16 does.** Across every kernel, f16 is never computed in. The compositor widens to f32 before blending (`compositor_cpu.c:142-144`), the filters widen to f32 (`canvas2d_filter.c:155`), the blur accumulates in f32 (`blur.h:31`), premultiply runs in f32 (`canvas2d_math.c:73-84`), the Metal shader reads the RGBA16Float texture as `float4`. Counting sites (excluding pixvm/png): ~33 f32→f16 narrows, ~28 f16→f32 widens, ~19 u8↔float conversions. The bandwidth-heavy *image* data — emoji captures and their mip pyramids (`canvas2d_text.c:594`, u8 premul with an integer halving that preserves the premul invariant exactly), `drawImage` sources, patterns, and the coverage mask itself — is already u8. The accurate statement is: **f32 is the compute language, u8 is the image language, and f16 is the storage format of exactly three buffers** — the per-op tile, the compositor target, and the 1024-entry gradient ramp. The decision under review is narrower than its slogan, which is both a defense (less is riding on it) and a finding (the README's rationale doesn't describe the code).
2. **"8-bit only at the spec-mandated edges" is aging out.** The HTML spec has since grown a `float16` canvas color type and `rgba-float16` ImageData (Chrome shipped it in 2025), and `canvas2d_gradient.h:74` already anticipates ">8-bit output." The spec's edges are becoming f16-shaped. (Verify the current standard text before leaning on this; it postdates parts of the source material.)
3. **The "concurrent Metal-deletion memo" does not exist in-tree.** `docs/decisions/` holds only `codec-outsourcing.md`, on every branch and worktree. It is treated as a concurrent deliberation and the conditional is answered in §3.

---

## 1. Precision: where does the 11-bit mantissa actually bite?

The experiment models the codebase's exact store semantics (f32 math, narrow at each store; RTZ per `to_half_rtz`).

**Round-trip through the 8-bit edge (the ≤1/255 claim, tested exhaustively).** Every (u8 color × u8 alpha) pair, 65,280 of them, pushed through premultiply → store → unpremultiply → 8-bit quantize:

| store type | mismatched pairs | worst error |
|---|---|---|
| f16, round-to-nearest | **0 (0.000%)** | 0 |
| f16, RTZ (the Metal-matched store) | **0 (0.000%)** | 0 |
| u8 premul | **32,385 (49.61%)** | **127/255** |

This is the decisive row for f16-vs-u8. A u8 premul target corrupts `getImageData` after *any* translucent draw — half of all color/alpha pairs, worst case losing 7 of 8 color bits at low alpha. The web spec *permits* this loss (browsers do exactly it), so u8 is conformant — but `tests/test_image.c`'s get/put round-trip, the gallery's `imagedata`/`subrect` scenes (scratch-canvas atlas read back and re-drawn), and the "≤1/255" claims all depend on better. f16 is the narrowest type for which the spec's 8-bit edge round-trips losslessly. That, not "GPU-native," is its load-bearing defense.

**The constructible case where f16 hurts vs f32.** Iterated low-alpha source-over (the canvas fade/trail idiom: translucent `fillRect` every frame). White at alpha α over black, exact limit 255:

| α / iterations | f32 | f16 RNE | f16 RTZ | u8 premul |
|---|---|---|---|---|
| 0.05 / 2,000 | 255 | 254 | 253 | 246 |
| 0.02 / 5,000 | 255 | 252 | 249 | 231 |
| 0.01 / 10,000 | 255 | 249 | **243** | 206 |
| 0.004 / 20,000 | 255 | 240 | **224** | **131** |

f16 stalls short of the limit (12/255 short at α=0.01 — a constructible, idiom-relevant artifact f32 doesn't have). Three qualifications: (a) u8 is 4–8× worse — this is the browser "trails never fully fade" ghosting, and canvas2d's f16 is already better than the platform's actual behavior; (b) it takes thousands of accumulated blends at α≤0.05 to see; ordinary stacking is immune — 320 layered α=0.25 discs (the README batch scene) land within 1/255 of f32 in *every* format; (c) **half the f16 deficit is the RTZ hack, not f16** — RNE stalls at 249 where RTZ stalls at 243. The accuracy sacrifice `docs/backend-differential.md` documents has a quantifiable cost in the one regime where f16 precision is marginal.

**Filters are why u8 tiles are not invisible.** Tiles look write-once-read-once, but `canvas2d_filter_apply` runs *on the tile*, so tile quantization is input to arbitrary amplification. A dark 0→0.06 gradient under `brightness(10)`: f32 tile 154 distinct output levels, f16 tile 154, u8 tile **16** — hard banding. Low-alpha premul color in u8 has the same property (`contrast`, `saturate` on translucent paint). Any u8-tile design must route filtered ops through a wider scratch — re-adding the type it deleted.

**The gradient ramp.** `canvas2d_gradient.h` documents ≤0.4/255 max deviation from the exact ramp, "below the 8-bit step." f16 entries add ≤2⁻¹¹ ≈ 0.125/255 near 1.0 — inside the budget. u8 entries would add up to 0.5/255 and exceed the documented claim. Low-alpha stops also premultiply through the ramp; xp2 shows f16 holds them exactly where u8 wouldn't.

**Verdict on precision:** f16's cost is confined to pathological accumulation, and is partly self-inflicted (RTZ); u8's costs are everyday-observable (round-trip, filters, ramps, ghosting). f32's headroom is not needed by any committed test, gallery scene, or claim.

## 2. Memory, bandwidth, SIMD width

Per composited op on the CPU backend: tile write + tile read + target read/write = **32 B/px at f16, 16 at u8, 64 at f32**. A 1920×1080 target: 16.6 MB f16 / 8.3 u8 / 33 f32 (plus `read_unpremul`'s full-size temp per readback, `canvas.c:2429`).

But the SIMD-width argument in the README's orbit ("8-wide f16 = 128-bit NEON") is **unrealized in production code**: the compositor and filters process one pixel as a 4-lane vector widened to f32. Eight-wide f16 *arithmetic* exists only in pixvm — an exploration, not wired into the renderer. u8's "16-wide" is equally hypothetical.

And pixvm's own measurement (`docs/pixel-pipelines.md`, the "fp16 raised the cost" section) is adverse evidence: moving channels float→f16 made the *unchecked* kernel 27% faster but the **shipping checked build only 7% faster** (63.8→59.7 ms), because the bounds checks are integer work that f16 doesn't shrink — the check ratio rose 1.08×→1.40×. Under `-fbounds-safety`, the project's defining constraint, fp16's bandwidth win is mostly absorbed. Caveat in both directions: the production compositor amortizes checks differently than pixvm's register file, and **nobody has measured f32 tiles in the production pipeline**. That benchmark is one type-swap away and would convert this section from inference to data.

## 3. The Metal coupling — and the deletion conditional

What f16 buys on Metal today: `MTLPixelFormatRGBA16Float` is renderable, blendable, and uploaded verbatim (`compositor_metal.m:342` — 8 B/px, no conversion at the boundary). The framebuffer-fetch blend path reads it as `float4`; no filtering capability is needed.

What f16 *costs* on Metal is the backend-differential machinery: the GPU truncates RGBA16Float stores, C rounds to nearest, so 1% of pixels diverged by 1/255 — resolved by degrading the software backend to match (`to_half_rtz`, its vector twin, the `fp contract(off)` pragma, a doc). That cost is specific to f16 storage: with f32 storage, both sides store exactly what their correctly-rounded f32 `+,−,×` produce, so the linear modes would be bit-identical by construction, no hack (the divide/sqrt modes — dodge/burn/soft-light/HSL — would need re-verification; any residual divergence there has no one-rule fix, a risk f16's coarse grid absorbed). With u8 storage, the design would inherit RGBA8Unorm's store-rounding behavior — less documented than float truncation; the matching story could be worse than f16's.

**If Metal goes away:** (a) "native on this hardware" survives — `_Float16` is native ARMv8.2 on every Apple Silicon CPU; (b) `to_half_rtz`/`to_half_rtz4` and the accuracy bias become vestigial and should be deleted the same day (recovering 243→249 in the trail experiment — the gate that justified them composites against nothing); (c) the motivating rationale ("a direct match for Metal's half / RGBA16Float", `canvas2d_math.h:14`) is gone, and the choice reopens as f16-vs-f32 storage on cache-footprint grounds, where pixvm says the checked-build gap is single-digit percent. **The f16 rationale survives a Metal deletion, but only because §1's round-trip argument carries it — the reason written in the header and README would not.** The two memos should cross-reference: deleting Metal moves f16 from "obvious" to "defensible, pending one benchmark."

## 4. Conversion topology and the mixed-type case

Today's per-pixel journey: u8 coverage → f32 → fold paint (f32) → narrow f16 tile → widen f32 in compositor → blend → narrow f16 target (RTZ) → widen f32 at read → u8 out. Every stage boundary pays a conversion; f16 participates in none of the math. f32 tiles would *delete* ~61 narrow/widen sites outright; u8 tiles keep the same count (narrowing to u8 instead). The conversions are cheap NEON ops either way — this is a complexity cost, not milliseconds.

**Mixed per-stage types:** u8 premul tiles for unfiltered SRC_OVER ops (the majority) + f16 target. Ordinary ops are u8-invisible (xp4), tile traffic drops 32→20 B/px, and the target keeps round-trip fidelity. It dies on its own complexity: filters force a second, wider tile path (§1); `compositor_blend` forks into two ABIs; the Metal backend grows a second upload format and the shader a second read path; the `canvas2d_premul`/`canvas2d_unpremul` can't-mix-them-up type discipline (`canvas2d_math.h:16-20`) has to be restated for a third type. Highest complexity of any option, for a bandwidth win no profile has demanded. Reject unless tile traffic ever shows up dominating `ninja profile-scene`.

## 5. Reversal cost

The type touches **23 files, ~129 references** — but the u8 edges insulate almost everything: of ~58 test files, only `test_compositor.c`, `test_emoji.c`, `test_gradient_solve.c` touch `canvas2d_premul`/`_Float16` directly; the rest assert through u8 APIs and survive a storage swap untouched. A move to f32 is mechanical: retype the two structs, delete the RTZ machinery and most of `backend-differential.md`, re-run the differential, let all 32 gallery PNGs re-render in lockstep (the workflow supports that), refresh the bench tables. A few days, not a quarter — the encapsulation cuts both ways: keeping f16 is cheap to reverse later, so deferring carries little risk. A move to u8 is the expensive direction: every consequence in §1 becomes a test-expectation rewrite, a retracted ramp claim, a filter-scratch redesign, and a fresh GPU-matching investigation. Mixed types cost the most and are the only hard-to-reverse option (two ABIs spread through the code).

## 6. Recommendation

**Keep `_Float16` premultiplied storage — but rewrite its justification, and run the missing measurement.**

1. **The type is right; the stated reason is wrong.** "Colour's lingua franca, native on this hardware" describes neither the code (f32 computes, u8 images, f16 stores three buffers) nor the load-bearing argument. The accurate rationale is: *f16 is the narrowest storage type for which the spec's 8-bit edges round-trip exactly* (65,280/65,280 pairs, even under RTZ, vs u8's 49.6% corruption), *at half the footprint of the f32 that would also achieve it* — and it is where the spec is heading. Fix `canvas2d_math.h:14`'s comment and the README line; an axiom whose written justification is the Metal texture format is the blind spot the owner flagged, because that justification is currently up for deletion in a sibling memo.
2. **u8 universal storage is the one alternative that is refuted**, not just disfavored: round-trip corruption, filter banding, ramp-budget blowout, 4–8× worse iterative ghosting — each observable through committed tests or gallery scenes.
3. **f32 is not refuted, only unmeasured.** Its constructible win (the trail stall) is pathological; its cost is 2× memory/bandwidth and an unverified GPU-matching story for the divide modes. Conceded: f16+RTZ leaves a 12/255-short artifact in a real web idiom that f32 wouldn't, and the project has never measured what f32 tiles cost end-to-end.
4. **Two cheap actions now:** (a) run the f32-tile production benchmark (`ninja benchcmp`/`throughput` after a mechanical retype on a branch) so this decision rests on a number, not the pixvm proxy; (b) record in the Metal-deletion memo that if Metal goes, `to_half_rtz`/`to_half_rtz4` and the RTZ bias go with it immediately — that accuracy debt depends on the Metal backend.

## 7. What would change my mind

- **The f32 production benchmark comes back ≤~5% slower end-to-end** (plausible under the checked build, per pixvm's 7%): switch to f32, delete the RTZ hack, retire `backend-differential.md`'s accuracy-bias section, gain the trail-idiom headroom — the round-trip argument holds at any width ≥ f16, so f16's only edge over f32 is the footprint the benchmark would have just priced.
- **Metal is deleted *and* that benchmark is close**: same switch, stronger — both of f16's original motivations (GPU format, SIMD width) would then be dead letters.
- **`ninja profile-scene` ever shows tile/target memory traffic dominating real scenes**: the u8-fast-path mixed design re-enters on evidence.
- **colorSpace / HDR / `float16` ImageData lands on the roadmap**: f16 hardens from "recommended" to "mandated by the edge," and this memo's hedged spec claim should be re-verified first.
- **The backenddiff gate moves off tolerance-0**: the RTZ hack becomes deletable independent of everything else, removing half the constructible f16 artifact for free.
- **8-wide f16 arithmetic actually lands in the production compositor** (pixvm design C grafted onto `compositor_blend`): f16 would then take the compute role the README already credits it with, and the "lingua franca" line would become accurate instead of needing rewording.

---

## Addendum (2026-06-10): the compute half of this memo is superseded

The last trigger fired.  [color-axis.md](color-axis.md) measured the three-way fork
this memo left open (§6.4(a)'s "run the f32-tile production benchmark") and Mike
ratified its arm (c): **`_Float16` is now the compute type as well as the storage
type.**  The blend kernel (all 26 modes), the filter colour matrix, the gradient stop
lerp, premultiply/unpremultiply, and the readback quantize all do their arithmetic in
f16, 8 lanes per 128-bit NEON vector — measured 13–15 % faster than the f32-compute
pipeline on the flagship renders.  Two of this memo's framings age accordingly:

- Premise correction 1's "f32 is the compute language … f16 never computes" was true
  when written and is now false by design; the "lingua franca" line it corrected is
  accurate today.  The storage argument (§1's round-trip table) is untouched and
  remains the load-bearing rationale — and it survived compute narrowing exhaustively
  (all 65,280 pairs still round-trip under f16 arithmetic, now pinned in `test_image`).
- §1's trail-pathology table gains the f16-compute column in color-axis.md §5: RNE
  f16 *compute* lands at or above the f16-storage fixed point at every tested α, so
  the conceded artifact did not deepen.  The RTZ rows are historical — the RTZ hack
  died with the Metal backend.

The one place f32 arithmetic deliberately survives in the colour pipeline is the box
blur's running-sum accumulator (`canvas2d_blur.h` has the measured drift numbers).