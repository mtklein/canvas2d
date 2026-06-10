# Rasterization: the standing survey

Status: **living document.** This is the map we return to whenever rasterization is
the slow pole — algorithms, quality, parallelism, vectorizability. Unlike the
decision memos in [decisions/](decisions/), nothing here is settled; entries get
re-priced as the profile moves. Each option carries the experiment that would
price it, in the house style: a paired bench, a byte-diff of the 33 gallery PNGs,
and a memo.

Last grounded: 2026-06-10, one `ninja profile-scene` pass (release gallery,
`GALLERY_REPS=200`, 4 s `sample` window, Apple M4 Max) plus the measured numbers
in [decisions/opt-level.md](decisions/opt-level.md),
[decisions/color-axis.md](decisions/color-axis.md),
[decisions/gradient-eval.md](decisions/gradient-eval.md), and
[sparse-coverage.md](sparse-coverage.md).

## 1. Where the milliseconds go

Top-of-stack self-time across the whole gallery (all 33 scenes, PNG encode
excluded by the script's design — codec work happens only on the final rep,
outside the sample window). The script prints the top 15 lines; everything below
`sinf`'s 28 samples is ≤ ~1 % each. 2,766 samples visible:

| function | samples | share | stage |
|---|---|---|---|
| `paint_tile` | 586 | 21.2 % | shade |
| `cnvs_premultiply` | 543 | 19.6 % | shade (called per pixel from the paint loops) |
| `draw_image_quad` | 392 | 14.2 % | shade (image sampling + per-pixel premultiply path) |
| `cnvs_cover_add_edge` | 248 | 9.0 % | coverage accumulate (accum_row/deposit inlined) |
| `compositor_blend` | 221 | 8.0 % | composite |
| `canvas_clip` | 201 | 7.3 % | clip-mask intersect (full-canvas scalar loop) |
| `cnvs_cover_resolve` | 196 | 7.1 % | coverage resolve |
| `src_over8` | 77 | 2.8 % | composite |
| `cnvs_px8_load_k` | 74 | 2.7 % | composite (tails) |
| `cnvs_mip_halve` | 55 | 2.0 % | image (emoji mips) |
| `cnvs_gradient_color_row` | 43 | 1.6 % | shade |
| `cnvs_px8_store_k` | 38 | 1.4 % | composite (tails) |
| `blur_box_h_f16` | 35 | 1.3 % | filters |
| `cnvs_verts_tri` | 29 | 1.0 % | stroke geometry |
| `sinf` | 28 | 1.0 % | stroke geometry (emit_disc) |

Bucketed by pipeline stage:

| stage | share of visible compute |
|---|---|
| **shade** (coverage → premultiplied f16 tile: `paint_tile` + `cnvs_premultiply` + `draw_image_quad` + gradient row) | **56.5 %** |
| **coverage** (accumulate + resolve) | 16.1 % |
| **composite** (planar f16 blend incl. tails) | 14.8 % |
| **clip** (mask build/intersect) | 7.3 % |
| stroke geometry | 2.1 % |
| image mips | 2.0 % |
| filters/blur | 1.3 % |
| **flattening** | absent from the top 15: **< 1 %** |

**The slow pole, named with a number: the shade stage — the scalar per-pixel
coverage→tile fold — at ~57 % of gallery compute self-time**, and within it the
pair `paint_tile` + `cnvs_premultiply` at **41 %**. This is the stage the planar
conversion *didn't* reach: the color-axis ruling deliberately left "paint_tile's
scalar coverage/alpha fold" in scalar f32 because the pre-planar profile didn't
implicate it. The compositor then got 3–4× faster (planar f16, −17/−25 % flagship)
and the pie re-divided: what used to hide behind `compositor_blend`'s 34 % is now
the biggest line item. Rasterization *proper* — accumulate + resolve — is 16 %.

Caveats on these numbers, stated once:

- `sample` gives proportions, not milliseconds. Absolute anchors come from
  hyperfine: `bench_render` 18 ms, `bench_render_large` 179 ms (1024², 10 frames
  ≈ 17.9 ms/frame), e2e `bench` 45 ms and codec-bound (README table). The
  profile is the *gallery* mix; `bench_render_large`'s few-huge-fills shape
  weights the same stages differently (more composite, less per-op overhead).
- Attribution is smeared by inlining at `-Os`: `accum_row`/`deposit` fold into
  `cnvs_cover_add_edge`, the prefix sum and fill-rule fold into
  `cnvs_cover_resolve`. `cnvs_premultiply` is a real out-of-line call, so its
  19.6 % splits across `paint_tile`, `draw_image_quad`, the pattern loop, and the
  shadow tint — all per-pixel scalar callers.
- **A correction to tribal knowledge:** `src/` spawns **zero threads**. Nothing
  in the tree calls dispatch/pthreads — the blur included (it's plain
  single-threaded loops). The `__workq_kernreturn` pile that `sample` filters out
  is system frameworks' parked workers, not ours. The whole pipeline is one core,
  full stop.

