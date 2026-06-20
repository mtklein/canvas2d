# Canvas 2D coverage: the gap list

The README's capability table shows what works. This is the complement: a sweep
of the full [`CanvasRenderingContext2D`](https://html.spec.whatwg.org/multipage/canvas.html#canvasrenderingcontext2d)
surface against what is implemented, splitting **partial** (implemented but
narrower than spec) from **missing entirely**, plus the items out of scope for a
headless C library. (Last swept against the living spec: June 2026 — the sweep
that caught `lang`, `colorType`/`pixelFormat`, and `scrollPathIntoView`'s
removal; updated when colour management landed.)

The project is a vehicle for learning `-fbounds-safety`, not for spec
completeness, so the selection is biased: features whose hot path is
*indexed-buffer-dense*, where bounds checking applies, are prioritized over
features that are mostly plumbing or string parsing.

## Near-term plan

Chosen because their kernels are dense, per-pixel, indexed-buffer work and good
SIMD targets:

1. ~~**`globalCompositeOperation`**~~ — **done** (all 26 modes), as the
   checked-C blend kernels (item 2). (A since-removed Metal backend ran
   the same math as fixed-function source-over plus a framebuffer-fetch shader; see
   [decisions/metal-backend.md](decisions/metal-backend.md).)
2. ~~**A software blend stage**~~ — **done**, and blending is now a stage of
   [canvas.c](../src/canvas.c) itself, not a backend: the per-pixel
   `blend8(src, dst, mode)`
   kernel is all 26 composite/blend modes, premultiplied, over `__counted_by`
   tiles, ~350 lines of checked C compositing onto the canvas's own RGBA16F
   target. It began behind a compositor ABI selected against a Metal GPU
   backend at build time and held
   bit-for-bit identical to it by a tolerance-0 differential; once measurements showed
   the CPU path faster on the flagship workload, Metal was removed, the GPU-parity
   rounding it required was dropped, and the compositor object itself was later
   dissolved into canvas.c (see
   [decisions/backend-differential.md](decisions/backend-differential.md) and
   [decisions/metal-backend.md](decisions/metal-backend.md)).
3. ~~**Shadows**~~ — **done** (fills/strokes/text/images). The op's source
   alpha — the spec's "render the shadow from image A", post-filter — is
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
6. ~~**`filter` `drop-shadow()`**~~ — **done**; with it the filter list is
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
- The internal colour type — **settled**. A review kept `_Float16` storage and
  corrected its rationale (it round-trips the spec's 8-bit edges exactly where u8
  corrupts ~half of them) — see
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

- **Text styling** covers `set_font_size`, `font-family` (settable, default
  Libian TC), `font-weight` (CSS 100..900) and `font-style` (italic) -- the
  latter two resolved through a Core Text trait descriptor, which matches a real
  bold/italic face or synthesizes an oblique when the family has none --
  `letterSpacing`, `wordSpacing`, `textAlign` (start/end resolving against
  direction), `textBaseline`, and `direction` (ltr/rtl, the bidi paragraph base
  -- it reorders mixed-direction text, resolves neutrals, and rides the
  shape-block cache key through record/replay). Family, weight, and style join
  the shaping cache key and the glyph-cache identity (so a synthesized
  bold/italic never aliases the regular face) and serialize through
  record/replay. No CSS `font` shorthand, and none of `lang` (a recent spec
  addition), `fontKerning`, `fontStretch`, `fontVariantCaps`, `textRendering`.
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
- **PNG I/O**: `canvas_write_png` writes a BT.2100 PNG — 16-bit, Rec.2020
  primaries, PQ (ST 2084) transfer, signalled by a `cICP` chunk — with real
  compression (Up-filtered rows + the in-house `cnvs_zlib` deflate; Up-only
  because it vectorizes as whole-row ops with no left-neighbor recurrence). The
  surface is transformed into that encoding on the way out regardless of working
  space, so wide-gamut and HDR values from a linear canvas carry through. Output
  only: there is no PNG decoder (nothing reads PNGs back — image input is raw
  bitmaps and the `.canvas` format), and no `toDataURL`/`toBlob` string forms.
- **`Path2D`** has no SVG path-data string constructor (string parsing); the
  constructible object, `add_path`, the builders, and the
  fill/stroke/clip/isPointIn* overloads are all supported.
- **`fillStyle`/`strokeStyle`** are float RGBA in an explicit colour space
  (sRGB / extended-linear-sRGB / Oklab — not CSS color strings),
  linear/radial/conic gradients, and image patterns. Gradients and patterns
  are state setters (the pattern image is borrowed), not reusable first-class
  objects, so `CanvasPattern.setTransform` has no analogue: a pattern is pinned
  to the CTM in effect when it is set.
- **Colour management** — the canvas has a working colour space, `sRGB`
  (default, gamma-encoded) or `extended-linear-sRGB`, fixed at creation
  (`canvas_in_space`); compositing runs in that space, so linear-light blending
  is available. Every colour the API takes or returns names its space ({sRGB,
  extended-linear-sRGB, Oklab}); untagged input is sRGB (the legacy spelling)
  and the tagged forms convert at the boundary. Gradients interpolate in a
  chosen space (sRGB / linear / Oklab) and alpha mode (unpremultiplied /
  premultiplied), set independently. Conversions are the sRGB transfer, a
  linear-sRGB↔Oklab pair, and the Rec.2020 matrix + PQ transfer the PNG output
  uses ([cnvs_color.c](../src/cnvs_color.c)). Image sources are sampled in their
  own tagged space and the resolved sample converts to the working space on
  deposit (the image format governs filtering, the canvas governs compositing).
  One deliberate limit: **compositing uses sRGB primaries only** (no Display-P3 /
  Rec.2020 working space — a single linear hub covers the current effects;
  parameterized primaries deferred).
- **`getImageData`** returns RGBA8 in a chosen colour space (sRGB /
  extended-linear-sRGB / Oklab); no `pixelFormat` (`rgba-float16`, the
  `Float16Array`-backed `ImageData`).
- **`drawImage`** sources borrowed RGBA8 bitmaps (`canvas_draw_bitmap*`) and
  reified images (`canvas_draw_image*`); image storage is {unorm8, f16} ×
  {unpremultiplied, premultiplied}, each carrying a colour-space tag.
  `canvas_snapshot` is canvas-as-source: the surface is premultiplied f16 and
  so is the snapshot, one memcpy.
  `imageSmoothingQuality` is live — `low` samples bilinearly
  (nearest-neighbour when smoothing is disabled), `medium`/`high` antialias
  minification through a premultiplied mip chain with trilinear filtering
  (an image's chain caches via the explicit `canvas_image_build_mips`, the
  user deciding when to pay; a borrowed bitmap has no identity to cache
  against, so its chain rebuilds per minifying draw; a mip-less image
  deliberately falls back to bilinear), and `high` magnifies through a 4×4
  Catmull-Rom (premultiplied taps, the BC-spline pair one swappable line in
  canvas.c).  Sources are RGBA8 only and DOM source kinds are out of scope
  below; an image source is sampled in its own tagged space and the resolved
  sample converts to the working space on deposit (the colour-management row
  above) -- filtering stays in the source's space by design.
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
  whole gallery: every scene commits a self-contained `.canvas` program next
  to their PNG, and `test_replay_gallery` replays each one and proves it
  reproduces the committed PNG byte-for-byte with zero shape/glyph boundary
  misses — so on the fontless CI runner (no Libian TC; it's download-on-demand,
  which is why `gate.yml`'s byte gate covers only the text-free scenes) a
  replay that matched a text scene's PNG used the embedded blocks, not host
  fonts.
- **Shadows** are cast from fills, strokes, text, and `drawImage` — the op's
  source alpha (the spec's "render the shadow from image A": paint alpha,
  coverage, global alpha, and any filters all shape it — a transparent sprite
  shadows its alpha shape, not its quad) is blurred (a CPU box-blur, three
  passes ≈ Gaussian, [blur.c](../src/blur.c)), tinted, offset, and composited
  under the shape, all in checked C. Offsets honour subpixel fractions on a
  1/256th-px grid (one 2-tap lerp pass per axis after the blur — translation
  in its convolution form). One deviation remains: the blur approximates the
  spec's Gaussian with three box passes.
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
  list.) `drop-shadow()`'s offsets ride the same 1/256th-px subpixel grid as
  `shadowOffset{X,Y}` (a bilinear read of the source alpha).

## Missing entirely

- Nothing at the moment: every remaining gap is a narrower-than-spec row above
  or out of scope below.

## Out of scope for a headless renderer

Listed for completeness; most have no meaning without a document/host:

- `drawFocusIfNeeded` — accessibility / DOM focus. (Its old sibling
  `scrollPathIntoView` has since been removed from the spec entirely.)
- Context attributes and the context-loss machinery, each for its own reason:
  - `alpha: false` exists so a page compositor can skip blending the element
    over the page; headless, an opaque canvas is just a background fill.
  - `desynchronized` requests out-of-band presentation (lower input-to-pixel
    latency); there is no display pipeline here to desynchronize from.
  - `willReadFrequently` hints the backing store be kept CPU-side so
    `getImageData` doesn't stall on a GPU readback — a CPU-only renderer
    satisfies it by construction.
  - `colorSpace` is implemented — the canvas working space (`canvas_in_space`)
    plus the tagged colour API (the colour-management row above). Only the
    spec's string-keyed `PredefinedColorSpace` spelling and wide-gamut
    primaries (`display-p3`) are unimplemented.
  - `colorType` (`unorm8` | `float16` output bitmap): the surface is `_Float16`
    and an extended-linear working space carries extended range, but readback
    and `getImageData` produce `unorm8` — a `float16` output buffer is not
    offered ([decisions/float16-color-type.md](decisions/float16-color-type.md),
    [decisions/color-axis.md](decisions/color-axis.md)).
  - Context loss is GPU-process death; a CPU renderer has nothing to lose
    (`canvas_is_context_lost` honestly returns false).
- Non-buffer `drawImage` sources tied to the DOM (`HTMLVideoElement`, `VideoFrame`,
  `ImageBitmap`, …). Canvas-as-source, once the one genuine gap here, is now
  `canvas_snapshot` (the partial row above).
