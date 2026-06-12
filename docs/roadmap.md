# Canvas 2D coverage: the honest gap list

The README's capability table shows what works. This is the complement: a sweep of
the full [`CanvasRenderingContext2D`](https://html.spec.whatwg.org/multipage/canvas.html#canvasrenderingcontext2d)
surface against what we actually implement, splitting **partial** (we have it, but
narrower than spec) from **missing entirely**, plus the handful that are genuinely
out of scope for a headless C library. (Last swept against the living spec:
June 2026 — the sweep that caught `lang`, `colorType`/`pixelFormat`, and
`scrollPathIntoView`'s removal.)

This project isn't chasing spec completeness for its own sake — it's a vehicle for
learning `-fbounds-safety`. So the selection is biased: we prioritize features
whose hot path is *indexed-buffer-dense*, where bounds checking has something to
say, over features that are mostly plumbing or string parsing.

## Near-term plan

Chosen because their kernels are exactly the kind of dense, per-pixel,
indexed-buffer work `-fbounds-safety` is meant for (and good SIMD targets):

1. ~~**`globalCompositeOperation`**~~ — **done** (all 26 modes), as the
   checked-C blend kernels (item 2). (A since-removed Metal backend ran
   the same math as fixed-function source-over plus a framebuffer-fetch shader; see
   [decisions/metal-backend.md](decisions/metal-backend.md).)
2. ~~**A software blend stage**~~ — **done**, and blending is now a stage of
   [canvas.c](../src/canvas.c) itself, not a backend: the per-pixel
   `blend8(src, dst, mode)`
   kernel is all 26 composite/blend modes, premultiplied, over `__counted_by`
   tiles, ~350 lines of checked C compositing onto the canvas's own RGBA16F
   target. It began life behind a compositor ABI chosen against a Metal GPU
   backend at build time and held
   bit-for-bit identical to it by a tolerance-0 differential; once measurements showed
   the CPU path winning the flagship workload, Metal was removed, the GPU-parity
   rounding it required was dropped, and the compositor object itself was later
   dissolved into canvas.c (see
   [decisions/backend-differential.md](decisions/backend-differential.md) and
   [decisions/metal-backend.md](decisions/metal-backend.md)).
3. ~~**Shadows**~~ — **done** (fills/strokes/text/images). The op's coverage mask is
   blurred by [blur.c](../src/blur.c)'s separable box passes (the stencil-loop
   probe, three passes ≈ Gaussian), tinted, offset, and composited under the
   shape — all in checked C. `filter`
   `blur()` (item 5) shares the same three-pass structure.
4. ~~**`filter`, the colour functions**~~ — **done** (`brightness`, `contrast`,
   `grayscale`, `hue-rotate`, `invert`, `opacity`, `saturate`, `sepia`), behind
   a typed API (`canvas_add_filter_*` — no CSS string parsing). Each compiles
   at add time to a 3x3 matrix + alpha-scaled offset in closed premultiplied
   form ([cnvs_filter.c](../src/cnvs_filter.c)) and applies per pixel to the
   op's premultiplied tile, before its shadow and composite.
5. ~~**`filter` `blur()`**~~ — **done**. An RGBA16F flavour of
   [blur.c](../src/blur.c)'s running-sum box blur (three passes ≈ Gaussian,
   stdDev = the given px) over the op's premultiplied tile, blurring against
   transparency rather than clamped edges; each paint site widens its bbox by
   the filter chain's spread so the soft skirt outgrows the shape. Held to a
   brute-force reference in `test_filter`.
