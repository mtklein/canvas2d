# Data-dependent gather under `-fbounds-safety`: a 256-entry LUT

A per-pixel lookup table — gamma, sRGB, a levels/curve adjustment — is the canonical
*data-dependent gather*: the index into the table comes from the pixel, not the loop.
That is the interesting case for `-fbounds-safety`, because the compiler can't bound a
data-dependent index the way it bounds a loop counter. NEON has no hardware
scatter/gather (unlike AVX2's `vgather`), so the vector path uses `tbl` — a
register-resident byte-table lookup.

[../src/lut.c](../src/lut.c) applies a 256-entry 8-bit LUT two ways:

- `lut_apply_mem`: scalar, `px[i] = lut[px[i]]`.
- `lut_apply_neon`: `vqtbl4q_u8`. The 256-entry table is loaded once into four
  64-byte register tables; each `tbl` returns `lut[idx]` for indices in its own
  64-wide range and 0 elsewhere (an out-of-range index yields 0), so OR-combining the
  four reconstructs `lut[idx]` for every `idx` in 0..255 — the whole gather lives in
  registers.

## The memory LUT is checked per element — the index range isn't folded

`lut[px[i]]` indexes a **compile-time-256** table with a `uint8_t`, so the index is
provably 0..255 < 256 and the check can never fail. `-fbounds-safety` emits it anyway:

```c
ldrb w10,[x0]        ; px[i]  (a uint8, so 0..255)
add  x10,x2,x10      ; lut + px[i]
cmp  x10,x8          ; vs lut+256
ccmp x10,x2          ; vs lut
b.lo trap            ; check 0 <= px[i] < 256  -- always true
```

The pass doesn't use the index's integer type range to elide the bound, so a
data-dependent LUT keeps one per-element check. A range-analysis opportunity left on
the table — but in practice it's cheap here (below).

## `vtbl` gathers from registers, so there's no per-lane check

`vqtbl4q_u8` reads the table out of registers; there is no per-lane *memory* index to
bound. Only the 16-wide block load/store stay checked (one `memcpy` bound each,
amortized over 16 pixels). It's the same move that made the register pipeline free in
[pixel-pipelines.md](pixel-pipelines.md): **put the indexed data in registers and the
per-element checks vanish.**

## Cost

1M pixels, `-Os`, release (`-fbounds-safety`) vs unsafe, `A`-vs-`A` 1.00×:

| LUT | bounds-safe | unsafe | cost of checks | vs scalar |
|---|---|---|---|---|
| scalar memory | 59 ms | 56 ms | **1.05×** | 1.0× |
| NEON `vqtbl4q_u8` | 20 ms | 18 ms | **1.10×** | **~3× faster** |

Two things worth noting:

- The scalar per-element check costs only ~5%, not the dramatic tax its presence might
  suggest. The LUT load is a dependent memory access (the address depends on `px[i]`),
  so it's latency-bound, and the predictable check branch issues in that shadow — the
  same "stalls hide the checks" effect the [vertical blur](stencil-blur.md) showed.
- The big win is `vtbl`'s ~3× throughput (16 lanes per `tbl`), and it carries no
  per-lane check on top. The remaining ~10% is the block `memcpy` bounds checks.

## Notes

- `arm_neon.h` composes with `-std=c23 -fbounds-safety -Weverything` with no friction —
  the NEON intrinsics and types are usable from checked code.
- `vtbl` tables are small (`vtbl1`/`vtbl4` reach 16/64 bytes). A 256-entry LUT needs
  the four-table + OR scheme above; palettes and coarse curves that fit 64 bytes are a
  single `tbl`.
- A `__counted_by` *local* can't reference a function **parameter** as its count
  (`argument of '__counted_by' attribute cannot refer to declaration of a different
  lifetime`) — `test_lut` allocates scratch with `int const len = n;` first. The
  reflex `T *__counted_by(n) p = malloc(...)` with a parameter `n` does not compile.