## 2. The architecture today, stated honestly

One pipeline, per op, over the op's device-space bounding box
([canvas.c](../src/canvas.c)):

1. **Flatten** at path-build time ([cnvs_path.c](../src/cnvs_path.c)): curves
   subdivide adaptively (`cross_chord2` flatness, depth-capped, NaN-as-flat) into
   device-space line segments. Cost is invisible in the profile (< 1 %).
2. **Accumulate** ([cnvs_cover.c](../src/cnvs_cover.c)): for each edge, walk its
   scanline rows; per row, deposit exact signed trapezoid areas into a dense
   per-bbox `float` buffer — two partial-column deposits at the span ends, and the
   interior **telescoped** into a contiguous constant-add done 8-wide with one
   whole-vector bounds check per block. The buffer is memset per op
   (`cnvs_cover_reset`).
3. **Resolve**: per row, an 8-wide in-register Hillis-Steele prefix sum (+ scalar
   carry between blocks) turns area deltas into winding, folds by fill rule
   (nonzero = clamp |run|; evenodd = triangle wave), and quantizes once to a
   dense u8 coverage buffer.
4. **Shade** (`paint_tile` / `paint_tile_pattern` / `draw_image_quad`): per pixel,
   scalar — read u8 coverage, refold to f32, fold paint alpha × global alpha,
   `cnvs_premultiply` (4-lane f16), store into the premultiplied RGBA16F tile.
   Gradients get their parameters and colors solved 8-wide per row
   ([gradient-eval.md](decisions/gradient-eval.md)) but the coverage fold around
   them is still scalar.
5. **Composite** ([compositor_cpu.c](../src/compositor_cpu.c) +
   [cnvs_planar.h](../src/cnvs_planar.h)): 8 pixels per step as four f16 channel
   planes, ld4/st4 at the seams, all 26 modes straight-line vector code, clip
   attenuation folded in. This is the fast, finished part.

Strokes go through the same path: [cnvs_stroke.c](../src/cnvs_stroke.c) expands
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
  Canvas itself is defined op-at-a-time, browsers do the same, and our
  scene-replay model depends on it — listed here so nobody "fixes" it without
  noticing it's load-bearing.
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
  **42 B of traffic per bbox pixel**, before one pixel visibly changes.
  [sparse-coverage.md](sparse-coverage.md) measured 39–59 % of a fill's bbox at
  zero coverage (convex → concave). At `bench_render_large` scale that's roughly
  260 MB/frame of floor traffic — ~10–15 % of the 17.9 ms frame at plausible
  single-core bandwidth, so we are **compute-bound, not bandwidth-bound**
  (estimate, unmeasured; consistent with a scalar shade stage dominating the
  profile).
- **The shade stage is scalar.** §1's 57 %. The planar vocabulary to fix it
  already exists in [cnvs_planar.h](../src/cnvs_planar.h) —
  `cnvs_h8_from_u8` (coverage plane), `cnvs_px8_premultiply`, `cnvs_px8_store` —
  it just was never wired into `paint_tile`.
- **One core.** Everything above is serial. The op stream is inherently ordered
  (each op read-modify-writes the shared target), but within an op every stage
  is row-independent, and across ops disjoint bboxes commute byte-exactly.
- **Per-clip full-canvas work.** `canvas_clip` allocates and walks width×height
  scalar per `clip()` call, and the mask is copied by value into saved states.
