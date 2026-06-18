> **Ruling (Mike, 2026-06-10): D1 — deleted without the oracle prerequisite; kept
> the clip fix's float32 attenuation, dropped its GPU-parity rounding; scrappy-phase
> trust in incremental testing.** The memo below recommended D2 (build the
> double-precision replacement oracle first). Between the memo and this ruling the
> clip fix (commit 7b20533) landed and re-converged the backends, so the "claim is
> false" deletion argument was repaired rather than standing. The ruling deletes
> Metal anyway: this stays a headless CPU library, the float32 clip attenuation is a
> correctness win worth keeping on its own merits, and the GPU-parity rounding
> (truncating half stores, the explicit FMA contraction shapes, the contract pragma)
> had nothing left to match. No replacement oracle was built first — the scrappy
> phase relies on the existing test/metamorphic/golden stack and incremental review.

---

# Memo: Should canvas2d delete the Metal backend (and the RTZ rounding rule with it)?

**Scope read:** `README.md`, `docs/backend-differential.md` (134 lines), `docs/bounds-safety.md` (613), `docs/roadmap.md`, `docs/decisions/codec-outsourcing.md`, `configure.py` (825), `.github/workflows/{gate,metal-probe}.yml`, `src/compositor.h` (81), `src/compositor_metal.m` (398), `src/compositor_cpu.c` (359), `shaders/compositor.metal` (169), `diff/diff_render.c` + `diff_compare.c` (277), `bench/bench_render{,_large}.c`, `examples/gallery.c` (clip scene), and commits `43e88c0`, `74658de`, `3ef78bf`, `1a548eb`, `9d4a925`, `5e40358`, `f8605a9`, `b106588`, `eb2135f`, `b1c7002`, `bf07e70`, `c64fb15`, `b3f4119`.
**Measured (this machine, Apple Silicon, Darwin 25.5):** full `ninja rendercmp` (hyperfine, 50/20 runs), `gputime`- and `throughput`-equivalent runs, a 10-run gallery A/B in scratch dirs, per-test-binary timing, and a forced tolerance-0 `backenddiff` re-run.

**Premise corrections.**

1. **"The CPU backend is bit-identical" is false today.** The committed `gallery/clip.png` is Metal's bytes (its hash matches a fresh `release/gallery` render exactly); `release-cpu/gallery` renders it with 20 of 144,000 channels off by exactly 1 (diff bbox x[45,270] y[20,99], reproducible). A forced `backenddiff` re-run still passes at tolerance 0 — its four scenes (`image`, `gradient`, `modes`, `clip`) all hold maxd=0. The gallery's clip scene draws opaque overdrawn stripes under partial clip coverage; the diff harness's clip scene draws translucent fills. The likely seam: `compositor_cpu.c` quantizes the clip-attenuated source to half (round-to-nearest) before blending, while `tile_blend_fs` hands `tile * clip` to the ROP in float32. So the doc's "one rule, not 26 hand-tuned cases" reduction is incomplete — there is at least a 27th rounding site, and it is live in a CI-byte-gated committed artifact. The bit-for-bit claim is gate-scoped, not global, and the README states it globally. (Filed as a separate task chip; it needs fixing whichever way this decision goes.)
2. **"Real canvas rendering is GPU-bound" — the docs' framing (README §Benchmarking, bounds-safety.md §What it costs) — is contradicted by the project's instrument.** `gputime` reports the GPU executing 1.38 ms across the small bench's 30 frames (~5% of its ~28–78 ms wall) and 7.0 ms across the large bench's 10 frames (~3%). This pipeline is CPU-bound (tile bake + premultiply) with GPU sync/driver overhead added. That sentence should be corrected regardless of the verdict.

## 1. Case for deletion

**a. On the flagship workload, Metal is a 2.5× slowdown.** This is a headless library whose only output is PNGs; the gallery is its largest workload, and `getImageData`/PNG-export is a readback-per-frame shape.

