# Rasterization survey

Status: **living document.** This surveys rasterization — algorithms, quality,
parallelism, vectorizability — whenever it is the slow pole. Unlike the decision
memos in [decisions/](decisions/), nothing here is settled; entries get re-priced
as the profile moves. Each option carries the experiment that would price it: a
paired bench, a byte-diff of the 33 gallery PNGs, and a memo.

Last grounded: 2026-06-10, one `ninja profile-scene` pass (release gallery,
`GALLERY_REPS=200`, 4 s `sample` window, Apple M4 Max) plus the measured numbers
in [decisions/opt-level.md](decisions/opt-level.md),
[decisions/color-axis.md](decisions/color-axis.md),
[decisions/gradient-eval.md](decisions/gradient-eval.md), and
[sparse-coverage.md](sparse-coverage.md). Re-grounded the same day after §3.1
(planarize the shade stage) landed; the pre-landing table this document was
written around is in git history.

Touched 2026-06-20 (SIMD pass): the `canvas2d_clip` bbox-limit landed and the
bilinear-sampler vectorization was tried and reverted (kill condition) — both in
§4. The §1 table predates them; the clip line is gone from the leaderboard and
the image-sampling bucket is unchanged (sampler reverted), so the §1 shares hold
otherwise. A fresh §1 re-grounding waits for the next structural change.

## 1. Where the milliseconds go

Top-of-stack self-time across the whole gallery (all 33 scenes, PNG encode
excluded by the script's design — codec work happens only on the final rep,
outside the sample window). The script prints the top 15 lines; everything below
`blend8`'s 27 samples is ≤ ~1 % each. 2,304 samples visible:

| function | samples | share | stage |
|---|---|---|---|
| `draw_image_quad` | 577 | 25.0 % | shade (per-lane sampling taps; the fold around them is planar) |
| `canvas2d_cover_add_edge` | 297 | 12.9 % | coverage accumulate (accum_row/deposit inlined) |
| `compositor_blend` | 276 | 12.0 % | composite |
| `paint_tile` | 253 | 11.0 % | shade (planar fold, solid + gradient) |
| `canvas2d_clip` | 245 | 10.6 % | clip-mask intersect (full-canvas scalar loop) |
| `canvas2d_cover_resolve` | 212 | 9.2 % | coverage resolve |
| `src_over8` | 90 | 3.9 % | composite |
| `canvas2d_mip_halve` | 72 | 3.1 % | image (emoji mips) |
| `???` (unsymbolized `-Os` outlined code) | 49 | 2.1 % | unattributed |
| `canvas2d_px8_store_k` | 49 | 2.1 % | planar tails (shade + composite) |
| `paint_tile_pattern` | 48 | 2.1 % | shade |
| `canvas2d_gradient_color_row` | 47 | 2.0 % | shade |
| `canvas2d_verts_tri` | 33 | 1.4 % | stroke geometry |
| `canvas2d_blur_box_h_f16` | 29 | 1.3 % | filters |
| `blend8` | 27 | 1.2 % | composite |

Bucketed by pipeline stage:

| stage | share of visible compute |
|---|---|
| **shade** (`draw_image_quad` + `paint_tile` + pattern + gradient row) | **40.1 %** |
| **coverage** (accumulate + resolve) | 22.1 % |
| **composite** (planar f16 blend) | 17.1 % |
| **clip** (mask build/intersect) | 10.6 % |
| image mips | 3.1 % |
| planar tails (`store_k`, shade + composite) | 2.1 % |
| unattributed outlined code | 2.1 % |
| stroke geometry | 1.4 % |
| filters/blur | 1.3 % |
| **flattening** | absent from the top 15: **< 1 %** |

The shade stage is the biggest bucket at 40 %, no longer scalar, and its
composition changed. The old pole — `paint_tile` + `canvas2d_premultiply` at 41 % —
collapsed to `paint_tile`'s 11 %: `canvas2d_premultiply` and `canvas2d_matrix_apply` left
the leaderboard entirely (their bulk callers now run the planar fold; in
absolute terms, with the flagship −29 %, paint_tile-shaped work is roughly 4×
cheaper). The new pole is `draw_image_quad`'s per-lane sampling interior at 25 %
— the data-dependent taps (four gathers per bilinear pixel, index clamp/wrap,
and the in-sample lerp arithmetic, all still scalar per lane inside
`sample_src`) — followed by coverage at 22 %, composite at 17 %, and
`canvas2d_clip`'s scalar intersect at 10.6 %. The distribution is flat: no single
fix buys 20 %.

Caveats on these numbers:

- `sample` gives proportions, not milliseconds. Absolute anchors come from
  hyperfine: `bench_render` 12.5 ms, `bench_render_large` 107 ms (1024², 10
  frames ≈ 10.7 ms/frame), e2e `bench` 45 ms and codec-bound (README table). The
  profile is the *gallery* mix; `bench_render_large`'s few-huge-fills shape
  weights the same stages differently (more composite, less per-op overhead).
- **The coverage histogram, re-measured (2026-06-10, seam-efficiency pass):**
  per-pixel coverage classes over every painted fill/stroke bbox.  The gallery
  mix is **12.2 % zero / 2.6 % partial / 85.1 % full** and `bench_render` is
  10.8 % zero — far fuller than the old survey's 39–59 %, which reproduces
  only on its original content classes (200 concave stars: 59.6/8.5/31.9;
  200 convex n-gons: 26.3/3.9/69.8).  The committed scenes are rect- and
  text-heavy: zero-coverage skipping has little to bite on in the flagship
  mixes, which is why the §3.8 block predicates priced ~flat there.
- Attribution is smeared by inlining at `-Os`: `accum_row`/`deposit` fold into
  `canvas2d_cover_add_edge`, the prefix sum and fill-rule fold into
  `canvas2d_cover_resolve`, and the planar shade helpers (`cover8`/`shade8`/
  `canvas2d_px8_premultiply`) fold into their callers — `paint_tile`'s 11 % is the
  whole planar fold, not a remnant of the old scalar loop. The scalar
  `canvas2d_premultiply` still exists for true single-pixel callers (shadow/filter
  tint setup) and is invisible here.
