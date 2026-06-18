# Building with `-fbounds-safety`

The `canvas2d` core — ~6.5k lines of C23 — is compiled under
`-std=c23 -fbounds-safety -Werror -Weverything`. This document records what the
flag requires, what it costs, and the patterns that resulted.

`-fbounds-safety` is a Clang extension (originally from Apple, upstreamed into
LLVM) that makes C pointers carry bounds and checks accesses against them:
spatial memory safety for C, opt-in per translation unit, with an annotation
vocabulary in `<ptrcheck.h>`.

## The pointer flavors

Under `-fbounds-safety`, every pointer has a *flavor* describing what bounds it
carries. In project code the default is `__single` (points to exactly one
object; no arithmetic). The flavors used:

- **`__single`** (default) — one object or NULL. Can't be indexed or offset.
- **`__counted_by(n)`** — points to `n` elements; `n` is another expression in
  scope (a sibling struct field, a parameter, or a `const` local). Used for
  arrays.
- **`__bidi_indexable`** — a "wide" pointer that carries its own lower/upper
  bounds (three words). What array decay and pointer arithmetic produce.
- **`__unsafe_indexable`** — a plain C pointer with no bounds; the escape hatch
  and the ABI that system headers assume.

`__counted_by` and `__single` have the ABI of a normal C pointer. The bounds
for `__counted_by` come from the *separate* count expression, not from a
fat-pointer representation; that is what makes the feature adoptable at
boundaries (below). `__bidi_indexable` is the fat one, and it exists mostly as
an intermediate the compiler threads through expressions.

## What composes cleanly

**`__counted_by` on function parameters.** Passing `(pointer, count)` pairs
makes the relationship checkable. The PNG encoder's
`adler32(uint8_t const *__counted_by(n) data, size_t n)` indexes against `n`
automatically. `drawImage`'s bilinear sampler is the same shape: a
`uint8_t const *__counted_by(slen)` source, four clamp-to-edge taps per output
pixel, every `src[(y*sw + x)*4 + k]` guarded.

**Slicing.** To hand one subpath to the stroker:

```c
cnvs_vec2 *poly = cv->path.pts + sp.start;          // pts is __counted_by(pt_cap)
cnvs_stroke_polyline(poly, sp.count, ...);          // param is __counted_by(n)
```

`pts + sp.start` decays the counted pointer to a `__bidi_indexable` that keeps
the upper bound, and passing it into a `__counted_by(sp.count)` parameter
inserts one runtime check that `sp.start + sp.count` fits. No manual offset
arithmetic leaks into the callee, and the callee stays bounds-checked.

**The struct-as-growable-array shape.** Every dynamic buffer is

```c
typedef struct { T *__counted_by(cap) data; int len; int cap; } vec;
```

with the data pointer and its count as sibling fields. Reallocation assigns
both together:

```c
T *nd = realloc(v->data, (size_t)newcap * sizeof *nd);
if (!nd) return false;
v->data = nd;        // pointer and its count, updated together
v->cap = newcap;
```

**Structure-of-arrays.** Splitting an array-of-padded-structs into parallel
arrays does not fight the flag. Three `__counted_by(cap)` arrays sharing one
`cap`: `realloc` each, then assign the three pointers and `cap` adjacently.
Packing all three into one allocation also works — slicing the block into typed
`__counted_by(cap)` sub-pointers keeps each one's bound with no forge:

```c
int  *__counted_by(cap) a = blk;          // [cap ints | cap ints | cap bools]
int  *__counted_by(cap) b = blk + cap;
bool *__counted_by(cap) c = (bool *)(blk + 2 * cap);
```

Casting `char*`→`int*` trips `-Wcast-align`, so the largest-alignment array
goes first and the block is typed as that element, letting the smaller arrays
cast down. (The single-block form copies on growth, since the sub-array offsets
move with `cap`, which is why the code stays array-of-structs where padding is
negligible.)

**The runtime guarantee.** An out-of-bounds index traps (`SIGTRAP`, exit 133)
at `-Os`, and the check is elided where the compiler can prove safety.
`test_bounds_safety` forks a child that writes `a[1000]` into an 8-element
buffer and asserts the child dies from a signal; it passes in both the
optimized and sanitizer builds.

**Composition with the sanitizers.** The debug build stacks
`-fsanitize=address,integer,undefined` on top of `-fbounds-safety` with
`-fno-sanitize-recover=all`. The two are complementary: bounds-safety covers
spatial OOB, the sanitizers cover UB/overflow/leaks.

