# Flexible pixel processing under `-fbounds-safety`: three designs

A configurable per-pixel processor (the shape behind filters, blend chains, and
shader-like effects) can be built several ways. This compares three dispatch
styles on one question: where does `-fbounds-safety` help, and where does it
fight back?

1. **One big switch** — a bytecode program; a `for pc { switch(op) }` loop runs
   it, `PIXVM_N` pixels per step. *(implemented)*
2. **Tail-call threaded VM** — same bytecode, but each opcode is a handler that
   does its work and `[[clang::musttail]]`-returns the next handler.
   *(implemented)*
3. **SkRasterPipeline-style pipeline** — stages carry the current r,g,b,a (and
   x) as explicit vector arguments and tail-call the next stage, so the channels
   stay in registers across the whole program. *(implemented)*

They share an ISA (`pixvm.h`): a register file of `PIXVM_N`-wide `_Float16`
vectors, RGBA8 load/store ops, and `splat`/`mov`/`add`/`sub`/`mul`/`mad`.
Designs 2 and 3 reuse the ISA and the test programs, so the comparison is same
work, different dispatch. All three are written within `-fbounds-safety` — no
unsafe pointers, no forges anywhere in the core.

## Design A — one big switch (implemented)

`pixvm_switch.c` was `pixvm_run_switch`: an outer loop over `PIXVM_N`-pixel
chunks, an inner loop over the program, a switch on the opcode. Load/store ops
deinterleave RGBA8 to/from four channel vectors with shuffles (a `memcpy` vector
load plus `__builtin_shufflevector`), with a scalar fallback for the final short
chunk; arithmetic ops are whole-vector. `test_pixvm.c` checked a copy
round-trip, a premultiplied source-over program, and the trap below.

### What composes cleanly

Everything indexed. The program is a `pixop *__counted_by(prog_len)`, the
register file is a plain `pixv reg[PIXVM_REGS]` local, the pixel buffers are
`__counted_by` parameters — and the whole interpreter (program fetch,
runtime-indexed register reads/writes, vector pixel loads/stores) compiles under
`-Weverything` `-fbounds-safety` with no annotation gymnastics. The vector load
is the adler32 idiom from [bounds-safety.md](bounds-safety.md):
`memcpy(&raw, src + x*4, 32)` stays bounds-checked (the SDK annotates `memcpy`
`__sized_by`) and lowers to one unaligned vector load, so eight pixels cost a
single bounds check, not 32 per-byte ones.

### The safety property

A bytecode VM runs *data* as code: the register indices and pixel offsets come
from the program, not the compiler. Under `-fbounds-safety` those accesses are
bounds-checked at runtime, so a malformed program **traps instead of corrupting
memory**. `test_pixvm` covers it: a child process runs a one-instruction program
whose register operand is out of range (`STORE` reading `reg[20..23]` of a
16-register file, with the result written to `dst` so the read can't be
optimized away), and the parent asserts the child died from a signal. It does,
in every bounds-safe variant including `-Os`.

The lane subscript inside a register (`reg[d][lane]`) needs no check: `lane` is
bounded by the chunk's active count, not supplied by the bytecode. The
*register* index `d` is the one that comes from data, and that is the one
checked.

### Optional counted buffers can't be plain `__counted_by`

`pixvm_run_switch` takes `src` and `cov` that a program need not use, so a
caller passes `NULL`. The natural
`uint8_t const *__counted_by(n * 4) src` rejects that at compile time:

```
error: passing null to parameter 'src' of type '...__counted_by(n * 4)'
       with count value of 32 always fails
```

`-fbounds-safety` knows a `__counted_by(k)` pointer with `k > 0` cannot be
`NULL`, and when `k` is a visible constant it is a hard error, not a runtime
trap. The fix is `__counted_by_or_null(n * 4)`, which permits `NULL` (and traps
only if the program then indexes it). The "(ptr, count) parameter" pattern has a
nullable variant to reach for deliberately.

### What it costs: ~1.4× at `-Os`, raised by fp16

A source-over program over a 256×256 tile (`bench_pixvm_switch.c`), release vs
unsafe (same `-Os` source), `A`-vs-`A` hyperfine control 1.00× — with `_Float16`
channels, and the earlier `float` channels for contrast:

| channels (`-Os`) | bounds-safe | unsafe | cost of checks |
|---|---|---|---|
| `_Float16` (now) | 59.7 ms | 42.7 ms | **1.40×** |
| `float` (before) | 63.8 ms | 58.7 ms | 1.08× |

Moving to `_Float16` (native 128-bit NEON at 8 wide, half the register-file
bytes) made the *unchecked* kernel ~25% faster but barely moved the bounds-safe
build, so the cost of the checks rose, 1.08× → 1.40×. The index and pointer
checks are integer work fp16 doesn't shrink, so once the colour data and
arithmetic get cheaper they are a bigger slice of the total — the same mechanism
by which `-O2` widens the gap. The kernel is cheap in absolute terms (the
once-per-eight-pixels `memcpy` bounds check plus register-index checks amortize
against real work), well below the per-byte blit's 2.55×.

