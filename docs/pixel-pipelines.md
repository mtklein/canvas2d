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
Load/store ops deinterleave RGBA8 to/from four channel vectors with shuffles (a
`memcpy` vector load plus `__builtin_shufflevector`), with a scalar fallback for the
final short chunk; arithmetic ops are whole-vector.
[../tests/test_pixvm.c](../tests/test_pixvm.c) checks a copy round-trip, a
premultiplied source-over program, and the trap below.

### What's frictionless

Everything indexed. The program is a `pixop *__counted_by(prog_len)`, the register
file is a plain `pixv reg[PIXVM_REGS]` local, the pixel buffers are `__counted_by`
parameters — and the whole interpreter (program fetch, runtime-indexed register
reads/writes, vector pixel loads/stores) compiles under `-Weverything`
`-fbounds-safety` with zero annotation gymnastics. The vector load is the adler32
trick from [bounds-safety.md](bounds-safety.md): `memcpy(&raw, src + x*4, 32)` stays
bounds-checked (the SDK annotates `memcpy` `__sized_by`) and lowers to one unaligned
vector load — so eight pixels cost a single bounds check, not 32 per-byte ones. This
is the indexed-buffer-dense case the flag is built for, and a VM is nothing but
indexed-buffer-dense.

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

### What it costs: ~8% at `-Os`

A source-over program over a 256×256 tile
([../bench/bench_pixvm.c](../bench/bench_pixvm.c)), release (`-Os -fbounds-safety`)
vs unsafe (`-Os`), same source, the `A`-vs-`A` hyperfine control at 1.00×:

| opt | bounds-safe | unsafe | cost of checks |
|---|---|---|---|
| `-Os` | 63.8 ms | 58.7 ms | **1.08×** |
| `-O2` | 66.5 ms | 54.6 ms | **1.22×** |

That is the expected shape: a small overhead at `-Os` (the shipping config), growing
to ~22% at `-O2` because the unsafe build optimizes the surrounding work better, so
the checks are a larger slice of a smaller total. The kernel lands between the
project's flatten (1.07×) and blit (2.55×) kernels — the once-per-eight-pixels
`memcpy` bounds check and the register-index checks amortize against the vector
arithmetic, nowhere near the per-byte blit worst case.

#### How we got here: a lane-store artifact, and why it mattered

The first version of this kernel deinterleaved RGBA8 one lane at a time
(`reg[d][lane] = src[...] / 255`), and benchmarked **backwards** — bounds-safe ran
1.65× *faster* than unsafe at `-Os`. That was not a bounds-safety property; it was an
`-Os` alias-analysis pessimization in the *unsafe* build. A scalar store into the
stack-resident vector register file aliases the other slots the compiler holds in
NEON registers, so plain `-Os` spilled and reloaded the whole file (`stp`/`ldp`)
around every lane write; `-fbounds-safety`'s provably in-bounds offset (lane masked
`& 7`) let its alias analysis skip the spill. Ruled out the alternatives one at a
time — `A`-vs-`A` was 1.00× (not contention), a standalone `clang` reproduced it (not
the build config), `-fno-vectorize` left it unchanged (not autovectorization), the
disassembly showed the spill/reload only in the unsafe build, and `-O2` shrank the
gap. The lesson is design-level and carries into B and C: **lane-wise writes into a
stack vector register file are codegen-hostile** — keep loads/stores whole-vector
(shuffles), which is what the rewrite above does and what C gets for free by holding
channels in function arguments. A real secondary finding survives too: bounds
annotations can *help* codegen by handing the optimizer no-alias facts.

### What it sets up for B and C

The throughput ceiling here is the register file: `reg[d]` with `d` from the
bytecode can't live in registers (the index is dynamic), so the file is stack
memory and every op round-trips through it. Design C exists precisely to remove
that — carrying r,g,b,a as function arguments so they stay in SIMD registers across
the pipeline. Measuring A against C will show that cost directly.

A's baseline for that comparison: **63.8 ms** for the 300-iteration source-over
bench at `-Os` bounds-safe (~310 Mpix/s). B and C run the same program over the same
tile, so the three numbers are directly comparable.

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