- `src/` spawns **zero threads**. Nothing in the tree calls dispatch/pthreads —
  the blur included (it is plain single-threaded loops). The `__workq_kernreturn`
  pile that `sample` filters out is system frameworks' parked workers. The whole
  pipeline is one core.

## 2. The architecture today

One pipeline, per op, over the op's device-space bounding box
([canvas2d.c](../src/canvas2d.c)):

1. **Flatten** at path-build time ([canvas2d_path.c](../src/canvas2d_path.c)): curves
   subdivide adaptively (`cross_chord2` flatness, depth-capped, NaN-as-flat) into
   device-space line segments. Cost is invisible in the profile (< 1 %).
2. **Accumulate** ([canvas2d_cover.c](../src/canvas2d_cover.c)): for each edge, walk its
   scanline rows; per row, deposit exact signed trapezoid areas into a dense
   per-bbox `float` buffer — two partial-column deposits at the span ends, and the
   interior **telescoped** into a contiguous constant-add done 8-wide with one
   whole-vector bounds check per block. The buffer is memset per op
   (`canvas2d_cover_reset`).
3. **Resolve**: per row, an 8-wide in-register Hillis-Steele prefix sum (+ scalar
   carry between blocks) turns area deltas into winding, folds by fill rule
   (nonzero = clamp |run|; evenodd = triangle wave), and quantizes once to a
   dense u8 coverage buffer.
