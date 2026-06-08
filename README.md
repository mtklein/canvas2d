# canvas2d

A C23 implementation of (a growing subset of) the HTML **Canvas 2D API**,
antialiased in C and composited on the GPU via **Metal**, built with **ninja**.

The point of the project is twofold:

1. **Learn `-fbounds-safety`** — Clang's spatial-memory-safety extension — by
   building something real with it.
2. **Show that C can play with the modern big boys (Rust).** The whole codebase
   compiles under `-std=c23 -fbounds-safety -Werror -Weverything` with only five
   warnings disabled (each documented), and the interesting work — path math,
   curve flattening, analytic-coverage antialiasing, stroking, gradients, a PNG
   encoder — lives in bounds-checked C. Metal is just a tile compositor.

If you want the reflective version — what worked, what fought back, what we'd do
differently — read **[docs/bounds-safety.md](docs/bounds-safety.md)**.

## Gallery

Every image below is rendered by the C core, composited on the GPU, and written by the in-tree
PNG encoder ([examples/gallery.c](examples/gallery.c)); regenerate with `ninja images`.

Transforms, `save`/`restore`, global alpha, filled Béziers and arcs, strokes:

![shapes](gallery/shapes.png)

Winding rules — a donut (nonzero hole), then a self-intersecting pentagram filled
nonzero (solid centre) vs even-odd (hollow centre):

![winding](gallery/winding.png)

Line dashing — `setLineDash` patterns and a dashed arc:

![dashes](gallery/dashes.png)

Line joins (miter / round / bevel) and caps (butt / round / square):

![joins](gallery/joins.png)

Path primitives — a filled ellipse and a rounded rectangle (filled + outlined):

![paths](gallery/paths.png)

Clipping — a circular window, the intersection of two discs, and a
self-intersecting star, each masking the same flood of stripes (coverage mask):

![clip](gallery/clip.png)

Gradients — a diagonal linear fill (outlined with a cyan→yellow gradient
*stroke*), an off-centre radial "sphere", and a multi-stop rainbow ramp
(evaluated exactly per pixel on the CPU):

![gradients](gallery/gradients.png)

Batching — 320 translucent discs, each its own `fill()`, all submitted in a
single compositor command buffer (the alpha overlap shows ordering is preserved):

![batch](gallery/batch.png)

`drawImage` — a 16×16 source drawn 1:1 (crisp), scaled up (bilinear smoothing),
and scaled + rotated (AA quad edges from the coverage rasterizer):

![drawimage](gallery/drawimage.png)

`getImageData` captures the leftmost motif; `putImageData` stamps the copies:

![imagedata](gallery/imagedata.png)

## Quick start

```sh
python3 configure.py     # generate build.ninja
ninja                    # build the release + debug variants
ninja test               # build + run every test in both variants
ninja benchcmp           # hyperfine: release vs unsafe (cost of -fbounds-safety)
ninja images             # regenerate the gallery/*.png shown above
```