| workload (release builds) | metal | cpu | winner |
|---|---|---|---|
| **gallery, all 32 scenes end-to-end + PNG encode** | 427.0 ± 8.1 ms | 167.8 ± 1.4 ms | **cpu 2.54×** |
| small 256², 30 frames, per-frame readback | 78.0 ± 14.8 ms | 28.4 ± 0.4 ms | cpu 2.75× |
| small, one readback at end | 71.0 ± 10.4 ms | 25.5 ± 0.5 ms | cpu 2.79× |
| large 1024², 10 frames, per-frame readback | 264.7 ± 5.1 ms | 289.1 ± 2.5 ms | metal 1.09× |
| large, one readback at end | 251.1 ± 9.7 ms | 273.5 ± 1.7 ms | metal 1.09× |
| steady-state throughput, small | 69.1 Mpx/s | 79.2 Mpx/s | cpu 1.15× |
| steady-state throughput, large | 48.4 Mpx/s | 37.8 Mpx/s | metal 1.28× |

The gallery's metal split: 169 ms user vs 200 ms system (driver/IPC), against cpu's 157/7. Metal's best case — `bench_render_large`, a scene constructed to favor it (full-canvas fills, millions of px per draw) — is 1.09× wall, 1.28× steady-state, and commit `b1c7002` notes the ceiling is structural: the tile bake runs on the CPU for both backends, so the GPU only accelerates the blend. The 32-PNG gallery outputs were byte-identical across backends except the clip.png finding above.

**b. The RTZ rounding rule reverts, and the correctly-rounded backend is correct again.** `to_half_rtz`/`to_half_rtz4` revert to nearest-even; the `#pragma clang fp contract(off)` pinning the hot source-over loop (no FMA, to mirror the GPU) lifts; the coupling noted in backend-differential.md — "a green build depends on Apple GPU behaviour" — is removed. The purchased asset, bit-for-bit equivalence, is already false (correction 1); keeping the claim accurate means finding and matching the clip-attenuation rounding too and gating all 32 scenes — more coupling, not less.

**c. CI decouples from a paravirtual GPU.** `gate.yml` runs the full `ninja` on `macos-26`, which needs the VM's "Apple Paravirtual device" three ways: 112 of the suite's 224 test runs link Metal, `backenddiff` diffs against it, and the gallery byte-diff re-renders through it. An OS/runner-image update that changes the virtual device's store rounding breaks main with zero code change — the coupling named in the doc, mitigated today only by `metal-probe.yml` having passed once. There is also a measured cost: each metal-linked test binary pays ~45–50 ms of device-init + runtime shader compile vs ~2 ms for its cpu twin (`test_compositor`: 46.5 vs 2.1 ms).

**d. The deleted surface is wide for a single-purpose shim.** `compositor_metal.m` (398, the only ObjC in the tree), `compositor.metal` (169, including a 130-line framebuffer-fetch blend shader whose `[[color(0)]]` is Apple-GPU-only — this backend ran only on Apple Silicon), `diff/` (277), `metal-probe.yml` (33), `bench_gpu.h` (21), the `gpu_timing` ABI member and its plumbing through `compositor.h`/`canvas.{h,c}`/the cpu stub (~25), the RTZ machinery (~35), and ~100 lines of `configure.py` wiring (`BACKENDS`, the `objc_` rules, `--embed-dir`, `PIPELINE_BENCHES`, `rendercmp`/`gputime`, the dual diffdump edges). Roughly 1,050 lines, plus structure: variants collapse 5 → 3, the checked test matrix halves 224 → 112 runs.