4. **Shade** (`paint_tile` / `paint_tile_pattern` / `draw_image_quad`): eight
   pixels per step over channel planes (the §3.1 conversion). For folding
   composite modes — the over-family (§3.8's ruling) and any filtered op —
   coverage normalizes as
   one f16 multiply by RN16(1/255) and the fold (paint alpha × global alpha ×
   coverage) runs in f16, the pipeline's compute type, with one narrowing
   convert where the colour data is born f32 (the sampling taps) — the §3.1
   f16 arm, taken in the simplification sweep (task #30); for the lerp
   family (copy, the in/out family, dst-atop, lighter, the blends) the tile
   is the source at full
   strength (paint alpha × global alpha only) and the op's u8 coverage buffer
   rides to composite as its own plane. Either way: one narrowing convert to
   f16, then the planar premultiply (`canvas2d_px8_premultiply`) and an st4 into
   the premultiplied RGBA16F tile. One seam shortcut (the §3.8 efficiency
   pass, landed): a lerp-family **solid** paint writes no tile at all — the
   one premultiplied colour goes to `canvas2d_blend_solid` as a splat (so
   do clearRect/reset's unit-alpha erase and the full-strength shadow tint).
   Gradients get their parameters and
   colors solved 8-wide per row
   ([gradient-eval.md](decisions/gradient-eval.md)) and are picked back up
   as planes (ld4 over the row buffer). Pattern and image sampling stay scalar
   per lane — the taps are data-dependent gathers — inside the planar fold,
   with the device→source coordinate chain vectorized (elementwise, bit-exact
   per lane).
5. **Blend** ([canvas2d.c](../src/canvas2d.c)'s blend kernels +
   [canvas2d_planar.h](../src/canvas2d_planar.h)): 8 pixels per step as four f16 channel
   planes, ld4/st4 at the seams, all 26 modes straight-line vector code,
   compositing onto the canvas's own premultiplied RGBA16F target.  There is
   no compositor: the old object, its ABI, and its copies (the per-clip-change
   mask copy, the per-readback target copy) dissolved into canvas2d.c — the
   kernels read the canvas's clip mask directly (`canvas2d_blend.h` is the
   internal seam the oracle tests drive) and readback un-premultiplies
   straight off the target.  Effective coverage (op plane × clip mask)
   applies per the §3.8 ruling: folded into src for the over-family,
   `lerp(dst, blend, cov)` after the full-strength blend for everything
   else.

Strokes go through the same path: [canvas2d_stroke.c](../src/canvas2d_stroke.c) expands
the polyline to triangles, `stroke_device_path` feeds each triangle's edges with
consistent winding, and one nonzero resolve unions them. Clips rasterize coverage
the same way, then intersect into a full-canvas u8 mask with a scalar
`old * pc / 255` loop (the 7.3 % line above).

### Quality guarantees, precisely

- **Exact-area AA, not sampled.** Per pixel the accumulated value is the sum of
  exact signed trapezoid areas, in f32; the resolve's 8-lane tree association
  differs from a serial scan by ≤ 1 ULP; one quantize to 8 bits (≤ 1/510). There
  is no sample grid, so none of supersampling's banding — coverage is a smooth
  function of geometry. `test_cover` gates this at ±1–2/255.
- **Coincident interior edges cancel.** An edge traversed both ways deposits
  identical magnitudes with opposite signs into the same cells, so shared edges
  of the stroke triangulation (and of abutting subpaths within one fill) cancel
  to within the running cell sum's f32 rounding — sub-quantizer, no seams. This
  is the property that lets the stroker emit a sloppy overlapping triangle soup
  and still produce a clean union: the union is computed in winding space and
  folded once.
- **Conflation *within* a path exists, at the fold.** We compute
  `fold(∫ winding)` per pixel; the exact answer is `∫ fold(winding)`. They
  differ only where winding varies within a pixel beyond the fold's linear
  range: a self-overlapping nonzero path whose sub-regions each partially cover
  the same pixel over-covers (min(a+b, 1) ≥ area of union), and evenodd is
  worse — a pixel half at winding 0 and half at winding 2 resolves to coverage 1
  where the true evenodd coverage is 0. These are edge-crossing pixels of
  self-overlapping geometry only; no committed scene exercises it, and no test
  pins it. Skia's analytic AA has the same property; it's intrinsic to
  winding-space accumulation.
- **Conflation *across* ops is spec behavior.** Each op resolves and composites
  independently, so two fills sharing a geometric edge each AA against what's
  underneath: 50 % + 50 %·50 % leaves a 75 % seam over the background. HTML
  Canvas itself is defined op-at-a-time, browsers do the same, and the
  scene-replay model depends on it — listed here because it is depended on.
- **The f16 pipeline does not erode the coverage claims.** Coverage is f32 until
  the u8 quantize; downstream, every 8-bit edge value round-trips f16 exactly and
  blends are off-by-one on ~5 % of translucent triples, max 1/255
  ([color-axis.md](decisions/color-axis.md) experiments 1–2, now committed
  tests). The u8 coverage quantizer (1/510) is the coarsest step in the chain —
  coarser than f16's ~1/2048 near 1.0.

### Ceilings, with numbers

- **Dense touch of the whole bbox.** Per solid-fill bbox pixel, regardless of
  coverage: 4 B memset (`cover_reset`) + 4 B read + 1 B write (resolve) + 1 B
  read + 8 B write (shade) + 8+8+8 B (composite read tile, read/write target) ≈
  **42 B of traffic per bbox pixel**, before one pixel visibly changes.  The
  seam-efficiency pass (§3.8, landed) trimmed the worst of it: the tile's
  16 B/px round trip is gone outright for lerp-family solids (the splat), and
  zero-coverage blocks skip the composite's 16 B/px dst round trip.  The
  re-measured histogram (§1) says the committed mixes are 85 % full coverage,
  so the *zero-skip* part has little to bite on today; the old 39–59 % figure
  was the stars/convex survey content, not the gallery.
- **The shade stage's remaining scalar interior is the sampling taps.** The
  fold is planar everywhere (§3.1, landed); what stays per-lane is
  `sample_src`/`pattern_sample`'s data-dependent addressing — four taps per
  bilinear pixel at arbitrary source coords — *plus* the in-sample lerp
  arithmetic and index clamp/wrap math that ride along scalar because they live
  inside the per-lane sample functions. The taps themselves have no batch shape
  (no NEON gather), but everything around them is elementwise f32 and could
  vectorize bit-exactly; that's §1's 25 % line.
- **One core.** Everything above is serial. The op stream is inherently ordered
  (each op read-modify-writes the shared target), but within an op every stage
  is row-independent, and across ops disjoint bboxes commute byte-exactly.
- **Per-clip full-canvas work.** `canvas2d_clip` allocates and walks width×height
  scalar per `clip()` call, and the mask is copied by value into saved states.
- **The u8 round-trip between resolve and shade.** Resolve quantizes to u8;
  shade immediately reloads and divides by 255. Two passes over the bbox where
  one would do, plus a quantize that exists only because the seam is a byte
  buffer (the shadow and clip paths do want bytes; the paint path doesn't).

### Determinism, the two senses

The byte-gate (34 committed PNGs, replayed byte-for-byte across machines) imposes
two distinct constraints, worth keeping separate when pricing options:

1. **Run-to-run / machine-to-machine identity for fixed code.** Threatened only
   by nondeterministic scheduling. Any parallel decomposition that partitions
   *pixels* (rows, tiles) keeps each cell's float-add order identical to the
   serial code and is bit-exact by construction. Decompositions that partition
   *edges* and merge accumulators are also safe **iff** the merge order is fixed
   (per-thread partials reduced in thread-index order) — but cost extra buffers.
   Free-running atomics or work-stealing into a shared float buffer are
   forbidden, full stop.
2. **Code-change byte churn.** Any reordering of per-cell accumulation (a
   different traversal order, a tiled prefix sum with different carry
   association) moves sums by ≤ 1 ULP, which *occasionally* flips a quantized
   byte — a one-time re-baseline commit of moved PNGs, priced as routine by
   [decisions/gallery-committed-pngs.md](decisions/gallery-committed-pngs.md).
   Not a blocker; a known line item.

## 3. The option space

Each entry: the idea, what it buys, what it costs, how it composes with
`-fbounds-safety`, and the experiment that would price it.

### 3.1 Finish the planar conversion through the shade stage

**Idea.** `paint_tile`'s inner loop, 8 pixels per step: load 8 coverage bytes as
one f16 plane (`f16x8_from_u8` — exists), fold `paint_alpha × global_alpha ×
coverage` as plane math, `canvas2d_px8_premultiply` (exists), `canvas2d_px8_store`
(exists). For solid paint the whole loop is a color splat, ~4 multiplies, and an
st4 per 8 pixels. For gradients, `canvas2d_gradient_color_row` already produces a row
of colors — it needs a planar seam (ld4 over the `crow` buffer) instead of the
scalar pickup. `draw_image_quad` gets the same treatment on its premultiply/fold
half (the bilinear sample stays gather-shaped, but the fold around it
vectorizes). Step two, optional: fuse resolve→shade — have resolve hand the paint
path f16 coverage planes directly, skipping the u8 buffer for paints (keep the u8
emit for shadow/clip consumers), deleting 2 B/px of traffic and the /255 refold.

**Buys.** The top of the profile: 41 % (`paint_tile`+`premultiply`) addressed
directly, 57 % with `draw_image_quad`. Every prior planar conversion of a
per-pixel f16 stage returned 3–4× on the kernel and double-digit flagship wins
(color-axis layout addendum: −17.5 %/−24.8 %). If the shade stage gets the same
kernel-level factor, the gallery compute pie loses on the order of 20–30 %. It
is pure SIMD — no format change, no new invariants.

**Costs.** Modest code (the vocabulary exists); the usual risk that per-lane
arithmetic drifts from the scalar form. The discipline is established: bitwise
selects, compare+select min/max, per-lane arithmetic identical to scalar — the
26-mode kernel held all 33 PNGs byte-identical through the same conversion.
The fold itself is f32 today (`covf * ga * col.a` then premultiply); moving it to
f16 planes re-rounds — either keep the fold f32 in 8-wide form (widen the
coverage plane, two converts per block) or accept a possible ≤1/255 shift and a
re-baseline. Price both arms.

**-fbounds-safety.** Ideal shape: one `__counted_by(8)` seam check per block at
the coverage load and the st4 store, replacing 8 scalar indexed reads + 8 scalar
struct writes per 8 pixels. Checks get *cheaper*, the established pattern.

**Experiment.** Convert `paint_tile` solid+gradient paths; paired hyperfine on
`bench_render`/`bench_render_large`/`bench_fill`/`bench_gradient_fill` + controls;
`ninja images` byte-diff (target: zero bytes moved with the f32-fold arm); memo.
Then `draw_image_quad` as a follow-up with `bench_render` + the emoji/drawimage
scenes as the probe.

**Landed (2026-06-10), the f32-fold arm.** Five commits: `paint_tile`
(solid + gradient rows), `emit_shadow`'s tint loop, `paint_tile_pattern`,
`draw_image_quad`, then the sampling loops' coordinate chain (`canvas2d_matrix_apply`
had surfaced at 4.4 % as an out-of-line call per covered pixel — now eight
lanes of elementwise vector math). **Zero gallery bytes moved at any commit**:
the fold stayed f32 with the scalar association per lane, coverage widened
exactly, one narrow to f16, and `canvas2d_px8_premultiply` is lane-wise the scalar
`canvas2d_premultiply`. Measured (paired hyperfine, copies in /tmp, medians):
`bench_render` 17.7 → 12.5 ms (**−29.4 %**), `bench_render_large` 175.7 →
106.9 ms (**−39.2 %**) — beating the −15–25 % prediction — with e2e `bench`,
`bench_fill`, `bench_gradient_fill`, `bench_blit`, and `bench_blur_h` all flat
(`bench_fill` is coverage-bound stars, `bench_gradient_fill` is bound on the
row solve/stop search, both as the pie predicted). The `-fbounds-safety` tax on
`bench_render` fell from 3.0 ms to 1.9 ms checked-vs-unsafe (per-pixel checks
became per-block seam checks), holding at ~1.18× relative. What stayed scalar,
per site: the sampling taps in `draw_image_quad`/`paint_tile_pattern`
(data-dependent gathers — see §1's new pole), the gradient OOM fallback
(per-pixel solve, cold by construction), and the single-pixel
`canvas2d_premultiply` API for true single-pixel callers. Not taken: fusing the
gradient row buffer away (the st4/ld4 round trip over `crow` is noise next to
the stop search — fusion would buy a registers-only handoff at the cost of
restructuring the gradient module's API; re-look only if `paint_tile`'s
gradient share ever grows) and the resolve→shade f16-coverage fusion (step
two above — still open, re-price against the new pie).

**Postscript (2026-06-11, the simplification sweep):** the deferred f16-fold
arm landed once byte-stillness stopped being a constraint — the fold now runs
in f16 end to end (coverage × RN16(1/255), alpha factors folded in the colour
data's native type, one narrow at the f32 sampling taps).  24 scenes
re-baselined, every change AA-edge-only at max 1/255 (interiors are exact:
255 × RN16(1/255) == 1.0); `bench_render_large` −3 %, the rest flat.

### 3.2 Sparse / RLE coverage and strip formats

**Idea.** Represent resolved coverage per row as spans (`start, len, value` for
zero / fractional / saturated runs — Skia's `SkAlphaRuns`/RLE-mask lineage)
instead of a dense byte per pixel, and let shade and composite skip zero runs and
take a solid fast path on 255 runs (source-over with an opaque solid paint is a
store, no dst read).

**Buys.** Skips the transparent 39–59 % of every fill's bbox across
resolve→shade→blend, and collapses saturated interiors to one span. The win
*today* is inflated by the scalar shade stage (every skipped pixel is an
expensive pixel); **after §3.1 lands, the payoff shrinks** — this is the same
re-pricing [sparse-coverage.md](sparse-coverage.md) recorded when the dense path
vectorized the first time. Re-measure after, not before.

**Costs.** The prior run-aware-resolve experiment is the cautionary number:
+14 % on convex content, **−17 % on concave** (many short runs; per-run overhead
exceeds the skipped work). So RLE means a format *plus a chooser*, two code paths
for one semantics. It also doesn't compose with the resolve's whole-row 8-wide
prefix sum for free — runs are found *after* the winding is summed, so the
accumulate stays dense; only resolve's output and downstream change.

**-fbounds-safety.** Spans are friendly: a `__counted_by(n)` span array, and each
span's pixels are one counted slice — checks per span, not per pixel. Better
shape than the dense path's per-block checks, on paper.

**Experiment.** The memo's standing advice still holds: the highest-value,
lowest-friction slice is **the clip mask** (see §3.6), not fill coverage. For
fill coverage: re-run the coverage histogram on today's gallery (text-heavy
scenes shifted the mix since the 200-shape measurement), prototype span-emitting
resolve behind the chooser, paired bench on `bench_fill` (stars = concave kill
case, convex = win case) + flagship; byte-diff must be clean (spans are exact
representation, no arithmetic change); memo. **Kill condition:** chooser can't
hold concave within ~2 % of dense.

### 3.3 Active-edge-table / scanline-incremental rasterization

**Idea.** Replace per-edge full traversal + dense accumulate + full-bbox resolve
with the classic AET: edges sorted by y-min, advanced incrementally per
scanline, crossings sorted in x, winding accumulated left-to-right, spans
emitted directly. The AA variant is FreeType's smooth rasterizer / stb_truetype:
sparse *cells* only at edge-crossing pixels, winding carried between cells, so
the buffer touched is O(edge pixels), not O(bbox).

**Buys.** Deletes the memset and the dense resolve read — the parts of the 42
B/px floor that exist even where nothing happens. Output is naturally spans, so
it composes with §3.2 without a separate run-finding pass. Asymptotically right
for sparse content (thin strokes, text, spiky paths) where the bbox is mostly
empty.

**Costs.** Branchy, sort-heavy, scalar-shaped — the opposite grain from the
house style. Our accumulate is already 8-wide on span interiors and the dense
resolve blows through zeros at 8/cycle-ish; the old sparse memo's lesson was
that vectorized-dense moved the crossover far toward sparse content. Per-cell
accumulation order changes (x-sorted instead of edge-input order), so sums move
≤1 ULP → likely a re-baseline (determinism sense 2, not sense 1). Two
rasterizers in the tree unless it wholesale replaces — and it can't, without
losing the telescoped dense path's wins on filled content.

**-fbounds-safety.** Cell pools as index-linked counted arrays work fine
(`__counted_by` pool + int next-indices, the tagptr.md genre); the cost is a
bounds check per pointer-chase, on a path that is already latency-bound chasing.
The least flattering shape for the flag in this codebase.

**Experiment.** A standalone `canvas2d_cover_aet` with the same
add-edges/resolve-to-u8 contract; `bench_fill` (stars/convex) + a text-heavy
scene bench; compare coverage output to the dense rasterizer at ±1 tolerance and
the gallery at byte level (expect churn; count it); memo. **Kill condition:**
loses on filled convex content by more than it wins on stars — that's the old
result, and it would have to beat it to justify a second rasterizer.

### 3.4 Tile binning + per-tile rasterization

**Idea.** The Pathfinder/Vello shape, on CPU: bin flattened segments into N×N
device tiles (16 or 32); compute each tile's *backdrop* winding (the prefix
contribution of everything left of it); classify tiles **empty / solid /
partial**; partial tiles run the existing accumulate+resolve on an L1-resident
N×N f32 micro-buffer; solid tiles skip coverage entirely and take the opaque
fast path through shade+blend; empty tiles cost one classification check.

**Buys.** The sparse win at tile granularity *without* the per-run chooser
problem — classification is one branch per tile, and inside a partial tile the
dense 8-wide machinery runs unchanged. Cache locality (the micro-buffer never
leaves L1; today a 1024-wide fill's accumulate walks a 4 KB row stride). And it
is **the parallelism unit**: tiles partition pixels, so per-tile work is
embarrassingly parallel and bit-exact under determinism sense 1 — this is the
enabler §3.5 wants.

**Costs.** A binning pass (O(segments × tiles crossed) — Vello pays this on
thousands of GPU threads; serially it's new work the current pipeline simply
doesn't do). Real complexity: backdrop bookkeeping, tile lists, a second code
shape for small ops (a text glyph *is* one tile; binning it is pure overhead —
threshold on bbox area). The resolve's row carry currently flows across the
whole row; per-tile resolve gets its carry from the backdrop sum instead — a
different float association, so expect a one-time re-baseline (sense 2).

**-fbounds-safety.** Good shape: per-tile segment lists and micro-buffers are
small counted arrays; whole-vector checks per block as today. No
scatter/gather pressure beyond the current deposit path.

**Experiment.** Phase it: (1) binning + classification only, keeping serial
execution — measures whether empty/solid skipping pays by itself on the gallery
and `bench_render_large` (big fills = many solid tiles; stars = many partial
tiles); (2) then threads (§3.5). Paired bench + byte-diff (expect ≤1-ULP churn;
count moved PNGs) + memo. **Kill condition:** if after §3.1 the solid-tile fast
path wins < 5 % on the flagship, the complexity isn't paying rent serially and
the entry survives only as the threading vehicle.

### 3.5 Multicore

**Ruled out for now (Mike, 2026-06-10):** "Threading is kind of the last thing
I want to turn to — our callers could be doing the threading almost as well as
we could at this point (separate canvases for small cached tiles is the classic
design). Only if we find a spot that's not simple embarrassing parallelism, but
a spot where our specific knowledge of data and control flow makes us being the
ones to do the threading essential." Row-band threading is exactly the
embarrassing kind — a caller with N canvases gets the same parallelism today,
with better scheduling control and no thread policy imposed by a library. The
analysis below stays as the map of HOW we'd thread if a library-essential spot
ever appears (a decomposition only visible from inside — something the
N-canvases design structurally cannot reach); the bar is essentiality, not
speedup.

Two refinements to the ruling, same conversation. **The shape, should we ever
qualify:** a dependency-injected executor — `void enqueue(void (*fn)(void *ctx),
void *ctx)` plus `wait()` (or `wait_or_help_work()`) — so threads stay in the
user's control; the library never creates one ("a library that creates threads
feels close to a violation"). **The canonical essential spot:** shared coverage.
N tile-canvas callers each re-rasterize every path that crosses their tile; the
library could rasterize coverage once and hand each tile its slice — knowledge
of data flow the N-canvases design structurally cannot express. That is the
kind of case that would clear the bar, if its redundancy ever shows up hot.

What the ruling does NOT restrict: threads in **testing tools**. A harness that
draws and stitches 256×256 tiles from worker threads — emulating the threaded
user — keeps the library honest about thread safety (no hidden global mutable
state; distinct canvases must be fully independent), and a TSAN build variant
(separate from ASan/ISan/UBSan — they don't combine) makes that mechanical
rather than aspirational. Thread-safety-as-tested is a user-facing property held
regardless of whether src/ ever threads. (Landed: `tests/test_threads.c`
is that harness — parallel tile-stitch byte-equals serial — and the `tsan`
variant runs it under `-fsanitize=thread`, both on every bare `ninja`.)

A third refinement (Mike, same conversation): the essential spot may be better
**dissolved by API design than served by internal threads**. The Canvas 2D spec
only half-reifies coverage — `Path2D` is the geometry half, `clip()` re-derives
the mask per canvas — and stops short of "one thread makes a mask, many draw
through it." The project already augments the spec elsewhere (the typed filter
API, the counted `_n` text entries, record/replay, `canvas2d_load_png`). A
first-class immutable mask object — rasterize a path's coverage once,
then fill/stroke/image-draw *through* it from any canvas on any thread, the
sharing safe precisely because the object is frozen after build — would export
the library's coverage-reuse knowledge to the caller instead of hiding it
behind a thread pool the library shouldn't own. It is also a thin reification
of an existing internal stage (the dense u8 coverage buffer every op already
produces), and it would serialize like the other block types (the bitmap-block
machinery). If shared-coverage redundancy ever shows up hot, price THIS first;
the executor is the fallback, not the plan.

The constraint first: the byte-gate demands the same target bytes at any thread
count. That rules out nothing important — it just dictates the decomposition.
Pixel-partitioned schemes are bit-exact by construction (each cell's float-add
order is unchanged); edge-partitioned schemes need fixed-order reduction of
per-thread partial buffers (k× memory + a reduce pass — dominated, don't).

Ranked shapes:

- **(a) Row bands within an op.** Each worker takes a band of bbox rows; for
  accumulate, every worker walks *all* edges but deposits only rows in its band
  (the per-edge row loop clips to the band in O(1)); resolve/shade/blend are
  already row-independent. Bit-exact (sense 1), no format change, ~a hundred
  lines plus a pool. Amdahl says: shade+coverage+composite ≈ 88 % of compute is
  band-parallel, so 4 workers cap at ~2.9×, 8 at ~3.9× — *on big ops*.
  Fork/join latency (~µs on GCD) demands an area threshold; the gallery's many
  glyph-sized ops stay serial, so `bench_render` moves little and
  `bench_render_large` moves a lot. That asymmetry is acceptable: large canvases
  are where one core actually hurts.
- **(b) Tiles within an op** — §3.4's unit; same determinism story, better load
  balance on spiky content (a band can be all-edge while its neighbors are
  empty; tiles shuffle better). Costs the binning machinery first.
- **(c) Op-level parallelism via bbox disjointness.** Ops whose bboxes (plus
  filter margins) don't intersect — and that share no clip/state mutation
  between them — commute byte-exactly; schedule the op stream as a DAG. Attacks
  the many-small-ops regime (text!) that (a) can't reach. Costs lookahead
  (the record/replay layer already linearizes ops, a natural place to build the
  DAG) and care with the shared scratch buffers (`cv->tile`, `cv->cov` would
  need to become per-worker). Most complexity per win; third.
- **(d) Pipeline parallelism** (stage per thread): deterministic but capped by
  the 57 % shade stage at < 2× and serializes badly. Not worth it; listed to
  close the door.

**-fbounds-safety:** orthogonal — bands/tiles hand each worker counted
sub-slices; the flag has nothing new to say. GCD is the obvious pool (Apple-only
is already spent, per color-axis).

**Experiment.** (a) first, gated behind `bbox_area ≥ threshold`: paired bench on
`bench_render_large` at 1/2/4/8 workers + `bench_render` (must not regress) +
byte-diff (must be **zero** moved bytes — any churn is a decomposition bug, not
a re-baseline) + memo with the measured threshold. **Kill condition:** if
fork/join overhead eats the win below ~1.5× at 4 workers on
`bench_render_large`, shelve threading until canvases grow.

### 3.6 Targeted small fixes (cheap, do alongside)

- **Vectorize `canvas2d_clip`'s intersect loop.** 7.3 % of the profile is a scalar
  full-canvas `old * pc / 255`. 8-wide u8→f16→u8 (or integer (a*b*0x8081)>>23
  style) with the planar seams; bbox-limit the intersect (outside the path's
  bbox, `new = 0` — that's a memset, and `old` only needs multiplying inside).
  Probably a half-day for most of a 7 % line. Byte risk: the /255 must stay
  exact (it's integer today; keep it integer).
- **Sparse clip masks** (the old memo's "highest value, lowest friction" pick):
  intervals per row instead of a full-canvas byte plane. Pays in clip-heavy
  scenes, per-state copy cost, and composes with the blend stage's clip
  attenuation (skip fully-open rows). Becomes more attractive if clip usage
  grows; the profile says 7 % today, mostly the intersect loop above — do the
  cheap fix first and re-look.
- **Resolve micro-tuning:** interleave two 8-lane blocks per row iteration to
  hide the 3-step prefix-sum latency behind the carry chain. Caps at a slice of
  7 %; only worth it bundled with other resolve surgery (§3.1's fusion).

### 3.7 Quality items

- **The fold-order conflation (§2) has no oracle test.** Build the adversarial
  scenes — self-overlapping nonzero path with partial-coverage overlap pixels;
  evenodd star whose crossings land mid-pixel — and render against a 16×16
  supersampled reference. The behavior is the same as every winding-space
  rasterizer including Skia's analytic AA; the expected outcome is a measured
  bound, not a fix.
- **8-bit coverage is the coarsest quantizer in the pipeline** (1/510 vs f16's
  ~1/2048 near 1). A f16 coverage seam (which §3.1's fusion produces for free on
  the paint path) would lift it without a format change; whether any scene can
  see 1/510 is doubtful — check with a near-transparent large-gradient-fill
  scene before claiming.
- **Stroking quality is geometric, not coverage-bound:** round joins/caps are
  64-segment polygon fans (`emit_disc`), `sinf`/`cosf` visible in the profile at
  1 %. Adaptive-to-radius is already there; nothing urgent. Dashes cap at 2^20
  spans (DoS guard), miter math is standard. The known deviation: zero-length
  subpaths don't get caps (README notes it).
- **Exactness under f16 is settled by committed tests** (color-axis §5 gates).
  Nothing to re-litigate until output exceeds 8 bits.

### 3.8 Field notes (from knowledge, not measurement)

For calibration against the wider world — claims here are from literature, not
from our benches:

- **Skia** keeps *both* dense and RLE mask formats with per-mask choice, and its
  analytic scan converter (the SkAnalyticEdge lineage) accumulates exact areas
  much as ours does, with the same winding-space conflation. The
  SkRasterPipeline design — small composable stages with a vector ABI — is the
  direct ancestor of our planar small-functions-in-q0-q3 style; ours is the
  ahead-of-time, fixed-function cousin (we don't JIT stage chains; we don't need
  to — the pipeline shape is static per paint kind).
- **FreeType's smooth rasterizer / stb_truetype**: sparse cell accumulation
  (§3.3's AA variant) — the proof that O(edge-pixels) accumulation works and is
  the right shape for glyphs; also the proof that it's scalar-grained.
- **Pathfinder** demonstrated CPU-side tile classification (alpha tiles vs solid
  tiles) feeding a GPU; the classification economics (§3.4) are its result.
- **Vello / piet-gpu**: full GPU compute binning — flatten → bin → coarse →
  fine, per-tile command lists, backdrop winding per tile. The CPU adaptation in
  §3.4 borrows exactly two ideas (backdrop + empty/solid/partial classification)
  and deliberately not the rest; the GPU version's economics depend on
  thousand-way parallelism we don't have.
- **Blend2D**: the closest existing system to "this codebase plus threads" — a
  band-based multithreaded CPU rasterizer with JIT-compiled pipelines, claiming
  multi-× wins over single-threaded AGG/Cairo-class renderers. Its banding is
  §3.5(a) with the JIT we don't need at our pipeline count.

### 3.8 The unified draw pipeline (erasing the canvas/compositor line)

A model of a draw (Mike, 2026-06-10), now that both sides of the old
canvas/compositor split are CPU:

```
float x,y inputs
half  cov = cov_fn(x,y,cov_ctx)
if (cov == 0) return
f16x4 src = shader_fn(x,y,shader_ctx)
f16x4 dst = load_dst(ptr, fmt)
f16x4 blend = blend_fn(src,dst)
if (cov < 1) blend = lerp(dst, blend, cov)
store_dst(ptr, fmt, blend)
```

Mapped onto today's pipeline: `cov_fn` is the resolved coverage plane (it stays
materialized in every variant — analytic coverage is born from scanline
accumulation, not point evaluation); `shader_fn` is `paint_tile`'s planar fold;
`load/blend/store_dst` is the blend stage; and the seam between them is a full
premultiplied f16 **tile**, written by shade and re-read by composite — a
~16 B/px round trip that exists only because these were once different
machines. The experiment: evolve the structure literally to the model, racing
both shapes Mike named — (a) staged planar row buffers between stages, vs (b) a
fused per-block register pipeline (`canvas2d_px8` is an HVA, so stage functions
compose through q0–q3 without fusing the *code*) — and let paired benches pick.
The fused shape deletes the tile on the no-filter path outright.

Two things the model surfaces that a refactor must not blur past:

- **Coverage semantics, ruled (Mike, 2026-06-10) — IMPLEMENTED (same day):**
  "in principle it's done as a lerp between the output of the blend function
  and the destination" — folding coverage into source alpha "is correct math
  for several blend modes, but not all of them." The criterion, from
  `co = Fa·s + Fb·d`: fold ≡ lerp exactly when `Fa` is `sa`-free and `Fb` is
  affine in `sa` with `Fb(0)=1`. As landed (`coverage_folds`, now in
  [canvas2d.c](../src/canvas2d.c), re-folded in the seam-efficiency
  pass): source-over, destination-over, destination-out, source-atop, xor,
  AND all 15 blend modes fold; copy, the in/out family, destination-atop,
  and **lighter** lerp — shade skips its coverage fold for those, the op's
  u8 coverage buffer rides to `canvas2d_blend` as its own plane, and the
  kernel computes `out = lerp(dst, blend(src,dst), op_cov × clip_cov)` (clip
  coverage was the same bug and takes the same lerp). Two findings sharpened
  the ruling while implementing, both pinned by the oracle
  (tests/test_coverage_lerp.c): (1) lighter passes the Fa/Fb criterion only
  unclamped — its `co = s + d` saturates, the supersampled truth clamps per
  subsample, and the fold was up to 0.25 off the oracle where it clamps, so
  lighter lerps; (2) every separable/non-separable blend is fold-EXACT in
  exact arithmetic — the premultiplied term `T = sa·da·B(d/da, s/sa)` is
  degree-1 homogeneous in `(s, sa)`, so for blends fold vs lerp moves only
  f16 rounding, never semantics. The fix's #28 commit took the lerp for
  blends (the recorded criterion); the seam pass cashed the homogeneity
  license back in — blends re-fold shade-side, deleting their coverage-plane
  read and lerp, moving blend.png alone by the same 368 px / max 1/255 the
  lerp had moved it, oracle green (part A fold error ≤ 0.0015 of the double
  reference, part B's idealized fold-vs-lerp ≤ 3e-15 summed). The genuine
  fixes are copy/in/out/dst-atop (oracle error sums collapsed ~800×;
  porterduff.png re-baselined, 1030 px, max delta 163) and lighter's clamp
  edge — those keep the lerp; they NEED it. Filtered ops still fold before
  the filter runs — blur consumes the silhouette from the tile's alpha,
  after which coverage genuinely is source alpha.
- **Materialization boundaries stay.** blur()/drop-shadow()/shadows need the
  op's spatial extent: the filter path keeps a tile; shadows are a second
  pipeline invocation (blurred cov_fn, tint shader_fn). The fused pipeline is
  the fast path for the empty-filter-list case, i.e. nearly always.
- The model's two `if`s become block predicates 8 lanes at a time; the
  cov==0 early-out is §3.4's empty-classification arriving through the front
  door.

**Landed (2026-06-10, the seam-efficiency pass — step 2 of 3,
"efficient, not necessarily unified").** Four structural changes, each
byte-still except the licensed re-fold:

1. **The splat:** lerp-family solid paints (and clearRect/reset's unit-alpha
   erase, and the full-strength shadow tint) write no tile —
   `canvas2d_blend_solid` takes the one premultiplied colour and the region
   walk splats it, deleting a ~16 B/px round trip that carried zero
   information.  Byte-identical by construction and by test
   (test_blend's solid_vs_tile pins all 26 modes × coverage shapes
   bit-for-bit; blend8 now delegates source-over to src_over8 — the generic
   `fa*s + fb*d` arm contracts differently than `s + fb*d`, a 1-ULP trap no
   caller could previously reach).
2. **The block predicates — landed flat, then removed.** The model's
   `if (cov==0) return` as one u64 compare per 8-px block, a vector test for
   transparent-black source blocks, the k = 1 short path — all bit-exact, all
   priced ~flat on the committed mixes (the §1 histogram's 85 % full coverage
   explains why).  Removed for no perf win; the paired removal bench measured
   −1 %/−3 % flagship, so the tests themselves were a small tax.  The design and
   prices are recorded here and in git history if sparse content ever arrives to
   re-purchase them.
3. **The re-fold — landed flat, then removed.** Blends folding coverage
   shade-side again per the homogeneity license priced ~flat-to-−1 %; removal
   restores the one-sentence rule (the proven over-family folds, everything else
   lerps) and blend.png's lerp pixels (≤ 1/255, lockstep).  The homogeneity
   finding stays true and recorded above — a license not currently needed.
4. **The row-granular handoff — tried, measured, dropped.** Unfiltered ops
   shading one reused tile row and compositing it cache-hot (the staged shape
   of the §3.8 race at row granularity) was implemented and priced: −3 % on
   `bench_render` (the gallery's many small ops pay a blend call per ROW
   instead of per op), +0.8 % ≈ 1σ on `bench_render_large`, flat e2e.  The
   per-call overhead beats the cache win at our op sizes; the code is not
   committed (this entry is its record).  The result is also the first real
   data point for #27: interleaving the stages at row granularity LOSES —
   whatever fusion wins, it must win by eliminating the handoff entirely
   (registers), not by shrinking its granularity.

Measured (interleaved A/B medians, gallery + flagships): the splat bought
−7.8 % `bench_render` / −6.8 % `bench_render_large` and is the pass's one
survivor; the predicates and the re-fold priced flat and were removed
(removal itself measured −1 %/−3 % — the flat paths were a small net tax);
row-granular lost outright (above).  The economics: buffer-based handoff's
benefit is each stage running at its own full speed, its cost one memory round
trip per handoff — so the seam wins came from deleting traffic (the splat), not
from finer scheduling or speculative skips.  What remains for
the unification question (#27, step 3): the folding paths still write and
re-read the whole-op tile (16 B/px round trip, DRAM-scale for large ops);
the u8 coverage seam between resolve and shade (2 B/px + the /255 refold)
still stands; the lerp family still rides its plane.  Full fusion would move
blocks through q0–q3 registers instead — and the row-granular loss says the
win has to come from deleting the handoff, not rescheduling it.

**Landed (2026-06-11, task #36): the compositor was removed.**  The object, its
ABI (compositor.h), and compositor_cpu.c are gone; the kernels moved verbatim
into canvas.c, dispatching on the public web-named `canvas2d_composite_op` (the
COMPOSITOR_* mirror and the static_asserts that pinned it retired).  Two copies
were removed with it: the full-canvas clip copy (per clip change AND per restore
— the kernels now take the canvas's own mask as a call parameter, so
putImageData's ignore-the-clip is just passing NULL) and the readback staging
copy (canvas2d_read_rgba un-premultiplies straight off the target the canvas now
owns).  Byte-still at
every commit; the oracle tests retargeted to the internal seam
([canvas2d_blend.h](../src/canvas2d_blend.h)) with every bound intact.  Paired
hyperfine (deleted-copy traffic, same kernels): `bench_render_large`
95.6→89.5 ms (−6.4 %, the 1024² per-frame readback + clip copies),
`bench_render` 10.6→10.3 ms (−2.8 %, ≈1σ), e2e `bench` and `bench_blit`
flat.

## 4. Ranked next experiments

(#1, planarize the shade stage, **landed 2026-06-10** — −29/−39 % flagship,
zero bytes moved; outcome recorded in §3.1. The list below is the post-landing
re-rank against §1's new, flatter pie.)

(Row-band threading briefly held the #1 slot after the shade landing — the only
entry that buys a multiple, ~2.5–3.5× on `bench_render_large`, bit-exact by
construction. **Ruled out 2026-06-10**: it's exactly the embarrassing
parallelism a caller already has via N canvases over cached tiles; §3.5 carries
the ruling and the essentiality bar a future threading proposal must clear. The
how-to-thread analysis stays parked there.)

(§3.8's coverage-semantics ruling **landed 2026-06-10** — the fold-vs-lerp
split is in the kernel, the oracle pins it, porterduff/blend re-baselined; the
outcome is recorded in §3.8. Sequencing for the rest: seam efficiency next, then
the unification question.)

(#1, seam efficiency, **landed 2026-06-10** — the splat, the block predicates,
and the blend re-fold; row-granular tried and dropped; outcome and the remainder
recorded in §3.8's landed note. The staged-row shape won by default at row
granularity; the fused-register shape is #27's remaining question, now fighting
for row-cache bandwidth rather than DRAM.)

(#1, vectorize the bilinear sampler interior of `draw_image_quad`, **tried and
reverted 2026-06-20** — the kill condition fired. `sample_src_8wide` did the
weight/lerp/clamp 8-wide (taps still scalar gathers), byte-exact. But the new
`bench_drawimage` (added to price exactly this pole — it had no bench before)
measured **1.01× ± 0.05× vs the per-lane form, a wash**: the four tap-gathers
(~16 byte-loads/pixel) dominate, so vectorizing the arithmetic around them buys
nothing — precisely the "gather/insert shuffling eats the arithmetic win"
predicted. The ~150 lines were reverted; `bench_drawimage` stays so the next
attempt can re-price before committing. Lesson: profile-scene's "`sample_src`
self-time halved" was redistribution into `draw_image_quad`, not a speedup —
proportions located it, only hyperfine priced it.)

(#2, `canvas2d_clip` bbox-limit, **landed 2026-06-20** — the cheap half of §3.6.
The intersect now memsets the mask and multiplies only the path's canvas-clamped
bbox; `canvas2d_clip` dropped off the profile-scene top-15 (was 10.6 %), byte-exact.
The `old*pc/255` stayed the scalar mul+shift the compiler already emits — the
8-wide inner multiply is the remaining, now-smaller half, below.)

The post-pass re-rank (sampler reverted, clip bbox-limited; coverage and
composite are now the largest buckets the gallery still pays per pixel):

1. **Tile classification, serial first** (§3.4 phase 1: bin + empty/solid skip,
   no threads). Coverage (~22 %) and composite (~17 %) are the bigger targets
   now; tile classification attacks both via the solid-tile fast path. *Kill:*
   < 5 % flagship, or a regression on concave/small-op content; if killed, keep
   the binning design notes as the threading load-balancer fallback.
2. **Vectorize `canvas2d_clip`'s in-bbox multiply 8-wide** (the remaining half of
   the bbox-limit landing). 16-wide `(old*pc*0x8081)>>23` — the mul+shift the
   compiler already emits scalar — with planar seams; keep /255 integer-exact.
   Smaller now that the bbox-limit landed and `canvas2d_clip` left the top.
   No memo; the byte-gate is the gate.

Below the line, in order: the conflation oracle scenes (§3.7 — quality bound,
not speed), resolve→shade f16-coverage fusion (§3.1's step two — deletes 2 B/px
and the /255 refold, but the shade fold it feeds is no longer the pole),
AET/sparse-cell prototype (§3.3 — only if a text/stroke-dominated workload
becomes a flagship), full RLE coverage (§3.2 — re-price against the cheaper
dense shade; a zero block now costs ~a dozen vector ops, so skipping buys
mostly coverage/composite traffic, not shade compute).

## Open questions (unmeasured)

- **Bandwidth vs compute:** the 42 B/px floor and the ~10–15 %-of-frame traffic
  estimate are arithmetic, not measurement. A counters pass (Instruments,
  `os_signpost` around stages) would settle how much of #2's ceiling is
  bandwidth.
- **Per-op fixed costs are unprofiled.** The gallery is many small ops; how much
  of `bench_render` is per-op overhead (bbox, reset, dispatch) vs per-pixel work
  is exactly what #1's kill condition would reveal — nobody has measured it
  directly.
- **GCD fork/join latency at our op sizes** is folklore (~µs); measure
  `dispatch_apply` overhead on this machine before designing #2's threshold.
- **Whether any committed scene can see the 1/510 coverage quantizer** — claimed
  doubtful above, untested.
