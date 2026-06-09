# Canvas 2D coverage: the honest gap list

The README's capability table shows what works. This is the complement: a sweep of
the full [`CanvasRenderingContext2D`](https://html.spec.whatwg.org/multipage/canvas.html#canvasrenderingcontext2d)
surface against what we actually implement, splitting **partial** (we have it, but
narrower than spec) from **missing entirely**, plus the handful that are genuinely
out of scope for a headless C library.

This project isn't chasing spec completeness for its own sake — it's a vehicle for
learning `-fbounds-safety`. So the selection is biased: we prioritize features
whose hot path is *indexed-buffer-dense*, where bounds checking has something to
say, over features that are mostly plumbing or string parsing.

## Near-term plan

Chosen because their kernels are exactly the kind of dense, per-pixel,
indexed-buffer work `-fbounds-safety` is meant for (and good SIMD targets):

1. ~~**`globalCompositeOperation`**~~ — **done** (all 26 modes). It currently runs
   in the Metal backend: source-over on fixed-function blend, the rest in a
   framebuffer-fetch shader. The blend *math* isn't in checked C yet — that's the
   next item.
2. ~~**A software compositor backend**~~ — **done**.
   [compositor_cpu.c](../src/compositor_cpu.c) implements the same `compositor.h`
   ABI in ~100 lines of checked C; the per-pixel `cnvs_blend(src, dst, mode)`
   kernel ([cnvs_blend.h](../src/cnvs_blend.h)) is all 26 composite/blend modes,
   premultiplied, over `__counted_by` tiles. Chosen instead of Metal at build time
   (the `-cpu` variants), it links no GPU frameworks and cross-validates the Metal
   backend — every pixel test runs against both, and they agree to ≤1/255.
3. **Shadows / `filter` blur** — a separable Gaussian (or box) convolution: a
   horizontal then vertical pass over a coverage/colour buffer, every tap an
   indexed read against a `__counted_by` row. The canonical stencil loop.

Internals (not API features) considered and deferred: a sparse/RLE coverage format
to skip the transparent ~40–60% of a fill's bbox — analysis and why dense+SIMD stays
the default in [sparse-coverage.md](sparse-coverage.md).

## Partial — implemented but narrower than spec

- **`measureText`** returns only `.width`. The real `TextMetrics` also carries
  `actualBoundingBoxLeft/Right/Ascent/Descent`, `fontBoundingBoxAscent/Descent`,
  `emHeightAscent/Descent`, and the baseline offsets.
- **Text styling** is `set_font_size` only. No font-family selection (pinned to
  Libian TC), weight, style, or the CSS `font` shorthand; and none of
  `textAlign`, `textBaseline`, `direction`, `letterSpacing`, `wordSpacing`,
  `fontKerning`, `fontStretch`, `fontVariantCaps`, `textRendering`.
- **`fillText`/`strokeText`** take no `maxWidth` (no auto-condense).
- **Text shaping** maps code point → glyph 1:1: no ligatures, contextual forms, or
  bidi, and only the BMP path is exercised (CJK works; emoji/astral untested).
- **`roundRect`** takes a single uniform scalar radius; the spec allows 1–4 radii,
  per-corner, and elliptical (x/y) corners.
- **`fill`/`stroke`/`clip`** have no `Path2D` overload, and the fill rule is taken
  from state rather than an explicit argument.
- **`fillStyle`/`strokeStyle`** are solid colour (float RGBA, not CSS color
  strings) plus linear/radial gradients — and the gradients are state setters, not
  reusable first-class objects.
- **`putImageData`** has no dirty-rectangle overload (`dirtyX/Y/Width/Height`).
- **`getImageData`** is fixed RGBA8; no `colorSpace`/settings.
- **`drawImage`** sources only our packed RGBA8 buffer (no canvas/image-as-source),
  and always samples bilinearly.
- **Glyph outlines** are re-fetched from Core Text on every `fill_text` — no cache.

## Missing entirely

- **`strokeRect`** — we have `fillRect`/`clearRect` but not this one.
- **Shadows** — `shadowColor`, `shadowBlur`, `shadowOffsetX`, `shadowOffsetY`.
- **`filter`** — the CSS filter functions (`blur`, `drop-shadow`, `brightness`,
  `contrast`, `grayscale`, `hue-rotate`, `invert`, `opacity`, `saturate`, `sepia`).
- **`createPattern`** (image patterns + `repeat`/`repeat-x`/`repeat-y`/`no-repeat`)
  and **`createConicGradient`**.
- **`Path2D`** — no constructible path object, no `addPath`, no SVG path-data
  string, and none of the `fill/stroke/clip/isPointIn*` overloads that take one.
- **Hit testing** — `isPointInPath`, `isPointInStroke`.
- **State/readback** — `getTransform`, `getLineDash`, `reset`, `isContextLost`.
- **Image smoothing** — `imageSmoothingEnabled` (no nearest-neighbour) and
  `imageSmoothingQuality`.
- **`createImageData`** — allocate a blank ImageData.
- **Canvas resize** — width/height are fixed at create (the spec's resize also
  clears the bitmap and resets state).

## Out of scope for a headless renderer

Listed for completeness; these have no meaning without a document/host, so we don't
intend to implement them:

- `drawFocusIfNeeded`, `scrollPathIntoView` — accessibility / DOM focus.
- Context attributes (`alpha`, `desynchronized`, `willReadFrequently`,
  `colorSpace`) and the context-loss machinery.
- Non-buffer `drawImage` sources tied to the DOM (`HTMLVideoElement`, `VideoFrame`,
  `ImageBitmap`, …). "Canvas-as-source" is the one genuine gap here, noted above.