Requirements: macOS with Xcode (Apple clang 21+, which has `-fbounds-safety`,
`#embed`, and a Metal device), and ninja. `ninja benchcmp` also needs
[hyperfine](https://github.com/sharkdp/hyperfine). No offline Metal toolchain
component is needed — the shader is embedded with `#embed` and compiled at runtime.

Three variants are produced from one source tree:

| Variant | Flags | Story |
|---|---|---|
| `release` | `-Os -fbounds-safety` | the shipping build; bounds checks still trap |
| `debug` | `-O0 -g -fbounds-safety -fsanitize=address,integer,undefined -fno-sanitize-recover=all` | any sanitizer finding is fatal |
| `unsafe` | `-Os` | identical to release minus `-fbounds-safety`; the benchmark baseline |

`ninja test` runs all test binaries in `release` and `debug`. The PNGs land in
`build/` (e.g. `build/m1_demo.png`).

## Architecture

```
        public API (include/canvas.h)
                  │
   canvas.c  ── state stack, CTM, styles; rasterizes coverage, builds tiles
      │  │
      │  ├── cnvs_math     2x3 affine transforms
      │  ├── cnvs_path     subpath storage + adaptive Bézier/arc flattening
      │  ├── cnvs_cover     analytic (signed-area) coverage → per-pixel alpha
      │  ├── cnvs_gradient linear/radial ramp, evaluated per pixel into a tile
      │  ├── cnvs_stroke   polyline → stroke triangles (joins, caps, dashes)
      │  ├── cnvs_image    clipped 2D RGBA8 blits (get/putImageData)
      │  ├── cnvs_geom     growable vertex/int buffers
      │  └── cnvs_png      RGBA8 → PNG encoder (CRC32 + adler32 + stored zlib)
      │
      ▼   compositor.h  (C ABI: opaque compositor*, RGBA16F tiles + a clip mask)
   compositor_metal.m  ── the ONE unsafe boundary: blend / replace / clear of
                          tiles onto a single-sample target, masked by a clip
                          coverage texture, batched + read back  (ObjC + ARC)
```

Everything above `compositor.h` is pure C23 under `-fbounds-safety`. The
[Metal compositor](src/compositor_metal.m) is the single boundary to a system
framework — and it is *just* a compositor: all geometry, **analytic
antialiasing**, gradient evaluation, and clipping happen on the CPU in checked C
and bake into finished `_Float16` RGBA16F tiles (colour's lingua franca here —
native on this hardware, 8-bit only at the spec-mandated edges), so the GPU never
rasterizes or masks and the bounds-safety surface stays in C. Nothing in the ABI
is GPU-specific; a CPU backend could implement `compositor.h` identically.

> The shim is the project's only translation unit *not* under `-fbounds-safety`:
> the flag is C-only and rejects Objective-C. That's sound because
> `__counted_by`/`__single` pointers share the plain-C-pointer ABI, so
> `compositor.h` is identical on both sides. See
> [docs/bounds-safety.md](docs/bounds-safety.md) for why we don't route around it.

## Public API (subset of Canvas 2D, snake_case)

```c
canvas *cv = canvas_create(width, height);   // (write canvas *__single cv under -fbounds-safety)
canvas_save / canvas_restore
canvas_translate / scale / rotate / transform / set_transform / reset_transform
canvas_set_fill_rgba / set_stroke_rgba / set_global_alpha / set_fill_rule
canvas_set_fill_linear_gradient / set_fill_radial_gradient / add_fill_color_stop
canvas_set_stroke_linear_gradient / set_stroke_radial_gradient / add_stroke_color_stop
canvas_set_line_width / set_line_join / set_line_cap / set_miter_limit
canvas_set_line_dash / set_line_dash_offset
canvas_clear_rect / fill_rect
canvas_begin_path / move_to / line_to / rect / quadratic_curve_to /
    bezier_curve_to / arc / ellipse / round_rect / arc_to / close_path
canvas_fill / canvas_stroke / canvas_clip
canvas_get_image_data / put_image_data / read_rgba / write_png
canvas_draw_image / draw_image_scaled / draw_image_subrect   // RGBA8 source
canvas_destroy(cv);
```

Coordinates are pixels, origin top-left, +y down — matching the web platform.

## Capabilities and limitations

| Area | Status |
|---|---|
| Transforms, save/restore, alpha blending | ✅ |
| `fill_rect` / `clear_rect`, solid fills, PNG export | ✅ |
| Paths: lines, rects, Béziers, arc, ellipse, roundRect, arcTo | ✅ |
| `fill()` — winding rules (nonzero + even-odd), holes, self-intersection | ✅ analytic coverage |
| `stroke()` — width (CTM-scaled), miter/round/bevel joins, butt/round/square caps, line dash | ✅ |
| `getImageData` / `putImageData` (clipped 2D blits) | ✅ |
| `clip()` — arbitrary paths, intersection, save/restore nesting | ✅ coverage mask |
| Gradients — linear + radial, fills *and* strokes, multi-stop | ✅ exact per-pixel |
| Anti-aliasing | ✅ analytic coverage, both axes (fills, strokes, clips) |
| `drawImage` — RGBA8 source, bilinear, transform/clip/alpha-aware | ✅ |
| Text | ❌ not yet |
| Batched compositor submission | ✅ consecutive ops share one command buffer |

## Warning policy

Built with `-Weverything -Werror`; only these are disabled, each with a one-line
rationale in [configure.py](configure.py):

- `-Wno-poison-system-directories` — env/cross-compile artifact, not our code
- `-Wno-declaration-after-statement` — we use C23 declare-at-use style
- `-Wno-padded` — struct padding isn't a correctness signal
- `-Wno-pre-c23-compat` — we deliberately target C23
- `-Wno-implicit-void-ptr-cast` — C-only project; the idiomatic `calloc` cast
  (it does not weaken `-fbounds-safety`, which still traps undersized allocs)

## Benchmarking — what does `-fbounds-safety` cost?

The natural question isn't "how fast vs. Rust" but "how much does the safety cost
*us*" — the same code, same `-Os`, with and without the flag. That's the `release`
vs `unsafe` comparison:

```sh
ninja benchcmp     # hyperfine: each phase + e2e, release vs unsafe
```

The hot paths are benchmarked **in isolation** ([bench/](bench/)) so a slow phase
can't hide a regression in a faster one, plus an end-to-end run. All are CPU-only
(no GPU). A recent run on an Apple Silicon laptop:

| Phase | `release` (checked) | `unsafe` | overhead |
|---|---|---|---|
| `bench_gradient` — gradient eval (radial solve + multi-stop ramp lookup) | 82 ms | 82 ms | **1.00×** |
| `bench_flatten` — cubic-Bézier flattening | 118 ms | 110 ms | **1.07×** |
| `bench_png` — PNG encode (memcpy copies + SIMD adler32 + CRC) | 44 ms | 41 ms | **1.08×** |
| `bench` — end-to-end | 84 ms | 76 ms | **1.10×** |
| `bench_stroke` — stroke expansion (joins/caps) | 53 ms | 48 ms | **1.11×** |
| `bench_fill` — analytic coverage fill (signed-area accumulate + resolve) | 27 ms | 24 ms | **1.16×** |
| `bench_blit` — clipped 2D RGBA8 blit (getImageData copy) | 125 ms | 49 ms | **2.55×** |

The spread is the point: the 2D blit — four bounds-checked byte loads and stores
per pixel across two buffers, with no arithmetic to amortize them — pays the most
at **~2.5×**, while gradient evaluation and flattening (lots of float math between
a few indexed reads) are essentially free (**0–7%**).

These numbers are post-profiling: a `sample` of the e2e run (see
[docs/bounds-safety.md](docs/bounds-safety.md)) showed PNG encoding dominated by
byte-at-a-time copies and adler32's per-byte `% 65521`. Switching the copies to
`memcpy` and vectorizing adler32 with `ext_vector_type` cut `bench_png` from
~114 ms to ~44 ms **and** dropped its safety overhead from 1.27× to 1.08× — the
checks that hurt were per-byte, so amortizing the byte loops amortized the checks
too. (The bulk ops stay checked: the compiler treats `memcpy`/`memset` as
`__sized_by`, and a `memcpy`-spelled vector load is bounds-checked.) Real canvas
rendering is GPU-bound, so the end-to-end cost of safety is smaller still — but
these are the honest prices on the hottest pure-C kernels, one command to
re-measure.

## Roadmap

- ~~Blanket `-fbounds-safety` including the Metal shim~~ — investigated and
  rejected: the flag is C-only, and routing Metal through the Objective-C runtime
  forces an all-`__unsafe_indexable` TU (no ARC, no real checking). The boundary
  stays an isolated ObjC shim; see [docs/bounds-safety.md](docs/bounds-safety.md).
- ~~A `release`-vs-`unsafe` benchmark for the cost of `-fbounds-safety`~~ — done
  (`ninja benchcmp`); see above.
- ~~Winding-rule fills (holes, self-intersection)~~ — done; nonzero + even-odd,
  now via the analytic coverage rasterizer ([cnvs_cover.c](src/cnvs_cover.c)).
- ~~`getImageData` / `putImageData`~~ — done; clipped 2D blits
  ([cnvs_image.c](src/cnvs_image.c)).
- ~~`clip()`~~ — done; a per-pixel coverage mask the compositor multiplies into
  every blend ([canvas.c](src/canvas.c) `canvas_clip`).
- ~~Gradients~~ — done; linear + radial, multi-stop, evaluated exactly per pixel
  into the tile ([cnvs_gradient.c](src/cnvs_gradient.c)).
- ~~Batched compositor submission~~ — done; consecutive ops share one command
  buffer, flushed only at a readback or clip change
  ([compositor_metal.m](src/compositor_metal.m), `open_batch`/`flush_batch`).
- ~~Anti-aliasing~~ — done, and the big one: analytic (signed-area) coverage in
  checked C ([cnvs_cover.c](src/cnvs_cover.c)) antialiases fills, strokes, and
  clip edges in **both** axes; the GPU is now a pure tile compositor with no
  MSAA. See the AA discussion in [docs/bounds-safety.md](docs/bounds-safety.md).
- ~~`drawImage`~~ — done; bilinear-sampled RGBA8 source, transformed/clipped/
  alpha-composited through the coverage pipeline ([canvas.c](src/canvas.c)
  `canvas_draw_image*`).
- Text.

## Layout

```
configure.py             generates build.ninja (release, debug, unsafe)
include/canvas.h         public API
src/                     C core + the ObjC Metal compositor shim
shaders/compositor.metal tile vertex+fragment shaders (embedded via #embed)
tests/                   unit + pixel tests + a bounds-safety trap test
bench/bench.c            CPU-only benchmark (release vs unsafe)
examples/gallery.c       renders the gallery PNGs (ninja images)
gallery/                 committed showcase PNGs
docs/bounds-safety.md    the write-up
```