**`-Weverything`.** Across the core, the only warnings turned off are
environmental or C++-compat lints. The bounds annotations themselves never
forced a disable. The self-checking PNG encoder writes every byte through one
`buf[at]` cursor sized up front: if the size computation were wrong, it traps
instead of corrupting the heap.

**SIMD (`ext_vector_type`).** `ext_vector_type`, `__builtin_convertvector`, and
`__builtin_reduce_add` compile under `-Weverything`/`-fbounds-safety` (adler32
is a 16-wide example). A vector *load* of a `__counted_by` buffer is spelled
`memcpy(&v, p + i, sizeof v)`. The SDK annotates `memcpy`/`memset` as
`__sized_by`, so the load stays bounds-checked (it traps on overrun) while the
compiler lowers it to a single unaligned vector load. A direct
`*(u8x16 *)(p + i)` reinterpret trips `-Wcast-align`, an alignment/UB warning
orthogonal to bounds-safety, and reason to prefer the `memcpy` spelling. Since
the checks that cost most are per-element, vectorizing a hot loop amortizes its
checks along with its arithmetic.

## What the flag restricts

**`__counted_by` locals are restricted.** A

```c
int n = ...;
int *__counted_by(n) a = malloc(...);
```

is rejected with `local variable 'a' must be declared right next to its
dependent decl` and, if `n` is later mutated, `assignment to 'n' requires
corresponding assignment to '...a'`. A local count that references a
*parameter* fails differently: `argument of '__counted_by' attribute cannot
refer to declaration of a different lifetime`. The rules:

- A counted local works if its count is a **`const`** declared immediately
  adjacent (`int const n = ...; int *__counted_by(n) p = ...;`). Used in
  `canvas_write_png` and the PNG buffer.
- A *mutable* counted local drags its pointer along on every assignment to the
  count. Capacity math is instead done on a plain `int` with no pointer
  attached (`cnvs_grow_cap`), and the result stored into a struct field.

The net effect is that dynamic arrays go in structs and arrays pass as
`(ptr, count)` params; the error messages are cryptic until the cause is
internalized.

**Pointers to incomplete types must be `__single`, which leaks to consumers.**
The public handle is `typedef struct canvas canvas;`. A consumer who writes
`canvas *cv = canvas_create(...)` under `-fbounds-safety` gets an error —
`__bidi_indexable` (the default for a local pointer to a complete type) is
illegal for an incomplete type, so they must write `canvas *__single cv`. It is
a one-word fix the compiler suggests, and consumers *not* using
`-fbounds-safety` are unaffected (the macros vanish). The project's own tests
carry the `__single` annotations as a result.

**`<ptrcheck.h>` must be included wherever the macros appear.** Without it
`__counted_by` is not a macro but a reserved identifier, so the diagnostic is
`identifier '__counted_by' is reserved because it starts with '__'` plus a
cascade of parse errors, rather than a clear missing-include message.

**Generic callbacks lose the bounds at the boundary.** `qsort`/`bsearch` hand
the comparator a bare `const void *`: the counted pointer converts to `void *`
fine (always allowed), but inside, an `__unsafe_forge_single` is needed to
recover a typed pointer, and the swaps trust `nmemb` — so a sorted array is
checked everywhere except the sort itself. For small hot routines a hand-rolled
loop over the `__counted_by` array (every `data[j]` checked) stays safe end to
end. (The rendering core does not sort: the coverage rasterizer accumulates
into a per-pixel buffer and prefix-sums.)

**Allocation needs the `void*` conversion.** `T *p = malloc(...)` assigns a
`void*` to a typed pointer; `-Weverything` flags that under
`-Wimplicit-void-ptr-cast` (a C++-compat lint). The project disables that one
warning. This is not a bounds-safety hole: assigning an undersized allocation
to a `__counted_by(n)` target still traps at runtime (verified, exit 133). The
size check is independent of the cast diagnostic.