#### A lane-store artifact

The first version deinterleaved RGBA8 one lane at a time
(`reg[d][lane] = src[...] / 255`), and benchmarked backwards — bounds-safe ran
1.65× *faster* than unsafe at `-Os`. That was not a bounds-safety property; it
was an `-Os` alias-analysis pessimization in the *unsafe* build. A scalar store
into the stack-resident vector register file aliases the other slots the
compiler holds in NEON registers, so plain `-Os` spilled and reloaded the whole
file (`stp`/`ldp`) around every lane write; `-fbounds-safety`'s provably
in-bounds offset (lane masked `& 7`) let its alias analysis skip the spill. The
alternatives were ruled out one at a time — `A`-vs-`A` was 1.00× (not
contention), a standalone `clang` reproduced it (not the build config),
`-fno-vectorize` left it unchanged (not autovectorization), the disassembly
showed the spill/reload only in the unsafe build, and `-O2` shrank the gap. The
finding carries into B and C: lane-wise writes into a stack vector register file
are codegen-hostile — keep loads/stores whole-vector (shuffles), which is what
the rewrite above does and what C gets for free by holding channels in function
arguments. A secondary finding: bounds annotations can help codegen by handing
the optimizer no-alias facts.

### What it sets up for B and C

The throughput ceiling here is the register file: `reg[d]` with `d` from the
bytecode can't live in registers (the index is dynamic), so the file is stack
memory and every op round-trips through it. Design C exists to remove that —
carrying r,g,b,a as function arguments so they stay in SIMD registers across the
pipeline. Measuring A against C shows that cost directly.

A's baseline for that comparison: **59.7 ms** for the 300-iteration source-over
bench at `-Os` bounds-safe (~330 Mpix/s, `_Float16` channels). B and C run the
same program over the same tile, so the three numbers are directly comparable.

## Design B — tail-call threaded VM (implemented)

`pixvm_thread.c` was `pixvm_run_threaded`: same ISA and semantics as A, but no
switch and no loop. Each opcode is a handler that does its work and
`[[clang::musttail]]`-jumps to the next handler through a function-pointer
table; the chain of tail jumps is the interpreter. `-fbounds-safety` shapes this
design in three ways.

### `musttail` can't carry a `__counted_by` pointer whose bound changes

The natural threaded VM keeps the instruction pointer in a register and threads
it as an argument: `op(st, ip, rem) -> musttail op'(st, ip + 1, rem - 1)`. Under
`-fbounds-safety` that does not compile:

```
error: 'clang::musttail' attribute requires that the return value is the
       result of a function call
```

Passing `ip + 1` into a `__counted_by(rem - 1)` parameter inserts a
bound-narrowing check, so the call's result isn't returned directly and
`musttail` (which needs an exact tail position) is refused. It compiles without
the flag, and a fat `__bidi_indexable` ip threads fine with it (it carries its
own bounds, no per-call narrowing) — but `__bidi_indexable` isn't spellable in
the unsafe build, so it can't be used in source compiled both ways. The
resolution: keep the counted `prog` in the state struct (checked there) and
thread only the `__single` state pointer plus a plain `int pc`. So the flag
pushes the instruction pointer's bound out of a register and into memory — the
opposite of the threaded-VM performance idiom.

