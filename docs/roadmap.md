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
3. ~~**Shadows**~~ — **done** (fills/strokes/text). The op's coverage mask is
   blurred by [blur.c](../src/blur.c)'s separable box passes (the stencil-loop
   probe, three passes ≈ Gaussian), tinted, offset, and composited under the
   shape — all in checked C, so the backends stay bit-identical. `filter` blur
   (and the other CSS filter functions) reuse the same kernel and are next.

Internals (not API features) considered and deferred: a sparse/RLE coverage format
to skip the transparent ~40–60% of a fill's bbox — analysis and why dense+SIMD stays
the default in [sparse-coverage.md](sparse-coverage.md).

## Partial — implemented but narrower than spec

- **Text styling** covers `set_font_size`, `textAlign`, and `textBaseline`. No
  font-family selection (pinned to Libian TC), weight, style, or the CSS `font`
  shorthand; and none of `direction`, `letterSpacing`, `wordSpacing`,
  `fontKerning`, `fontStretch`, `fontVariantCaps`, `textRendering`.
- **Text shaping** maps code point → glyph 1:1: no ligatures, contextual forms, or
  bidi, and only the BMP path is exercised (CJK works; emoji/astral untested).
- **`fill`/`stroke`/`clip`** on the *current* path take the fill rule from state,
  not an explicit argument (the `Path2D` overloads — `fill_path`/`stroke_path`/
  `clip_path` — do take an explicit rule).
- **`Path2D`** has no SVG path-data string constructor (string parsing); the
  constructible object, `add_path`, the builders, and the
  fill/stroke/clip/isPointIn* overloads are all supported.
- **`fillStyle`/`strokeStyle`** are solid colour (float RGBA, not CSS color
  strings), linear/radial/conic gradients, and image patterns — and the
  gradients/patterns are state setters (the pattern image is borrowed), not
  reusable first-class objects.
- **`getImageData`** is fixed RGBA8; no `colorSpace`/settings.
- **`drawImage`** sources only our packed RGBA8 buffer (no canvas/image-as-source);
  it samples bilinearly, or nearest-neighbour when image smoothing is disabled.
- **Glyph outlines** are re-fetched from Core Text on every `fill_text` — no cache.
- **Shadows** are cast from fills, strokes, text, and `drawImage` — the op's
  coverage is blurred (a CPU box-blur, three passes ≈ Gaussian,
  [blur.c](../src/blur.c)), tinted, offset, and composited under the shape, all in
  checked C so the two backends stay bit-identical. The blur approximates the
  spec's Gaussian; the offset is rounded to whole device pixels; and the shadow's
  silhouette is the op's coverage (exact for opaque paint/images, an
  approximation for semi-transparent gradient/pattern alpha or a sprite's own
  alpha shape).

## Missing entirely

- **`filter`** — the CSS filter functions (`blur`, `drop-shadow`, `brightness`,
  `contrast`, `grayscale`, `hue-rotate`, `invert`, `opacity`, `saturate`, `sepia`).

## Out of scope for a headless renderer

Listed for completeness; these have no meaning without a document/host, so we don't
intend to implement them:

- `drawFocusIfNeeded`, `scrollPathIntoView` — accessibility / DOM focus.
- Context attributes (`alpha`, `desynchronized`, `willReadFrequently`,
  `colorSpace`) and the context-loss machinery.
- Non-buffer `drawImage` sources tied to the DOM (`HTMLVideoElement`, `VideoFrame`,
  `ImageBitmap`, …). "Canvas-as-source" is the one genuine gap here, noted above.