- **The u8 round-trip between resolve and shade.** Resolve quantizes to u8;
  shade immediately reloads and divides by 255. Two passes over the bbox where
  one would do, plus a quantize that exists only because the seam is a byte
  buffer (the shadow and clip paths do want bytes; the paint path doesn't).

### Determinism, the two senses

The byte-gate (33 committed PNGs, replayed byte-for-byte across machines) imposes
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
one f16 plane (`cnvs_h8_from_u8` — exists), fold `paint_alpha × global_alpha ×
coverage` as plane math, `cnvs_px8_premultiply` (exists), `cnvs_px8_store`
(exists). For solid paint the whole loop is a color splat, ~4 multiplies, and an
st4 per 8 pixels. For gradients, `cnvs_gradient_color_row` already produces a row
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
kernel-level factor, the gallery compute pie loses on the order of 20–30 %.
That's the expected-value champion and it's pure SIMD — no format change, no new
invariants.

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

**Experiment.** A standalone `cnvs_cover_aet` with the same
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

- **Vectorize `canvas_clip`'s intersect loop.** 7.3 % of the profile is a scalar
  full-canvas `old * pc / 255`. 8-wide u8→f16→u8 (or integer (a*b*0x8081)>>23
  style) with the planar seams; bbox-limit the intersect (outside the path's
  bbox, `new = 0` — that's a memset, and `old` only needs multiplying inside).
  Probably a half-day for most of a 7 % line. Byte risk: the /255 must stay
  exact (it's integer today; keep it integer).
- **Sparse clip masks** (the old memo's "highest value, lowest friction" pick):
  intervals per row instead of a full-canvas byte plane. Pays in clip-heavy
  scenes, per-state copy cost, and composes with the compositor's clip
  attenuation (skip fully-open rows). Becomes more attractive if clip usage
  grows; the profile says 7 % today, mostly the intersect loop above — do the
  cheap fix first and re-look.
- **Resolve micro-tuning:** interleave two 8-lane blocks per row iteration to
  hide the 3-step prefix-sum latency behind the carry chain. Caps at a slice of
  7 %; only worth it bundled with other resolve surgery (§3.1's fusion).

### 3.7 Quality items

- **The fold-order conflation (§2) has no oracle test.** Build the adversarial
  scenes — self-overlapping nonzero path with partial-coverage overlap pixels;
  evenodd star whose crossings land mid-pixel — render against a 16×16
  supersampled reference, and *decide whether we care*. Expectation: we ship the
  same behavior as every winding-space rasterizer including Skia's analytic AA;
  the doc-worthy outcome is a measured bound, not a fix.
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

## 4. Ranked next experiments

1. **Planarize the shade stage** (§3.1: `paint_tile` solid+gradient, then
   `draw_image_quad`'s fold; optionally fuse resolve's coverage emit).
   *Why first:* it attacks the measured 41–57 % with the exact recipe that has
   paid out twice (compositor −17/−25 %, filter matrix), using vocabulary that
   already exists; no format change, no determinism exposure in the f32-fold
   arm. *Expected:* order −15–25 % on the flagship renders if the stage gets the
   compositor's kernel factor; even half that beats everything else on this
   list per unit risk. *Kill:* < 3 % on `bench_render` (would mean small-op
   overhead, not pixels, dominates the gallery — itself a finding that re-aims
   this whole document at per-op fixed costs), or gallery bytes that can't be
   held in the f32-fold arm.
2. **Row-band threading within large ops** (§3.5a, behind an area threshold).
   *Why second:* it's the only entry that buys a *multiple* rather than a
   percentage, the decomposition is bit-exact by construction (byte-diff must
   come back zero — it's a correctness gate, not a re-baseline), and it's
   independent of #1 (run it after, so Amdahl numbers reflect the new pie).
   *Expected:* ~2.5–3.5× on `bench_render_large` at 4–8 workers on the ~88 %
   parallel share; `bench_render` ~flat (threshold keeps small ops serial).
   *Kill:* < 1.5× at 4 workers (fork/join overhead at our op sizes), or any
   nonzero byte-diff (decomposition bug — fix or abandon, never re-baseline
   around nondeterminism).
3. **Tile classification, serial first** (§3.4 phase 1: bin + empty/solid skip,
   no threads). *Why third:* it's the structured replacement for RLE (no per-run
   chooser cliff), it's the load-balancing upgrade path for #2, and phase 1
   prices the format without buying the complexity blind. *Expected:* on
   `bench_render_large`'s near-full-canvas fills, most tiles classify solid —
   shade/blend keep their cost but coverage (16 %) mostly vanishes and the
   opaque-store fast path trims blend; call it 5–15 %, honestly uncertain. On
   stars/concave, near-zero win — the kill case is overhead there. *Kill:*
   < 5 % flagship after #1 has landed, or a measurable regression on
   concave/small-op content; if killed, keep only the binning design notes as
   #2's load-balancer fallback.

Below the line, in order: `canvas_clip` vectorization (§3.6 — do it as a
drive-by, it needs no memo-grade ceremony), the conflation oracle scenes
(§3.7 — quality bound, not speed), AET/sparse-cell prototype (§3.3 — only if a
text/stroke-dominated workload becomes a flagship), full RLE coverage (§3.2 —
only re-price after #1 and #3 move the floor).

## Open questions (wanted to claim, couldn't ground)

- **Bandwidth vs compute:** the 42 B/px floor and the ~10–15 %-of-frame traffic
  estimate are arithmetic, not measurement. A counters pass (Instruments,
  `os_signpost` around stages) would settle how much of #2's ceiling is
  bandwidth.
- **Per-op fixed costs are unprofiled.** The gallery is many small ops; how much
  of `bench_render` is per-op overhead (bbox, reset, dispatch) vs per-pixel work
  is exactly what #1's kill condition would reveal — nobody has measured it
  directly.
- **The coverage histogram is stale.** 39–59 % zero-coverage predates the text
  and emoji scenes; re-run before pricing §3.2/§3.4 seriously.
- **GCD fork/join latency at our op sizes** is folklore (~µs); measure
  `dispatch_apply` overhead on this machine before designing #2's threshold.
- **Whether any committed scene can see the 1/510 coverage quantizer** — claimed
  doubtful above, untested.
