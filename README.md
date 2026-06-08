# canvas2d

A C23 implementation of (a growing subset of) the HTML **Canvas 2D API**,
rasterised on the GPU via **Metal**, built with **ninja**.

The point of the project is twofold:

1. **Learn `-fbounds-safety`** — Clang's spatial-memory-safety extension — by
   building something real with it.
2. **Show that C can play with the modern big boys (Rust).** The whole codebase
   compiles under `-std=c23 -fbounds-safety -Werror -Weverything` with only five
   warnings disabled (each documented), and the interesting work — path math,
   curve flattening, tessellation, stroking, a PNG encoder — lives in
   bounds-checked C. Metal is just a triangle rasteriser.

If you want the reflective version — what worked, what fought back, what we'd do
differently — read **[docs/bounds-safety.md](docs/bounds-safety.md)**.

## Quick start

```sh
python3 configure.py     # generate build.ninja
ninja                    # build the release + debug variants
ninja test               # build + run every test in both variants
ninja benchcmp           # hyperfine: release vs unsafe (cost of -fbounds-safety)
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
   canvas.c  ── state stack, CTM, styles; orchestrates the pipeline
      │  │
      │  ├── cnvs_math     2x3 affine transforms
      │  ├── cnvs_path     subpath storage + adaptive Bézier/arc flattening
      │  ├── cnvs_tess     ear-clipping fill triangulation
      │  ├── cnvs_stroke   polyline → stroke triangles (bevel joins, butt caps)
      │  ├── cnvs_geom     growable vertex/int buffers
      │  └── cnvs_png      RGBA8 → PNG encoder (CRC32 + adler32 + stored zlib)
      │
      ▼   gpu.h  (C ABI: opaque gpu*, gpu_vert, gpu_rgba)
   metal_backend.m  ── the ONE unsafe boundary: device, pipelines, offscreen
                        RGBA8 target, draw, readback   (Objective-C + ARC)
```

Everything above `gpu.h` is pure C23 under `-fbounds-safety`. The
[Metal backend](src/metal_backend.m) is the single boundary to a system
framework. All transform and geometry math happens on the CPU in checked C and
bakes into pixel-space triangles, so the GPU stays a dumb triangle rasteriser
and the bounds-safety surface stays in C.

> The shim is the project's only translation unit *not* under `-fbounds-safety`:
> the flag is C-only and rejects Objective-C. That's sound because
> `__counted_by`/`__single` pointers share the plain-C-pointer ABI, so `gpu.h` is
> identical on both sides. See [docs/bounds-safety.md](docs/bounds-safety.md) for
> why we don't route around the limitation.

## Public API (subset of Canvas 2D, snake_case)

```c
canvas *cv = canvas_create(width, height);   // (write canvas *__single cv under -fbounds-safety)
canvas_save / canvas_restore
canvas_translate / scale / rotate / transform / set_transform / reset_transform
canvas_set_fill_rgba / set_stroke_rgba / set_global_alpha / set_line_width
canvas_clear_rect / fill_rect
canvas_begin_path / move_to / line_to / rect / quadratic_curve_to /
    bezier_curve_to / arc / close_path
canvas_fill / canvas_stroke
canvas_read_rgba / canvas_write_png
canvas_destroy(cv);
```

Coordinates are pixels, origin top-left, +y down — matching the web platform.

## Capabilities and limitations

| Area | Status |
|---|---|
| Transforms, save/restore, alpha blending | ✅ |
| `fill_rect` / `clear_rect`, solid fills, PNG export | ✅ |
| Paths: lines, rects, quadratic/cubic Béziers, arcs | ✅ |
| `fill()` — winding rules (nonzero + even-odd), holes, self-intersection | ✅ scanline rasterizer |
| `stroke()` — width (CTM-scaled), bevel joins, butt caps | ✅ |
| Miter / round joins, round caps | ❌ bevel + butt only |
| Anti-aliasing | ❌ hard edges (MSAA planned) |
| Gradients, clipping, images, text | ❌ not yet |
| Batched GPU submission | ❌ one command buffer per draw (correctness first) |

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
| `bench_flatten` — cubic-Bézier flattening | 118 ms | 109 ms | **1.08×** |
| `bench_stroke` — stroke expansion | 44 ms | 36 ms | **1.22×** |
| `bench_png` — PNG encode (per-byte cursor + CRC/Adler) | 108 ms | 86 ms | **1.25×** |
| `bench_fill` — scanline fill (edge gather, crossing sort, winding walk) | 121 ms | 74 ms | **1.63×** |
| `bench` — end-to-end | 194 ms | 154 ms | **1.26×** |

The spread is the point: the scanline fill — wall-to-wall checked indexing (edge
gather, per-row crossing sort, winding walk, span emission) — pays the most at
**63%**, while flattening (lots of float math between a few indexed writes) is
nearly free at **8%**. The end-to-end 1.26× is a blend that, on its own, would
mask both. Real canvas rendering is GPU-bound,
so the end-to-end cost of safety is smaller still — but these are the honest prices
on the hottest pure-C kernels, one command to re-measure.

## Roadmap

- ~~Blanket `-fbounds-safety` including the Metal shim~~ — investigated and
  rejected: the flag is C-only, and routing Metal through the Objective-C runtime
  forces an all-`__unsafe_indexable` TU (no ARC, no real checking). The boundary
  stays an isolated ObjC shim; see [docs/bounds-safety.md](docs/bounds-safety.md).
- ~~A `release`-vs-`unsafe` benchmark for the cost of `-fbounds-safety`~~ — done
  (`ninja benchcmp`); see above.
- ~~Winding-rule fills (holes, self-intersection)~~ — done; scanline rasterizer
  ([cnvs_fill.c](src/cnvs_fill.c)) with nonzero + even-odd.
- Anti-aliasing (MSAA); miter/round joins; batched GPU submission.
- Gradients, clipping, then images/text.

## Layout

```
configure.py            generates build.ninja (release, debug, unsafe)
include/canvas.h         public API
src/                     C core + the ObjC Metal shim
shaders/canvas.metal     vertex+fragment shaders (embedded via #embed)
tests/                   unit + GPU pixel tests + a bounds-safety trap test
bench/bench.c            CPU-only benchmark (release vs unsafe)
docs/bounds-safety.md    the write-up
```