(A smaller snag, not bounds-safety's: handlers must return non-void. A
void-returning `[[clang::musttail]] return next();` trips `-Wpedantic`'s "void
function should not return void expression," so the handlers return `int`.)

### The dispatch table, and a bad opcode trapping

The function-pointer table compiled with no strict-fn-ptr fight: unlike the
un-adopted `CGPathApplierFunction` ([bounds-safety.md](bounds-safety.md)), every
handler signature is emitted by the project's own checked TU with the same
`__single`/`int` flavors, so they match `pixop_fn` exactly. And indexing
`handlers[prog[pc].kind]` is a bounds-checked array access, so a bytecode opcode
out of range **traps** — opcode validation for free. The switch gets the
opposite default: its `default: break` silently ignores an unknown opcode.
`test_pixvm` asserts both (bad opcode: threaded traps, switch is a no-op).

### The cost

Same source-over program and tile as A, release vs unsafe, `A`-vs-`A` control
1.00×, `_Float16` channels:

| backend | bounds-safe `-Os` | unsafe `-Os` | cost of checks |
|---|---|---|---|
| A — switch | 59.7 ms | 42.7 ms | **1.40×** |
| B — threaded | 88.4 ms | 49.8 ms | **1.78×** |

Unchecked, the two dispatch styles are within ~17% (threading's indirect jumps
vs the switch's jump table). The bounds-safe penalty is wider for B: the checks
and reloads cost A ~17 ms and B ~39 ms. In A the whole program runs in one
function: the optimizer hoists the register-file base, keeps `prog_len` in a
register, and the `for (pc < prog_len)` loop proves the program fetch in-bounds,
so that check is elided. In B each handler is a separate function reached by an
indirect `musttail`; the optimizer can't see across it, so every handler reloads
`st->prog`/`st->prog_len` from the (escaping) state struct and re-checks the
program fetch — visible in the disassembly as a `cmp`/`b.hi`-to-trap at the top
of every `op_*`. The bounds checks A amortizes once, B pays per opcode.

So under `-fbounds-safety` — the shipping config — the threaded VM is ~1.5×
slower than the switch (88 vs 60 ms) despite near-parity unchecked. (Threaded
dispatch may still win for a *larger* ISA, where the switch's jump table
mispredicts more; this small kernel does not reach that regime.) Moving to
`_Float16` widened both costs versus the earlier `float` channels (A 1.09× →
1.40×, B 1.55× → 1.78×): halving the colour data sped the unchecked work but not
the integer checks, so the checks loom larger.

## Design C — SkRasterPipeline-style register pipeline (implemented)

`pixvm_pipe.c` was `pixvm_run_pipe`: stages with a fixed signature carry the
live channels as arguments — `r,g,b,a` (working colour) and `dr,dg,db,da`
(backdrop), all `_Float16x8` — plus the pixel position; each does its work and
`[[clang::musttail]]`-jumps to the next. The channels ride in NEON registers
across the whole program, so there is no register file — the memory ceiling A
and B both hit. It runs the same source-over computation A and B express as
bytecode (verified bit-identical in `test_pixvm`).

### Within the rules, and the unsafe spelling isn't faster

C keeps the channels in registers and stays under `-fbounds-safety` — no
`__unsafe_indexable`, no forge. The two places SkRasterPipeline's C++ reaches
for a raw pointer both have checked spellings:

- **The program is a `__counted_by` array walked by a checked `int` index**,
  threaded through `musttail`. A counted *pointer* can't ride `musttail`
  (passing `prog + 1` into a narrower `__counted_by` inserts a bound-narrowing
  check, and `musttail` rejects a call whose result isn't returned directly —
  the B finding), but a plain index can, and `prog[idx]` is bounds-checked. The
  program-fetch cost A's loop hides by proving `pc < prog_len`, and B pays per
  opcode, C pays per *stage* — negligible at five.
- **The per-stage context is a typed `union`, not a forged `void*`.** Its buffer
  members keep `uint8_t *__counted_by(len)` with a sibling `len`, so a plain
  union access yields a pointer whose pixel loads/stores stay checked. Counted
  pointers inside a union compile and check — the bound rides in the struct, not
  the pointer.

Channels thread as vector args (no counted pointer in argument position), so
`musttail` lowers to indirect jumps; stages return `int` (a void `musttail`
return trips `-Wpedantic`). The short final chunk is the one place lane-wise
writes appear, into *local* channel vectors that alias nothing.

C was first transcribed the way SkRasterPipeline is written in C++ — a raw
`__unsafe_indexable` program pointer and a `void*` ctx forged per stage — then
rewritten to the checked spelling above. Benchmarking the two side by side:
identical, within noise. The unsafe pointers bought nothing, so they were
removed (the skirting version is in git history).

## The three together

Same source-over program, same 256×256 tile, `-Os`, `_Float16` channels,
`A`-vs-`A` control 1.00×:

| design | dispatch | channels | bounds-safe | unsafe | cost of checks |
|---|---|---|---|---|---|
| A — switch | `for`/`switch` | register file (stack) | 60.5 ms | 42.6 ms | **1.42×** |
| B — threaded | `musttail` chain | register file (struct) | 88.2 ms | 50.3 ms | **1.75×** |
| C — pipeline | `musttail`, counted index + typed union | function arguments | 32.9 ms | 32.4 ms | **~1.0×** |

Read as one axis — what `-fbounds-safety` taxes:

- A hot path that **indexes a register file**: every `reg[d]` is a check (A, B).
  fp16 makes this relatively worse, not better — it shrinks the data under the
  fixed-cost checks (A 1.08× → 1.42×).
- **Structure the optimizer can't see through**: B's per-opcode program re-check
  across an indirect `musttail`, un-hoistable, ~1.8×.
- **~Nothing** when the hot values live in registers and the only checked memory
  is a handful of per-chunk buffer accesses that amortize over the vector (C). C
  is ~1.9× faster than A *and* costs ~nothing under the flag.

C is both the fastest design and a fully bounds-checked one. Getting there meant
moving the colour channels into registers (`musttail` arguments — no register
file, so none of A/B's per-`reg[d]` checks), and that move needs no unsafe code.
The raw pointer the C++ idiom uses is a transcription habit, not a requirement;
the checked index walk and the typed-union ctx are as fast. (One caveat: the
checked index walk is free at five coarse stages; a pipeline of many stages
would start paying B's per-stage tax — but pixel pipelines are short.)

## Epilogue: the probe is retired

The three VMs (`pixvm_switch.c`, `pixvm_thread.c`, `pixvm_pipe.c`, the shared
`pixvm.h`/`pixvm_pixio.h` ISA, their test and three benches) existed to ask this
question; they were never wired into the renderer. The question is answered, so
the code has been retired from the tree — the finding is recorded here and in
the bench-table history, and the register-residency move it isolated is used
across the live kernels (the coverage resolve, the gradient solve, the blur).
Git history holds the modules.