**e. Neither stated purpose needs it.** Purpose 1 is learning `-fbounds-safety`; the flag rejects the `.m` file, so Metal is outside the curriculum — the README states "Metal is just a tile compositor." Purpose 2 is "C plays with Rust," and the Rust comparator (tiny-skia) is CPU-only; "antialiased and composited in checked C" is the matched comparison. The boundary lessons the Metal TU produced — plain-pointer ABI sharing, the can't-check-ObjC finding, the `objc_msgSend` spike, the RTZ investigation — are recorded in two docs that survive deletion. By the codec memo's rule ("shims for capabilities, in-house for algorithms"), Metal is a legitimately shaped boundary. On the flagship workload the CPU path beats it 2.5×.

## 2. Case against deletion

**a. The two-backend differential is the project's broadest oracle, and it is what would be deleted.** Two independent execution substrates (Apple's shader compiler + GPU vs clang + CPU) agreeing exactly across all 26 composite modes is regression coverage nothing else in the tree matches. The tolerance-0 gate is what let the Metal async-batching work (`f8605a9`, `eb2135f`, `b106588`) land safely, and the differential produced the project's RTZ-store finding. After deletion, what catches a blend-kernel bug? `test_composite` checks ten of 26 modes against hand-derived constants (correlated with the implementation's own formulas), `test_metamorphic` covers properties, and the committed gallery PNGs catch changes but not wrong-since-birth errors — and CI's byte-gate covers only 10 text-free scenes, which include neither modes grid. The counterfactual has a coverage hole unless a replacement is built first. (Mitigation exists — see H2 — but it is work.)

**b. The crossover is real and grows with canvas size.** 1.28× steady-state at 1024², and the trend is monotonic in canvas size; a 4K-canvas workload would widen it. Deleting Metal forecloses that. (Counterweight: the same measurements show the win is capped by the CPU-side tile bake, and uncapping it — moving gradient/coverage/bake to the GPU — is the move the roadmap rejects. The in-paradigm path to large-canvas speed is vectorizing the checked blend further, which is purpose-1 material.)

**c. Demonstration value.** The README's first sentence, the architecture diagram's "unsafe boundary #1," the gallery's "composited on the GPU" framing, and the batching scene all reference Metal. A CI-tested demonstration that a fully-checked C core drives a system framework across a plain-ABI seam is a stronger artifact than a doc describing it in past tense. Deletion turns one of the project's two boundary case studies into history. (Counterweight: the Core Text shim keeps the genre present — header adoption, callback lattices, sanitizer-visible C — and `docs/decisions/` holds completed experiments.)

**d. Deletion has transition costs.** All 32 committed PNGs flip from Metal bytes to nearest-even CPU bytes in one commit (review must trust the diff); README/docs need a rewrite, not a strikethrough; `rendercmp`/`gputime` are removed and `throughput` shrinks to one column. A day of surgery, and the clip.png inconsistency must be resolved first so the flip lands on a understood baseline.

## 3. Options, worked through

| Option | Perf | Correctness | CI | Oracle | Verdict |
|---|---|---|---|---|---|
| **K0: status quo** | flagship workload 2.5× slower than necessary by default build choice | software backend rounds to match the GPU; bit-for-bit claim currently false (clip.png) | coupled to paravirtual GPU semantics | broad but leaky (4 scenes) | Untenable as documented; the claim needs repair even to stand still |
| **K1: keep Metal, drop RTZ, gate at tolerance 1** | unchanged | CPU backend correctly rounded again; FMA unpinned; clip seam ceases to matter | still GPU-coupled | weakened (±1 hides one-ULP regressions) | The keep option that concedes the doc's headline and keeps the cross-check. Applicable if the demonstration value is judged load-bearing |
| **K2: keep Metal opt-in, off CI, CPU default** | cpu default everywhere | RTZ moot | decoupled | none — an untested backend | Rot with extra steps. `metal-probe` history shows GPU CI was hard-won; abandoning it abandons the backend incrementally |
| **D1: delete Metal, no replacement** | gallery 2.5× faster; `ninja` matrix halves | nearest-even restored | decoupled | coverage regression (see 2a) | Leaves the coverage hole; rejected |
| **D2: delete Metal + build the replacement oracle first** | as D1 | as D1 | as D1 | a double-precision reference blend in `test_composite` sweeping all 26 modes × an alpha/coverage grid, plus a text-free modes-grid scene added to `gate.yml`'s byte list (or `diff_render`'s scenes kept as committed goldens) | **Recommended.** The differential's `double` experiment showed the GPU was the deviant; a true-value reference tests against the correct answer rather than the GPU |