**An `alloc_size` wrapper can't faithfully return `malloc(0)`.** The OOM fault
injector ([tests/oom_alloc.c](../tests/oom_alloc.c)) wraps
malloc/realloc/calloc behind `alloc_size`-annotated declarations so checked
callers keep size tracking. Compiled *checked*, the wrapper traps the moment
anything allocates zero bytes: on return, `-fbounds-safety` checks that a
non-NULL result carries a non-empty range, but macOS `malloc(0)`/`calloc(0, n)`
return a non-NULL block of size zero, and zero-size allocations occur (the Core
Text shim callocs zero runs when shaping an empty string). A six-line repro
traps at every optimization level. The call *site* is fine — a checked caller
receiving the zero-size result gets a pointer it can't deref — only the checked
*definition*'s return check fires. So the injector compiles unchecked like the
boundary shims, and the annotations live in its header where callers see them.
A wrapper that must reproduce libc's exact corner-case behaviour belongs on the
unchecked side of the boundary, with its contract in the header.

**A `__counted_by` struct member can't grow its count in place.** Building the
emoji mip pyramid level by level, the natural shape is "append a level, bump
`nmips`" — but a pointer loaded from a counted member carries its *current*
count as its bounds, so incrementally raising the count under a live pointer
trips the dependency rules. The idiom (the same one every cache insert here
uses) is build in a local, install atomically: assemble the full level array in
a plain local — which carries complete bounds from `calloc`'s `alloc_size` —
then assign pointer and count together, adjacently. The rest of that code — the
pyramid math: clamped data-dependent neighbour indexing, four reads and a write
per pixel against `w*h*4` bounds, slab-slicing a counted member into chunk-sized
`__counted_by(n)` arguments — compiled and ran on the first complete build,
sanitizers included.