6. ~~**`filter` `drop-shadow()`**~~ — **done**, and with it the filter list is
   feature-complete. The entry composites the op's tile source-over on top of
   a blurred (item 5's passes), offset, colour-tinted copy of its own alpha —
   shadow-under-drawing as one image, which later list entries keep filtering
   (a colour function after a drop-shadow recolours the shadow too; both
   orders pinned in `test_filter`). All that's left of `filter` spec-wise is
   the CSS string form (deliberately out of scope — see the partial row below)
   and `url()` references to SVG filter elements (nothing to point into
   without a document — the out-of-scope list).

Internals (not API features) considered and deferred:

- A sparse/RLE coverage format to skip the transparent ~40–60% of a fill's bbox —
  analysis and why dense+SIMD stays the default in [sparse-coverage.md](sparse-coverage.md).
- The internal colour type — **settled**. A devil's-advocate pass kept `_Float16`
  storage and corrected its rationale (it round-trips the spec's 8-bit edges
  exactly where u8 corrupts ~half of them) — see
  [decisions/float16-color-type.md](decisions/float16-color-type.md). The arm it
  left open was then measured three ways (f32-everywhere vs f16-storage/f32-compute
  vs pervasive 8-wide `_Float16` compute) and ruled in
  [decisions/color-axis.md](decisions/color-axis.md): **pervasive f16 compute
  landed** — the blend kernel (all 26 modes), the filter colour matrix, the
  gradient stop lerp, premultiply, and readback all run f16 arithmetic 8 lanes
  wide, 13–15% faster on the flagship renders, with the 8-bit round-trip still
  exhaustively exact and blends within 1/255 of a double reference (both now
  pinned as tests). The lone f32 holdout is the blur's running-sum accumulator,
  kept on measured accuracy grounds (`blur.h`).

## Partial — implemented but narrower than spec

- **Text styling** covers `set_font_size`, `textAlign` (start/end resolving
  against direction), `textBaseline`, and `direction` (ltr/rtl, the bidi
  paragraph base -- it reorders mixed-direction text, resolves neutrals, and
  rides the shape-block cache key through record/replay). No font-family
  selection (pinned to Libian TC), weight, style, or the CSS `font` shorthand;
  and none of `lang` (a recent spec addition), `letterSpacing`, `wordSpacing`,
  `fontKerning`, `fontStretch`, `fontVariantCaps`, `textRendering`.
- **Text shaping**: `fillText`/`strokeText`, `measureText`, `textAlign`, and
  `maxWidth` all go through Core Text shaping (`cnvs_shape_text`) with font fallback
  — so code points Libian TC lacks now both draw and measure (color emoji from one
  canonical 160px RGBA8 capture per glyph, mip-sampled at draw; other fallback
  runs as outlines), and a string measures the way it draws. The shaper's
  complex layout is surfaced too: ligatures, contextual forms (Arabic joining),
  and bidi reordering all draw through the public API, with the `direction`
  attribute setting the paragraph base the runs are ordered against (the
  gallery's `rtl` scene; `test_rtl`, `test_shaping`). The shaped line also
  answers selection and caret queries — `cnvs_shaped_selection` maps a logical
  range to its visual x-spans (a bidi range splits into several),
  `cnvs_shaped_x_at_index` places a caret with mid-cluster indices snapped to
  the cluster's edge, and `cnvs_shaped_index_at_x` hit-tests a click back to a
  logical index (the gallery's `selection` scene; `test_shaping`). These are
  internal (`cnvs_text.h`) — no public mirror yet.
- **PNG I/O**: `canvas_write_png` writes real compression (Up-filtered rows +
  the in-house `cnvs_zlib` deflate — Up-only because it vectorizes as whole-row
  ops with no left-neighbor recurrence, and the adaptive five-filter chooser
  only measured ~2% smaller across the gallery), and `canvas_load_png` reads
  those files back, byte-exact. The decoder is deliberately scoped to our own
  encoder's output — 8-bit RGBA, non-interlaced, None/Up filters, every chunk
  CRC-verified, dimensions capped — and strictly rejects the rest of the PNG
  universe (palette, gray, 16-bit, interlace, Sub/Avg/Paeth), keeping the
  untrusted-parser surface small. There is no general-purpose PNG importer,
  and no `toDataURL`/`toBlob` string forms.
- **`Path2D`** has no SVG path-data string constructor (string parsing); the
  constructible object, `add_path`, the builders, and the
  fill/stroke/clip/isPointIn* overloads are all supported.
- **`fillStyle`/`strokeStyle`** are solid colour (float RGBA, not CSS color
  strings), linear/radial/conic gradients, and image patterns — and the
  gradients/patterns are state setters (the pattern image is borrowed), not
  reusable first-class objects, so `CanvasPattern.setTransform` has no
  analogue: a pattern is pinned to the CTM in effect when it is set.
- **`getImageData`** is fixed RGBA8; none of `ImageDataSettings` —
  `colorSpace`, or the recent `pixelFormat` (`rgba-unorm8` | `rgba-float16`,
  the `Float16Array`-backed `ImageData` flavour).
- **`drawImage`** sources only our packed RGBA8 buffer (no canvas/image-as-source);
  it samples bilinearly, or nearest-neighbour when image smoothing is disabled.
- **Text caching** has both halves ([text-boundary.md](text-boundary.md)). Live:
  every canvas memoizes boundary results — shaped lines by (size, text), and
  canonical glyph data by (font name, glyph id): outline curves + ink bounds, or
  a color glyph's fixed-size capture — checked before Core Text is called, so
  repeated strings shape once and repeated glyphs fetch (or rasterize) once
  (`test_textcache`, `test_emoji`). Serialized: a recorded canvas program
  carries that derived data inline (`font`/`glyph`/`bitmap`/`shape` blocks), so
  it replays byte-identically — pixels and measureText — with *no text boundary
  at all*, fonts installed or not, emoji included (`test_record_text`). The mip
  pyramid each emoji draw samples is derived checked-C data, deliberately not
  serialized. The record/replay format now covers **every pixel-affecting
  public op** (image sources ride deduplicated `image` blocks, Path2D draws
  ride `path` blocks, and the scalar ops — conic gradients, `roundRect` radii,
  smoothing, the filter list, reset/resize — are plain op lines; pure queries
  move no pixels and stay out), and the cross-machine claim is *gated* over the
  whole gallery: all 34 scenes commit a self-contained `.canvas` program next
  to their PNG, and `test_replay_gallery` replays each one and proves it
  reproduces the committed PNG byte-for-byte with zero shape/glyph boundary
  misses — so on the fontless CI runner (no Libian TC; it's download-on-demand,
  which is why `gate.yml`'s byte gate covers only the text-free scenes) a
  replay that matched a text scene's PNG used the embedded blocks, not host
  fonts.
- **Shadows** are cast from fills, strokes, text, and `drawImage` — the op's
  coverage is blurred (a CPU box-blur, three passes ≈ Gaussian,
  [blur.c](../src/blur.c)), tinted, offset, and composited under the shape, all in
  checked C. The blur approximates the
  spec's Gaussian; the offset is rounded to whole device pixels; and the shadow's
  silhouette is the op's coverage (exact for opaque paint/images, an
  approximation for semi-transparent gradient/pattern alpha or a sprite's own
  alpha shape).
- **`filter`** is functionally complete — the eight colour functions
  (`brightness`, `contrast`, `grayscale`, `hue-rotate`, `invert`, `opacity`,
  `saturate`, `sepia`), `blur()`, and `drop-shadow()` (both three box passes ≈
  the spec's Gaussian, like shadows), applied in list order to every painted
  op. What keeps it in this section is the interface: it's a typed API
  (`canvas_add_filter_*`), and the CSS string form (`ctx.filter =
  "grayscale(1) blur(2px)"`) is deliberately not parsed — string parsing has
  nothing for `-fbounds-safety` to say, the same call as `Path2D`'s SVG
  path-data strings above. (The spec's other `filter` form, `url()` into an
  SVG filter element, has nothing to reference headless — the out-of-scope
  list.) `drop-shadow()`'s offsets also round to whole device pixels, the
  shadowOffset deviation above.

## Missing entirely

- Nothing at the moment: every remaining gap is a narrower-than-spec row above
  or out of scope below.

## Out of scope for a headless renderer

Listed for completeness; these have no meaning without a document/host, so we don't
intend to implement them:

- `drawFocusIfNeeded` — accessibility / DOM focus. (Its old sibling
  `scrollPathIntoView` has since been removed from the spec entirely, so it no
  longer counts against anyone.)
- Context attributes and the context-loss machinery, each for its own reason:
  - `alpha: false` exists so a page compositor can skip blending the element
    over the page; headless, an opaque canvas is just a background fill.
  - `desynchronized` requests out-of-band presentation (lower input-to-pixel
    latency); there is no display pipeline here to desynchronize from.
  - `willReadFrequently` hints the backing store be kept CPU-side so
    `getImageData` doesn't stall on a GPU readback — a CPU-only renderer
    satisfies it by construction.
  - `colorSpace` would be inert metadata: with no CSS color strings and no
    tagged image sources, there is no conversion boundary for it to act on —
    the typed API's floats mean whatever the caller says they mean. It would
    acquire meaning only alongside the string/tagged features deliberately
    skipped above (CSS colors, a general image decoder, tagged PNG output).
  - `colorType` (`unorm8` | `float16`, a recent spec addition) is pinned by
    design: storage is `_Float16` that round-trips every 8-bit edge exactly
    and is clamped to the premultiplied range like `unorm8`
    ([decisions/float16-color-type.md](decisions/float16-color-type.md),
    [decisions/color-axis.md](decisions/color-axis.md)) — in effect a
    higher-precision `unorm8`, neither switchable nor extended-range.
  - Context loss is GPU-process death; a CPU renderer has nothing to lose
    (`canvas_is_context_lost` honestly returns false).
- Non-buffer `drawImage` sources tied to the DOM (`HTMLVideoElement`, `VideoFrame`,
  `ImageBitmap`, …). "Canvas-as-source" is the one genuine gap here, noted above.
