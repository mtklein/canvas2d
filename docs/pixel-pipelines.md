# Flexible pixel processing under `-fbounds-safety`: three designs

A configurable per-pixel processor (the shape behind filters, blend chains, and
shader-like effects) can be built several ways. This is an exploration of three
dispatch styles, each judged on the same question the rest of the project asks:
where does `-fbounds-safety` help, and where does it fight back?

1. **One big switch** — a bytecode program; a `for pc { switch(op) }` loop runs it,
   `PIXVM_N` pixels per step. *(implemented)*
2. **Tail-call threaded VM** — same bytecode, but each opcode is a handler that does
   its work and `[[clang::musttail]]`-returns the next handler. *(planned)*
3. **SkRasterPipeline-style pipeline** — stages carry the current r,g,b,a (and x)
   as explicit vector arguments and tail-call the next stage, so the channels stay
   in registers across the whole program. *(planned)*

They share an ISA ([../src/pixvm.h](../src/pixvm.h)): a register file of
`PIXVM_N`-wide `float` vectors, RGBA8 load/store ops, and `splat`/`mov`/`add`/`sub`/
`mul`/`mad`. Designs 2 and 3 reuse the ISA and the test programs so the comparison
is apples-to-apples — same work, different dispatch.

## Design A — one big switch (implemented)

[../src/pixvm_switch.c](../src/pixvm_switch.c) is `pixvm_run_switch`: an outer loop
over `PIXVM_N`-pixel chunks, an inner loop over the program, a switch on the opcode.
Load/store ops deinterleave RGBA8 to/from four channel registers per lane;
arithmetic ops are whole-vector. [../tests/test_pixvm.c](../tests/test_pixvm.c)
checks a copy round-trip, a premultiplied source-over program, and the trap below.

### What's frictionless

Everything indexed. The program is a `pixop *__counted_by(prog_len)`, the register
file is a plain `pixv reg[PIXVM_REGS]` local, the pixel buffers are `__counted_by`
parameters — and the whole interpreter (program fetch, runtime-indexed register
reads/writes, per-lane pixel loads and stores) compiles under `-Weverything`
`-fbounds-safety` with zero annotation gymnastics. This is the indexed-buffer-dense
case the flag is built for, and a VM is nothing but indexed-buffer-dense.

### The safety payoff is real and exactly where you want it

A bytecode VM runs *data* as code: the register indices and pixel offsets come from
the program, not the compiler. Under `-fbounds-safety` those accesses are
bounds-checked at runtime, so a malformed program **traps instead of corrupting
memory**. `test_pixvm` proves it: a child process runs a one-instruction program
whose register operand is out of range (`STORE` reading `reg[20..23]` of a
16-register file, with the result written to `dst` so the read can't be optimized
away), and the parent asserts the child died from a signal. It does — in every
bounds-safe variant, including `-Os`.

The lane subscript inside a register (`reg[d][lane]`) needs no check: `lane` is
bounded by the chunk's active count, not supplied by the bytecode. The *register*
index `d` is the one that comes from data, and that's the one that's checked.

### The one friction: optional counted buffers can't be plain `__counted_by`

`pixvm_run_switch` takes `src` and `cov` that a program need not use, so a caller
passes `NULL`. The natural `uint8_t const *__counted_by(n * 4) src` **rejects that
at compile time**:

```
error: passing null to parameter 'src' of type '...__counted_by(n * 4)'
       with count value of 32 always fails
```

`-fbounds-safety` knows a `__counted_by(k)` pointer with `k > 0` cannot be `NULL`,
and when `k` is a visible constant it's a hard error, not a runtime trap. The fix is
in the vocabulary — `__counted_by_or_null(n * 4)` — which permits `NULL` (and traps
only if the program then indexes it). A small gotcha, but a real one: the "(ptr,
count) parameter" pattern the project leans on has a nullable variant you have to
reach for deliberately.

### What it costs: nothing here — it's faster

`ninja benchcmp`-style, release (`-Os -fbounds-safety`) vs unsafe (`-Os`), same
source, running a source-over program over a 256×256 tile
([../bench/bench_pixvm.c](../bench/bench_pixvm.c)):

| build | `-Os` | `-O2` |
|---|---|---|
| bounds-safe | 112 ms | 118 ms |
| unsafe | 184 ms | 134 ms |
| **bounds-safe is** | **1.65× faster** | **1.14× faster** |

The checks are not the bottleneck. The hot work is whole-vector arithmetic plus a
runtime-indexed register file, and the per-lane load/store bounds checks ride along
in the noise. The direction is the surprise: at `-Os` it is the *unsafe* build that
codegens poorly, and `-fbounds-safety` steers the optimizer onto a faster path; the
gap narrows to 14% at `-O2`. The honest reading isn't "bounds-safety is free" — it's
that for this workload the checks cost nothing measurable and the flag's extra range
information happens to help `-Os`. (Contrast the existing 2.55× worst case for a
plain RGBA8 blit, which is *only* checked indexing with no arithmetic to hide it —
see [bounds-safety.md](bounds-safety.md).)

### What it sets up for B and C

The throughput ceiling here is the register file: `reg[d]` with `d` from the
bytecode can't live in registers (the index is dynamic), so the file is stack
memory and every op round-trips through it. Design C exists precisely to remove
that — carrying r,g,b,a as function arguments so they stay in SIMD registers across
the pipeline. Measuring A against C will show that cost directly.

## Design B — tail-call threaded VM (planned)

Same bytecode; replace the switch with one handler function per opcode, each ending
in `[[clang::musttail]] return next(...)`. The `-fbounds-safety` questions:

- Threading a `__counted_by(prog_len)` program pointer (and the pc) through every
  tail call, advancing it without tripping the counted-local restrictions.
- The handler table is an array of function pointers whose parameters carry bounds
  attributes. The strict function-pointer check
  (`-Wincompatible-function-pointer-types-strict`) already bit the Core Text
  callback ([bounds-safety.md](bounds-safety.md)); a homemade dispatch table is a
  clean place to see whether *our own* annotated signatures match without a fight.
- Whether `musttail` survives the bounds-check instrumentation the flag inserts.

## Design C — SkRasterPipeline-style register pipeline (planned)

Stages with a fixed signature carrying the live registers as arguments
(`stage(program, x, tail, r, g, b, a, dr, dg, db, da)`), each tail-calling the next;
the "program" is an array of `{fn, ctx}` stages advanced by pointer bump. The
`-fbounds-safety` questions, expected to be the sharpest of the three:

- Each stage casts an opaque `void *ctx` to its own parameter struct — the generic-
  callback hole (qsort, `CGPathApply`) from [bounds-safety.md](bounds-safety.md), now
  on every stage. How much is forged, and can load/store stages keep their pixel
  pointers `__counted_by` behind that `void*`?
- The tail (the final short run of `< PIXVM_N` pixels): partial vector load/store
  that stays checked, the `memcpy`-spelling trick from the adler32 work.