**Nested out-pointers (`T **`) carry no bounds.** There is no way to annotate
"pointer to a counted pointer," so a helper that returns both a buffer and its
count through out-params can't stay checked end to end. The fixes that keep
everything checked: return a small struct (a counted pointer and its count
travel together by value — works as a borrow-view), or restructure so the
callee fills caller-owned storage (the boundary's grow-and-refetch pattern).
Hit while factoring the glyph-curve fetch; the struct return was used.

## Antialiasing

Coverage is computed analytically on the CPU
([cnvs_cover.c](../src/cnvs_cover.c)): each edge deposits the exact fractional
area it leaves to its right into a per-pixel accumulation buffer, and a per-row
prefix sum turns that into winding-weighted coverage the fill rule folds to
`[0,1]` — exact in both axes. Clipping is a per-pixel coverage mask, gradients
are evaluated per pixel, and the result is a finished tile the blend stage
composites ([canvas.c](../src/canvas.c)'s planar blend kernels).

The alternative — MSAA on a GPU — antialiases only the geometry it is handed,
and a scan-converted fill is a stack of 1px-tall rectangles, so its top and
bottom step in whole pixels regardless of sample count. Analytic coverage
sidesteps that, and the hot loop is dense indexed-buffer work
(`acc[base + col] += ...` per edge per row, every index guarded against the
`__counted_by(cap)` buffer; the prefix-sum resolve; clip-mask intersection;
per-pixel tile assembly). All of it compiles and runs with no annotation
friction; nothing rasterizes, masks, or antialiases outside checked C.

## What it costs

The `release` and `unsafe` builds are identical `-Os` sources differing only in
`-fbounds-safety`, and `ninja benchcmp` runs hyperfine over each CPU-only
kernel in isolation plus an end-to-end run. A recent run:

| phase | overhead |
|---|---|
| 2D RGBA8 blit | 1.00× |
| gradient eval / gradient fill | 1.01–1.02× |
| cubic-Bézier flattening | 1.02× |
| stroke expansion | 1.03× |
| analytic coverage fill | 1.07× |
| end-to-end (now deflate-dominated) | — see below |
| box blur, vertical pass | 1.09× |
| box blur, horizontal pass | 1.10× |
| PNG decode (in-house inflate) | 1.09× |
| PNG encode (in-house deflate) | 1.32–1.43× |

The end-to-end ~1.07× is a blend that hides a wide spread between phases, in
which a regression in a fast phase could disappear. The spread:

- **Per-element checks are the cost, and vectorizing amortizes them.** The 2D
  blit was the worst case at **2.55×**: four byte loads and four stores per
  pixel across two buffers, with no arithmetic between them to hide the checks.
  Each clipped row is one contiguous run, so the inner loop is a `memcpy`;
  collapsing the eight per-pixel checks into a single per-row span check ran
  **13× faster, and dropped the overhead to ~1.0×**. PNG encode followed the
  same pattern while it was stored-zlib: its CRC went from a byte-at-a-time
  table to ARMv8's `crc32` instruction (~7× faster, 1.00×).
- **The in-house deflate held the worst case (~2.1×) for one day** — the blit's
  shape again: hash-chain walks and byte-at-a-time match verification,
  per-element checks with nothing to hide behind. The blit treatment then
  landed (8-byte vectorized match verify, XOR+ctz; 64-bit bit readers;
  table-driven Huffman decode; a 4-byte hash that made output *smaller* too):
  encode ended **10.5× faster at 1.32–1.43×**, decode 3.5× faster at 1.09×. The
  vectorized verify sped the unchecked build even more at first, briefly
  *raising* the ratio (2.0→2.2) before the chain-walk tuning collapsed it.
  What remains is data-dependent pointer chasing both builds pay for alike.
- **The coverage fill (~1.07×, down from ~1.22×)** got the same treatment
  twice: the resolve (per-row prefix sum, fill-rule fold, 8-bit convert) runs
  8-wide, and the accumulate telescopes each row span's interior columns — all
  depositing the same constant area — into a contiguous add, also one
  whole-vector check per block. The only writes still scattered
  (`acc[base + col] += …`) are the partial columns at each span's ends, one or
  two per row segment.
- **The horizontal blur pass was the worst case (~1.55×) until the same recipe
  landed (~1.10×)**: as a scalar sliding-window sum its contiguous loads never
  stalled, so the checks sat on the critical path; producing eight windows per
  step from an in-register prefix sum amortizes them to one whole-vector check
  per load. The unchecked build moved ~4%. Its strided twin — the vertical
  pass, identical math a row apart — at first looked like the opposite case: its
  scalar column walk had enough slack that the checks rode free (1.00×). The
  same recipe (eight adjacent columns per step, a running sum per lane — no
  prefix sum needed) made it ~5.8× faster and resurfaced the checks at 1.09×:
  free checks are a property of an unoptimized loop, not of an axis. The pair is
  analyzed in [stencil-blur.md](stencil-blur.md).
- **Flattening is nearly free (~1%)**: float arithmetic (de Casteljau
  midpoints, the flatness test) between a handful of indexed pushes, so the
  checks are small next to the FLOPs.
- Real canvas rendering is **CPU-bound** — the whole pipeline (rasterize, bake,
  premultiply, blend, readback) runs in checked C — so these per-kernel prices
  are the end-to-end cost, not a fraction of it. The compiler elides checks it
  can prove redundant; what's left is the cost of the ones it can't, which is
  workload-dependent.

## Comparison with Rust

After a security pass that fuzzed the public API and fixed six findings, the
following compares where the `-fbounds-safety` + UBSan + ASan stack reaches
Rust and where it does not. Each row is a bug the project hit (see
[docs/decisions/security-review.md](decisions/security-review.md)).

| Bug class | What was done in C | What Rust does | Result |
|---|---|---|---|
| **Spatial OOB** (a bad `__counted_by` bound → out-of-bounds index) | `-fbounds-safety` traps (`SIGTRAP`) in every build | mandatory bounds-check **panic** in every build | **parity** |
| **Integer overflow** in a size/count (Findings 1, 2) | UB; UBSan catches in debug; the resulting OOB then traps under `-fbounds-safety` | debug build **panics**; release **wraps** (defined), and the OOB it causes still panics on index | **comparable at runtime**; neither catches it at compile time |
| **float→int / float→uint8** conversion (Finding 4) | UB; a saturating `cnvs_f2i`/`cnvs_f2u8` (NaN→0, clamp) written by hand | `as` is **saturating by default** (the same semantics) | **Rust stronger** — safe by construction, no opt-in |
| **Temporal** (use-after-free, double-free) | *not covered* — `-fbounds-safety` is spatial-only; ASan catches it in debug, nothing in release | **borrow checker, at compile time** | **Rust stronger** |
| **Resource / termination** (Findings 5, 6: unbounded alloc, infinite loop) | found by fuzzing | found by fuzzing (or timeouts) | **same** — outside both type systems |

Three points:

1. **For the spatial-memory class, the runtime guarantee is at parity with
   Rust.** A wrong bound becomes a deterministic abort either way.

2. **The difference is in defaults and auditability, not capability.** Rust's
   `as` saturates regardless of who writes it; reintroducing Finding 4 requires
   typing `unsafe { …to_int_unchecked() }`, which is greppable. The C model is
   the inverse: a fresh `(int)floorf(x)` reintroduces the UB with no compile
   error and no warning — the explicit cast even suppresses `-Wconversion`. The
   project relies on UBSan-in-debug + fuzzing to re-catch what Rust prevents.
   The unsafe surface in Rust is small, named, and auditable; in C it is the
   implicit default. The Finding 4 fix is Rust's cast semantics written out by
   hand, at every site found.

3. **Temporal safety is the gap.** `-fbounds-safety` does not attempt it.
   UAF/double-free are caught only by ASan in the *debug* build; the release
   build is unguarded. Rust's borrow checker makes them compile errors. This is
   the part C+`-fbounds-safety` does not deliver.

From Findings 5 and 6: neither language's type system catches unbounded
allocation or non-termination. Those are logic bugs that only fuzzing (or
runtime timeouts / allocation limits) surfaces.

Net: `-fbounds-safety` + the sanitizers + fuzzing reach Rust's runtime
guarantees for the spatial and value-conversion classes; Rust's are default,
non-forgettable, partly compile-time, and extend to temporal safety.

## ABI and the C ↔ Objective-C boundary (historical)

> The Metal backend this section studied was **removed** (D1; see
> [decisions/metal-backend.md](decisions/metal-backend.md)). The findings below
> are kept as a result about `-fbounds-safety`'s reach across an Objective-C
> boundary; they no longer describe a live translation unit. The surviving
> system-framework boundary is the Core Text shim (next section), to which the
> same ABI-sharing argument applies.

Because `__counted_by`/`__single` have plain-pointer ABI, the C core and the
Objective-C Metal shim shared `compositor.h` verbatim. The shim was compiled
without `-fbounds-safety`, so the annotations there expanded to nothing — sound,
because the representations match. `compositor_blend(compositor*, int x, int y,
int w, int h, _Float16 const *__counted_by(w*h*4) tile)` was a checked call on
the C side and an ordinary pointer-and-length on the ObjC side. No shims, no
marshalling.

### Can the boundary itself be bounds-safe? No

Two findings, both verified:

1. **`-fbounds-safety` is C-only.** Adding it to the `.m` shim failed:
   `error: -fbounds-safety is supported only for C language`. There is no way to
   put an Objective-C translation unit under the flag.

2. Metal can be driven from a pure-C `.c` file via the Objective-C runtime
   (`objc_msgSend`, `<objc/*>`) and that compiles under `-fbounds-safety` — but
   the runtime and SDK hand back `__unsafe_indexable` pointers for every
   `id`/`SEL`/`Class`, and bounds-safety refuses to implicitly narrow them. The
   only way to build is to declare the **entire TU** `__unsafe_indexable`
   (`__ptrcheck_abi_assume_unsafe_indexable()`). A spike doing exactly this — a
   clear-to-red and pixel readback in pure C — compiled and ran, but the TU was
   then entirely unchecked, did manual `retain`/`release`, and was fragile FFI
   (hardcoded selector strings, enum constants, hand-mirrored struct layouts).

So blanket `-fbounds-safety` was reachable only in the sense that every TU
compiles with the flag; the GPU TU would have checked nothing. And there was
nothing to check there: the compositor forwarded already-rendered RGBA16F tiles
directly to Metal as `void*`; all the geometry, coverage, gradient, and clip
logic lived in the C core, which is already fully covered.

The design that outlived the experiment was a 100% bounds-safe C core with a
small, explicit, isolated boundary (then one `.m` file that blended tiles; now
the boundary is Core Text alone, and even the tile blend is checked C).
`-fbounds-safety`'s coverage is in the code that manipulates memory, which is
all in C.

## Binding a *C* system library (Core Text): the adoption asymmetry

Text outlines come from Core Text — a pure-C framework
(`CTFontCreatePathForGlyph`, `CGPathApply`, `CFRelease`). So the Objective-C
objection above does not apply, and the question is whether a checked `.c` can
bind it cleanly. The seam was profiled before deciding; the result is a sharper
version of the qsort case.

**Why `qsort` is clean but `CGPathApply` is not — and it is not that one is "in
our code."** Both take a callback. The difference is header adoption.
`<stdlib.h>` opts into bounds-safety with a region pragma
(`_LIBC_SINGLE_BY_DEFAULT()` → `__ptrcheck_abi_assume_single()`), so from the
TU the comparator type is `int (*)(const void *__single, const void *__single)`
— the same `__single` default the project's code emits, so it matches and
compiles with no cast. (And it enforces: spelling the comparator's params
`__unsafe_indexable` or `__bidi_indexable` does warn.) `CGPath.h` has no such
pragma, so `CGPathApplierFunction`'s parameters are attribute-free — a state
distinct from every explicit flavor. Under `-Weverything`'s strict
function-pointer check, the callback (whose params the flag forces to
`__single`) can't match an attribute-free type, and no spelling fixes it
(`__single`, `__unsafe_indexable`, `__bidi_indexable`, `__indexable` all
mismatch). The resolutions are a scoped
`#pragma clang diagnostic ignored "-Wincompatible-function-pointer-types-strict"`
around the one call, or an `__unsafe_forge`-style cast.

The strict fn-ptr warnings predate the bounds model and lack a *compatibility
lattice* for it. They parse and compare the qualifiers (`__single`/
`__unsafe_indexable` are thin, same ABI; `__bidi_indexable` is fat, a real ABI
break), but they collapse to demanding exact textual identity, so against an
un-adopted callback they are unsatisfiable from inside a checked TU.

**Adopting the header from this side doesn't reach it either.** Wrapping the
`#include`s in `__ptrcheck_abi_assume_single()` to do for Core Text what
`<stdlib.h>` does for libc was measured (regional and blanket whole-TU), and
changes nothing for the two friction points:

- Opaque handles (`CTFontRef`, `CGPathRef`) are pointers to **incomplete**
  types; `assume_single` only re-defaults pointers to *complete* types, so they
  stay `__unsafe_indexable` and still need `__unsafe_forge_single`.
- The callback lives in a **function-pointer typedef**, and `assume_single`
  doesn't descend into a typedef's parameter list (verified:
  `CGPathApplierFunction` prints attribute-free even with the pragma immediately
  above the `#include`).
- The framework umbrellas carry their own region pragmas; pragma regions *set*
  rather than stack, so an included header resets the ambient state.

So checked binding costs about two forges plus one scoped pragma, and buys no
real safety: the only buffer that grows (the output `cnvs_path`) is owned by
checked code regardless; the one genuinely unbounded read is
`CGPathElement.points`, whose length is encoded in a sibling enum
(`type` → 0–3 points) that `__counted_by` cannot name.

**The design chosen** — and the one that mirrored the (now removed) Metal
boundary — is an unchecked C shim ([cnvs_text_ct.c](../src/cnvs_text_ct.c))
behind a bounds-safe ABI ([cnvs_text.h](../src/cnvs_text.h)). With the flag
off, the FFI is plain C — no forges, no pragma — and the glyphs flow back as
ordinary device-space `cnvs_path`s the existing coverage rasterizer fills. A C
shim still takes the debug sanitizers, so that unbounded `points[i]` read is
ASan-instrumented in debug — the boundary is unchecked at compile time but not
at run time. The forges-in-checked-code alternative was viable and is closer to
the "C matches Rust" framing (the forges *are* C's `unsafe { ffi() }`); the
shim was chosen for boundary consistency and because, here, it cedes
essentially nothing.

## Writing a text parser: the `__null_terminated` seam

`canvas_replay_from()` reads a text *canvas program* (one `canvas_*` command per
line) and replays it; the parser ([src/cnvs_replay.c](../src/cnvs_replay.c)) is
a hostile-input target, so it probes what `-fbounds-safety` requires for
tokenizing untrusted text.

**Parsing by index.** Parsing by index over a `char const *__counted_by(len)`
buffer needs no annotations beyond the one on the entry pointer: the cursor is a
`size_t`, every `data[i]` is bounds-checked against `len`, and a line is a
`[start, end)` slice. Comparing a token to a literal, scanning to whitespace,
the strict "unknown command / wrong arity → reject" structure are all ordinary
C. The DoS guard (cap the line length) is a plain `if`.

**The C-library boundary.** Text parsing crosses the `__null_terminated` seam,
because that is how libc is annotated:
- `strtof`'s first argument is `__null_terminated`; a `__bidi_indexable` cursor
  won't pass without `__unsafe_null_terminated_from_indexable()` (a linear scan)
  or a forge.
- `memcpy`'s source is `__sized_by`, so copying *out of* a `__null_terminated`
  string needs `__null_terminated_to_indexable()` first.
- a `__terminated_by` (i.e. `__null_terminated`) pointer can't be subscripted
  (`lit[k]` is an error — walk it with `*lit`/`lit++`) and can't be offset by
  more than one (`p + n` is an error — check `*end` instead of `end == p + n`).

These are invisible until the compiler stops you. The first cut kept the bulk
of the parser indexable and confined the seam crossings to two leaf forges (a
copied, NUL-terminated numeric token handed to `strtof`; a copied text tail
handed to `fill_text`) — a forge is C's `unsafe {}`, two one-liners asserting an
invariant the type system can't see.

The seam is avoidable entirely, and removing both forges:
- **Numbers:** drop `strtof` for a hand float parser that reads the token in
  place by index (sign, digits, `.`fraction, `e`exponent). It never builds a C
  string, so it never touches `__null_terminated`. It is also stricter than
  `strtof` (rejects `1.5.2`, trailing junk, hex/`inf`/`nan`), the right posture
  for untrusted input. (Its scaling now steps through exact powers of ten, so
  every `%.9g` the recorder emits reparses to the identical float32 — the
  text-block format made that round-trip a correctness requirement; see
  [text-boundary.md](text-boundary.md).)
- **Text:** give the engine a length-counted `canvas_fill_text_n` /
  `stroke_text_n` (`__counted_by(len)`), and the parser hands the tail through
  as a slice — `data + j` carries its own remaining count — with no copy, no
  NUL, no forge. The NUL-terminated `fill_text` stays as a convenience that
  delegates via the safe `__null_terminated_to_indexable`.

Reaching that length-counted API also made the Core Text shim length-bounded
(it had stopped at a NUL), hardening the one unchecked TU as a side effect: it
can no longer over-read a non-terminated buffer. So chasing zero forges in the
checked parser also tightened the unchecked shim.

**One gotcha:** a parameter used *only* inside a `__counted_by(n)` annotation
reads as unused in the `unsafe` variant (where the macro expands to nothing) and
trips `-Werror=unused-parameter`, so a build green under `-fbounds-safety` can
fail without it. The fix doubles as defense-in-depth: use the bound in a real
check (`if (ts + tlen > le) return false;`), which also guards the unchecked
`unsafe`/fuzz build where `__counted_by` is absent.

Net: `-fbounds-safety` made the parsing — the attack surface — safe for free.
The libc string boundary is the only place it pushes back, and even that is
optional: with a hand float parser and a length-counted text API, the parser
reaches zero forges and zero `__null_terminated`, staying in the indexable
world. Fuzzed under ASan+UBSan with nothing found.

### The other direction: the recorder (zero forges)

`canvas_record_to()` ([src/cnvs_record.c](../src/cnvs_record.c)) is the
write-side inverse: it installs a recorder on the canvas, and each recordable
public op appends its line, so a live session is serialized to the same text
format the parser reads. Round-tripping is pinned by
[tests/test_record.c](../tests/test_record.c) two ways — replaying the file is
pixel-identical, and replaying-while-recording reproduces the file
byte-for-byte.

Writing crosses the same libc seam as reading, but in the easy direction, so
the recorder needs no forges. Emitting is `__counted_by(n)` float runs (`v[i]`,
bounds-checked) and `__null_terminated` command names / text handed straight to
`fputs`/`fprintf` — libc's sinks want `__null_terminated`, so the conversion
the parser had to forge its way out of is the direction the writer flows into.
Parsing forces `indexable → __null_terminated` (no safe conversion, hence the
forges); emitting only does `__null_terminated → libc`, which type-checks.
Producing well-formed text is the safe direction; consuming hostile text is the
hard one.

The one non-bounds-safety subtlety is **re-entrancy**: the public API composes
(`arc` calls `ellipse`; `round_rect` calls `move_to`/`arc`/`close_path`;
`arc_to` calls `line_to`/`arc`), so a naive top-of-function hook would record an
op and its expansion and replay it twice. The fix is a reference-counted
suspend: a compound op writes its own line, then brackets its sub-calls with
`enter`/`leave` so they don't also record — the file keeps the op the caller
issued, and replay re-invokes the same function (bit-identical), rather than
relying on a decomposition staying equivalent. `arc_to` has early returns, so
its body moved to a single-exit `arc_to_impl` and the public wrapper guarantees
`leave` always balances `enter` (no `__attribute__((cleanup))`, which
`-fbounds-safety` rejects).

## Designs the contract admits

§What it costs measures the price of the checks. The converse is that the
contract admits designs that idiomatic C could not responsibly ship, and those
designs keep paying for their own checks.

- **Caller-supplied memory.** A buffer-plus-length parameter was the classic C
  CVE shape: the library trusts a count it cannot verify. `__counted_by` puts
  the contract in the signature and the check at every access, so
  "let the user own the allocation" becomes a safe-and-fast design — no
  defensive copy, no internal allocation, no trust required.
  `canvas_read_rgba(cv, out, len)` is the shape; the resource doctrine ("the
  library should be excellent at graphics and let the user make the mundane
  calls — memory, threads") is C-hostile without the annotation and safe with
  it.
- **Reified immutable objects.** `Path2D`, images, recorded programs — and
  prospectively coverage masks and shaped-text blobs
  ([rasterization.md](rasterization.md) §3.5): internal create-use-destroy
  transients promoted to user-managed lifetime, shareable across canvases and
  threads by bare pointer because they are frozen after build and their buffers
  carry counts. Each promotion also shrinks the library's temporal-safety
  surface.
- **The counted seam.** The NEON `ld4`/`st4` intrinsics take unannotated
  pointers; wrapping each in a `static inline` helper whose parameter is
  `__counted_by(8)` makes the implicit conversion at every call site the bounds
  check — one per 8-pixel block ([cnvs_planar.h](../src/cnvs_planar.h)). The
  safety annotation and the efficient factoring are the same line of code.
- **The trap that forced the faster shape.** Appending vertices through
  `v->data` made the compiler reload and re-check every step (the stores might
  alias `*v`); the fix the model required — hoist to a `__counted_by` local, one
  check per block — is the code a performance engineer would write
  ([cnvs_geom.c](../src/cnvs_geom.c)). The check model pushes toward the
  efficient idiom.
- **Planar vector ABIs.** Pipeline stages passing channel planes in registers
  have no pointers in flight, hence no bounds — the safety surface concentrates
  at the load/store seams and the middle of the pipeline gets faster for the
  same structural reason it gets safer.

## Choices to reconsider

- **Hand-rolled per-type vectors.** `cnvs_verts` and the `cnvs_cover`/clip-mask
  buffers are copy-paste-shaped growable arrays. A generic macro would remove
  the duplication, but generic containers interact awkwardly with
  `-fbounds-safety` (you can't take the address of a `__counted_by` field and
  pass it around as `void**` without breaking the pointer/count coupling the
  compiler enforces), and macro-defined containers risk `-Wunused-macros`
  noise. Concrete types stayed.
- **Full-canvas clip masks.** A clip is one coverage byte per canvas pixel, and
  `save()` deep-copies it. Correct and simple, but heavier than a bounding-box
  or reference-counted mask. Adequate at the scales here.
- **Coverage rasterized over the whole bbox.** Even a thin shape allocates and
  resolves a full bounding-box coverage tile. A run-based or active-edge variant
  would touch fewer pixels; the accumulation approach is chosen for being
  sort-free and simple.

## Aspirations

(The original entry — complex shaping, a glyph cache, alignment beyond the
default — all landed; see [text-boundary.md](text-boundary.md). The current
ones:)

- **Zero mallocs per frame at steady state.** Construction allocates; rendering
  should not. The fault-injecting allocator is already interposed everywhere —
  it can count as easily as it can fail, so this aspiration is one standing
  metric away from being a gate.
- **Reify the remaining create-use-destroy transients** — the coverage mask and
  the shaped-text blob — per the resource doctrine: user-managed lifetime,
  immutable after build, counted buffers making the sharing safe.

## Rules of thumb

1. Dynamic arrays live in a struct: `T *__counted_by(cap) data; int cap;`.
   Update `data` and `cap` together after `realloc`.
2. Counted locals: only with a **`const`** count declared right next to the
   pointer. Otherwise compute sizes on a plain `int` and store into a struct.
3. Pass arrays as `(T *__counted_by(n) p, int n)` parameters.
4. Slice with `base + offset`; it keeps bounds and re-checks at the callee.
5. Annotate handles to incomplete types as `__single` (the compiler will remind
   you).
6. `#include <ptrcheck.h>` in every header that uses the macros.
7. Keep the unsafe boundary small, named, and obvious — here, one shim (the Core
   Text `.c`) behind a bounds-safe C ABI. (A Metal `.m` shim was a second such
   boundary until the GPU backend was removed; the same discipline applied.)
8. Generic callback APIs (`qsort`, `bsearch`) lose the bounds at the callback —
   forge inside, or hand-roll the small hot ones to stay checked end to end.
9. Binding an un-adopted system header from checked code fights the strict
   function-pointer check on its callbacks (no spelling matches an
   attribute-free typedef); when the gain is only cosmetic, an isolated
   unchecked shim is cleaner than scattering forges. A C shim keeps the debug
   sanitizers; an ObjC one cannot.