## 4. Recommendation

**Delete the Metal backend — option D2 — in this order:**

1. **Resolve the clip.png inconsistency first** (the spawned chip), so the equivalence claim is either true or re-scoped before the baseline flips.
2. **Build the replacement oracle before deleting anything**: a double-precision reference blend over all 26 modes × an alpha/coverage sweep in `test_composite`, and a text-free 26-mode grid scene added to the gallery + `gate.yml` byte list. This converts the differential's coverage into a form that tests against the correct answer rather than the GPU's.
3. **Delete**: `compositor_metal.m`, the shader, `diff/`, `metal-probe.yml`, `bench_gpu.h`, the `gpu_timing` ABI, the `objc` rules/`#embed`/`BACKENDS`/`-cpu` variant machinery; revert `to_half_rtz` to a plain nearest-even cast and drop `contract(off)`; collapse variants to `release`/`debug`/`unsafe`; re-render and commit the 32 PNGs as CPU bytes in one isolated commit.
4. **Move `docs/backend-differential.md` to `docs/decisions/`** as the record of a completed experiment — it ends on the RTZ discovery, a finished result, not abandoned work.
5. **Fix the "real canvas rendering is GPU-bound" sentence** in README.md and bounds-safety.md regardless of all the above; `gputime` refutes it today.

The tradeoffs that drove it, ranked: **(1)** this is a headless PNG library and the CPU backend wins its flagship workload by 2.5×, with the GPU executing 3–5% of wall time when present and a structurally capped best case of ~1.1–1.3× on favorable scenes; **(2)** the rounding rule's costs are live — a deliberately downward-biased backend, a pinned optimizer, a global claim that measurement shows is false — while its purchased asset decays; **(3)** CI's green depends on virtual-GPU semantics the project doesn't control; **(4)** both stated purposes survive, the boundary curriculum keeps its Core Text exemplar, and the recorded lessons remain.

Conceded to the keep side: the 26-mode cross-substrate agreement is the broadest regression oracle the project has had, and its replacement must be built and proven; the 1.28× large-canvas win is left on the table; the README's first sentence narrows; and a living GPU boundary is not easily rebuilt once its CI plumbing is gone.

## 5. What would change my mind

- **A display target appears** (swapchain / CAMetalLayer / interactive demo) — composition becomes the product and the per-frame-readback measurements stop being the relevant ones.
- **The workload moves to large canvases with end-of-batch readback** at sizes where the steady-state crossover (1.28× at 1024², growing with area) compounds into felt time.
- **The replacement oracle disappoints** — if the double-precision reference + metamorphic + golden-scene stack cannot reproduce the differential's bug-catching record (e.g., it would not have caught the clip-seam divergence class), the two-backend rig re-earns its place.
- **Someone closes the seam completely** — root-causes the clip-attenuation rounding, matches it, and gates all 32 scenes CPU-vs-Metal at tolerance 0. A bit-for-bit claim that is true and cheaply held removes the strongest deletion argument (the claim is false).
- **The probe agenda turns GPU-ward** — if the next field reports need living Metal code (checked bindings over `objc_msgSend`, GPU timestamp work, a `-fbounds-safety`-adjacent FFI study), deleting the bench before the experiment removes the platform for it.
- **Reader evidence** — if the Metal boundary turns out to be the most-cited section of the write-ups, the demonstration value is higher than estimated here, and K1 (keep, drop RTZ, tolerance 1) becomes the better landing.