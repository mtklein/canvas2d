# Sparse (RLE) coverage — a considered future direction

Status: **not implemented, intentionally.** This records the analysis so a future
pass doesn't have to rediscover it.

## Today: dense coverage

The rasterizer produces an 8-bit coverage value for every pixel of a fill's
bounding box ([cnvs_cover.c](../src/cnvs_cover.c)): `add_edge` accumulates signed
area into a dense `w*h` float buffer, `resolve` turns it into a dense `w*h` byte
mask, [paint_tile](../src/canvas.c) shades that into a dense premultiplied tile, and
the compositor blends the whole tile. Clip masks are dense coverage too, held by
value in every saved state.

## The case for sparse

Measured coverage histogram over a fill's bbox (200 shapes each):

| content | zero-coverage | fractional (AA) | full (255) |
|---|---|---|---|
| concave stars | **59%** | 8.5% | 32.5% |
| convex polys | **39%** | 4.4% | 56% |

So 39–59% of every fill's bbox is transparent, and the dense pipeline still
resolves, shades, and blends all of it. A run-length representation (per row: a list
of `start,len,coverage` spans, à la Skia's `SkMask::kRLE_Format` / `SkAlphaRuns`)
would simply omit the zero runs and collapse each saturated interior to one span,
skipping that work across resolve → paint → blit.

## Why we didn't do it

1. **It's content-dependent, not a strict win** — exactly what our run-aware
   `resolve` experiment showed: specializing runs sped up solid/convex fills ~1.14×
   but *regressed* spiky/concave ones ~1.17×, because concave shapes have many short
   runs and the per-run overhead exceeds the per-pixel work it skips. This is why
   Skia keeps *both* a dense (`kA8`) and an RLE format and chooses per-mask — so RLE
   here means a format **plus a chooser heuristic**, not a replacement.

2. **Vectorizing the dense path shrank RLE's payoff.** RLE's win is "skip the
   exterior," but the exterior is now cheap: `resolve`'s convert is 8-wide vectorized
   (zeros cost almost nothing) and `paint_tile` solves the gradient parameter a whole
   row at a time. The waste that used to sit in scalar hotspots is now spread thinly
   across vectorized passes, so the absolute headroom is far less than the 39–59%
   sparsity suggests.

3. **The dense tile is the compositor boundary.** Geometry/AA happen on the CPU and
   bake into a dense premultiplied tile; the GPU (Metal) blends every tile pixel by
   design ("the GPU just composites"). RLE can't reach the GPU blend without
   reworking that interface, so it would only help the CPU-side stages.

## Where it *would* pay off (if revisited)

- **Clip masks (highest value, lowest friction).** A sparse/RLE clip — Skia's
  `SkAAClip` — is a well-known win for complex clip regions, shrinks the per-state
  memory the clip is copied with, and sidesteps the dense-tile boundary entirely
  (clip intersection is CPU-only). This is the slice to prototype first.
- **Very sparse content** — text runs, thin strokes, intricate paths — where the
  bbox is mostly empty and runs are long enough to amortize the per-run overhead.
- **The software compositor blend** ([compositor_cpu.c](../src/compositor_cpu.c))
  skipping fully-transparent source runs (the GPU path can't).

## Bottom line

Keep the dense + SIMD path as the default; it's a good local optimum for typical
solid/convex fills. Treat RLE as a targeted tool for sparse/text/clip-heavy
workloads, starting with a sparse clip mask, and only behind a dense-vs-sparse
chooser — never as a wholesale replacement.
