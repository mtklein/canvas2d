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

### What it costs: not yet measurable — the benchmark hit a codegen artifact

The first numbers looked impossible: release (`-Os -fbounds-safety`) ran **1.65×
faster** than unsafe (`-Os`) on a source-over program over a 256×256 tile
([../bench/bench_pixvm.c](../bench/bench_pixvm.c)), same source. That is not a
bounds-safety property, and the benchmark as written measures the wrong thing.

The cause is an `-Os` alias-analysis pessimization in the *unsafe* build. The
register file is a stack array of vectors (`pixv reg[16]`), and the RGBA8
deinterleave writes it one lane at a time (`reg[d][lane] = dst[...] / 255`). Plain
`-Os` cannot prove that scalar store stays inside one 32-byte slot, so it assumes it
may clobber the other register-file entries it is holding in NEON registers and
spills/reloads them around *every* lane write — `stp`/`ldp` thrash (50/39 store/load
pairs in the hot function vs 26/17 in the bounds-safe build). `-fbounds-safety`'s
access carries a provably in-bounds offset (the lane masked `& 7`), which feeds
alias analysis enough to keep the file in registers and skip the spill.

It is an artifact, not the cost of the checks — ruled out one suspect at a time:

- the *same* binary as both A and B in hyperfine reports 1.00× — not CPU contention;
- a standalone `clang` with explicit flags reproduces it — not a `build.ninja` mistake;
- `-fno-vectorize -fno-slp-vectorize` leaves it unchanged — not autovectorization;
- the disassembly shows the `stp/ldp`-around-every-lane-store pattern in the unsafe
  build only;
- `-O2` (better alias analysis) shrinks the gap to 1.14×, and de-aliasing the
  deinterleave through locals collapses it toward 1.15× — both consistent with an
  aliasing pessimization, not a check cost.

So the honest statement is that **the cost of the checks here is below what this
benchmark can resolve**: the lane-wise register-file store dominates and swings the
result by codegen quirk. Isolating the real cost needs the deinterleave rewritten to
avoid lane stores (vector shuffles — which is what design C will do by carrying
channels in registers). Until then no cost number from this VM is trustworthy. (One
genuine secondary finding survives: bounds annotations can *help* codegen by handing
the optimizer no-alias facts — workload-specific, not a blanket win. Contrast the
2.55× worst case for a plain RGBA8 blit in [bounds-safety.md](bounds-safety.md).)

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
