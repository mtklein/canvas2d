# Flexible pixel processing under `-fbounds-safety`: three designs

A configurable per-pixel processor (the shape behind filters, blend chains, and
shader-like effects) can be built several ways. This is an exploration of three
dispatch styles, each judged on the same question the rest of the project asks:
where does `-fbounds-safety` help, and where does it fight back?

1. **One big switch** — a bytecode program; a `for pc { switch(op) }` loop runs it,
   `PIXVM_N` pixels per step. *(implemented)*
2. **Tail-call threaded VM** — same bytecode, but each opcode is a handler that does
   its work and `[[clang::musttail]]`-returns the next handler. *(implemented)*
3. **SkRasterPipeline-style pipeline** — stages carry the current r,g,b,a (and x)
   as explicit vector arguments and tail-call the next stage, so the channels stay
   in registers across the whole program. *(implemented)*

They share an ISA ([../src/pixvm.h](../src/pixvm.h)): a register file of
`PIXVM_N`-wide `_Float16` vectors, RGBA8 load/store ops, and `splat`/`mov`/`add`/`sub`/
`mul`/`mad`. Designs 2 and 3 reuse the ISA and the test programs so the comparison
is apples-to-apples — same work, different dispatch. A fourth design, **D**, then
re-spells the fastest (C) with no unsafe pointers, to find how much of it survives
strict `-fbounds-safety`. *(all implemented)*

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

### What it costs: ~1.4× at `-Os`, and fp16 raised it

A source-over program over a 256×256 tile
([../bench/bench_pixvm_switch.c](../bench/bench_pixvm_switch.c)), release vs unsafe
(same `-Os` source), `A`-vs-`A` hyperfine control 1.00× — with `_Float16` channels,
and the earlier `float` channels for contrast:

| channels (`-Os`) | bounds-safe | unsafe | cost of checks |
|---|---|---|---|
| `_Float16` (now) | 59.7 ms | 42.7 ms | **1.40×** |
| `float` (before) | 63.8 ms | 58.7 ms | 1.08× |

The counter-intuitive part: moving to `_Float16` (native 128-bit NEON at 8 wide, half
the register-file bytes) made the *unchecked* kernel ~25% faster but barely moved the
bounds-safe build, so the cost of the checks *rose*, 1.08× → 1.40×. The index and
pointer checks are integer work fp16 doesn't shrink, so once the colour data and
arithmetic get cheaper they're a bigger slice of the total — the same mechanism by
which `-O2` widens the gap. The kernel is still cheap in absolute terms (the
once-per-eight-pixels `memcpy` bounds check plus register-index checks amortize
against real work), nowhere near the per-byte blit's 2.55×.

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

A's baseline for that comparison: **59.7 ms** for the 300-iteration source-over
bench at `-Os` bounds-safe (~330 Mpix/s, `_Float16` channels). B and C run the same
program over the same tile, so the three numbers are directly comparable.

## Design B — tail-call threaded VM (implemented)

[../src/pixvm_thread.c](../src/pixvm_thread.c) is `pixvm_run_threaded`: same ISA and
semantics as A, but no switch and no loop. Each opcode is a handler that does its
work and `[[clang::musttail]]`-jumps to the next handler through a function-pointer
table; the chain of tail jumps *is* the interpreter. This is the design
`-fbounds-safety` shapes the most, in three ways.

### `musttail` can't carry a `__counted_by` pointer whose bound changes

The natural threaded VM keeps the instruction pointer in a register and threads it as
an argument: `op(st, ip, rem) -> musttail op'(st, ip + 1, rem - 1)`. Under
`-fbounds-safety` that **does not compile**:

```
error: 'clang::musttail' attribute requires that the return value is the
       result of a function call
```

Passing `ip + 1` into a `__counted_by(rem - 1)` parameter inserts a bound-narrowing
check, so the call's result isn't returned *directly* and `musttail` (which needs an
exact tail position) is refused. It compiles fine without the flag, and a fat
`__bidi_indexable` ip threads fine *with* it (it carries its own bounds, no per-call
narrowing) — but `__bidi_indexable` isn't spellable in the unsafe build, so it can't
be used in source compiled both ways. The resolution: keep the counted `prog` in the
state struct (checked there) and thread only the `__single` state pointer plus a
plain `int pc`. So the flag pushes the instruction pointer's *bound* out of a
register and into memory — the opposite of the threaded-VM performance idiom.

(A smaller snag, not bounds-safety's fault: handlers must return non-void. A
void-returning `[[clang::musttail]] return next();` trips `-Wpedantic`'s "void
function should not return void expression," so the handlers return `int`.)

### The dispatch table is friction-free, and a bad opcode traps for free

The two worries didn't materialize. The function-pointer table compiled with **no
strict-fn-ptr fight**: unlike the un-adopted `CGPathApplierFunction`
([bounds-safety.md](bounds-safety.md)), every handler signature is emitted by our own
checked TU with the same `__single`/`int` flavors, so they match `pixop_fn` exactly.
And indexing `handlers[prog[pc].kind]` is a bounds-checked array access, so a
bytecode opcode out of range **traps** — opcode validation for free. The switch gets
the opposite default: its `default: break` silently ignores an unknown opcode.
`test_pixvm` asserts both (bad opcode: threaded traps, switch is a no-op).

### The cost: bounds-safety still taxes threading more than the switch

Same source-over program and tile as A, release vs unsafe, `A`-vs-`A` control 1.00×,
`_Float16` channels:

| backend | bounds-safe `-Os` | unsafe `-Os` | cost of checks |
|---|---|---|---|
| A — switch | 59.7 ms | 42.7 ms | **1.40×** |
| B — threaded | 88.4 ms | 49.8 ms | **1.78×** |

Unchecked, the two dispatch styles are within ~17% (threading's indirect jumps vs the
switch's jump table). But the bounds-safe penalty is wider for B: the checks and
reloads cost A ~17 ms and B **~39 ms**, and that gap is the finding. In A the whole
program runs in one function: the optimizer hoists the register-file base, keeps
`prog_len` in a register, and the `for (pc < prog_len)` loop *proves the program
fetch in-bounds*, so that check is elided. In B each handler is a separate function
reached by an indirect `musttail`; the optimizer can't see across it, so every
handler reloads `st->prog`/`st->prog_len` from the (escaping) state struct and
**re-checks the program fetch** — visible in the disassembly as a `cmp`/`b.hi`-to-trap
at the top of every `op_*`. The bounds checks A amortizes once, B pays per opcode.

So under `-fbounds-safety` — the shipping config — the threaded VM is **~1.5× slower
than the switch** (88 vs 60 ms) despite near-parity unchecked. The flag penalizes the
design it can't see through. (Threaded dispatch may still win for a *larger* ISA,
where the switch's jump table mispredicts more; this small kernel doesn't reach that
regime.) Moving to `_Float16` widened both costs versus the earlier `float` channels
(A 1.09× → 1.40×, B 1.55× → 1.78×): halving the colour data sped the unchecked work
but not the integer checks, so the checks loom larger.

## Design C — SkRasterPipeline-style register pipeline (implemented)

[../src/pixvm_pipe.c](../src/pixvm_pipe.c) is `pixvm_run_pipe`: stages with a fixed
signature carry the live channels as arguments — `r,g,b,a` (working colour) and
`dr,dg,db,da` (backdrop), all `_Float16x8` — plus the pixel position; each does its
work and `[[clang::musttail]]`-jumps to the next. The channels ride in NEON
registers across the whole program, so there is **no register file** — the memory
ceiling A and B both hit. It runs the same source-over computation A and B express
as bytecode (verified bit-identical in `test_pixvm`).

### It is the sharpest `-fbounds-safety` tradeoff: speed for an unchecked dispatch

C keeps the channels in registers and moves the *dispatch* out of the checked
domain entirely, in two forced moves:

- **The program pointer is raw (`__unsafe_indexable`).** A `__counted_by` advancing
  pointer can't survive `musttail` (the B finding), so the pipeline walk is
  unchecked — exactly what SkRasterPipeline does. Defensible here: the program is a
  handful of stages built by trusted code, not attacker input. But it is a real
  unchecked seam, where A's program fetch is checked-then-elided and B's is checked
  per op.
- **Each stage forges its `void *ctx`** to a typed context (`__unsafe_forge_single`)
  — the qsort/`CGPathApply` hole, now once per stage. The redeeming detail: a
  buffer's bound rides in a sibling `int len` field, so after the forge the pixel
  loads/stores are still `__counted_by(len)` and **checked**. The void* erases the
  *pointer's* bound; the struct re-establishes it. So the large-memory accesses keep
  their guard even though the dispatch and ctx-unwrap don't.

Channels thread as vector args (no counted pointer in argument position), so
`musttail` lowers cleanly to indirect jumps; stages return `int` (a void `musttail`
return trips `-Wpedantic`, the B gotcha again). The short final chunk is the one
place lane-wise writes appear, into *local* channel vectors that don't alias
anything — cold, and harmless.

But is the unchecked dispatch actually *necessary* for the speed? Design D refuses
the unsafe seams and finds out.

## Design D — the register pipeline, fully checked

[../src/pixvm_pipe_checked.c](../src/pixvm_pipe_checked.c) is C with both unsafe
seams removed — strict `-fbounds-safety`, no `__unsafe_indexable`, no forge. It keeps
the part that matters (channels still thread as `_Float16x8` `musttail` arguments, in
registers) and replaces the seams with bounds-safety-legal constructs:

- The raw program pointer becomes a `__counted_by` stage array in a struct, walked by
  a checked `int` index threaded through `musttail`. A counted *pointer* still can't
  thread through `musttail`, but a plain index can, and `prog[idx]` is bounds-checked.
- The `void *ctx` becomes a typed `union` in the stage struct. Its buffer members keep
  `uint8_t *__counted_by(len)` with a sibling `len`, so after a plain (typed) union
  access the pixel loads/stores stay checked — no forge. Counted pointers inside a
  union compile and check fine.

Both compile clean under `-Weverything -fbounds-safety` and in the unsafe build.

## All four together

Same source-over program, same 256×256 tile, `-Os`, `_Float16` channels, `A`-vs-`A`
control 1.00×, one hyperfine run:

| design | dispatch | channels | bounds-safe | unsafe | cost of checks |
|---|---|---|---|---|---|
| A — switch | `for`/`switch` | register file (stack) | 60.5 ms | 42.6 ms | **1.42×** |
| B — threaded | `musttail` chain | register file (struct) | 88.2 ms | 50.3 ms | **1.75×** |
| C — pipeline, unsafe seams | `musttail`, raw ptr + `void*` | function arguments | 32.9 ms | 31.5 ms | **1.04×** |
| D — pipeline, fully checked | `musttail`, counted index + union | function arguments | 32.9 ms | 32.9 ms | **~1.0×** |

The headline is **D ≈ C**: refusing the unsafe seams costs nothing measurable. So
the earlier reading of C — "fastest because it took the dispatch out from under the
flag" — was wrong about *why*. The win is entirely **channels in registers**, which
needs no unsafe code at all: with the channels in `musttail` arguments there is no
indexed register file, so the per-access `reg[d]` checks that A and B pay simply
don't exist. C's raw program pointer and `void*` forge bought ~nothing on top of
that — the checked index walk and the typed union are free here, because the program
is only five coarse stages, so the per-stage dispatch re-check that taxed B at
seventeen fine-grained ops is negligible.

So the corrected spectrum, read as *what `-fbounds-safety` taxes*:

- It taxes a hot path that **indexes a register file** — every `reg[d]` is a check (A,
  B). fp16 makes this worse, not better: it shrinks the data under the fixed-cost
  checks (A 1.08× → 1.42×).
- It taxes **structure the optimizer can't see through** — B's per-opcode dispatch
  re-check across an indirect `musttail`, un-hoistable, ~1.8×.
- It costs **~nothing** when the hot values live in registers and the only checked
  memory is a handful of per-chunk buffer accesses that amortize over the vector (C,
  D). And reaching that does **not** require giving up checking: D is as fast as C and
  fully bounds-safe.

The practical lesson for this renderer: reach for the register-pipeline shape (D) —
channels threaded in registers, the program a counted array walked by index, pixel
buffers `__counted_by`, contexts typed. It is the fastest design *and* the fully
checked one. The unsafe pointer C used was a transcription habit from how
SkRasterPipeline is written in C++, not a requirement; under `-fbounds-safety` the
checked spelling is right there and just as fast. (The caveat is stage count: D's
checked index walk is free at five coarse stages; a pipeline of *many* stages would
start paying B's per-stage tax — but pixel pipelines are short.)
