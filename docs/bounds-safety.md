# Building with `-fbounds-safety`: what worked, what fought back

This is a field report from writing the `canvas2d` core — ~6.5k lines of C23 —
entirely under `-std=c23 -fbounds-safety -Werror -Weverything`. It's opinionated
and specific; the goal is to capture what we actually learned, including the
sharp edges, while it's fresh.

If you've never seen `-fbounds-safety`: it's a Clang extension (originally from
Apple, upstreamed into LLVM) that makes C pointers carry bounds and checks
accesses against them — spatial memory safety for C, opt-in per translation
unit, with a deliberately small annotation vocabulary in `<ptrcheck.h>`.

## The mental model that made it click

Under `-fbounds-safety`, every pointer has a *flavor* describing what bounds it
carries. In project code the default is `__single` (points to exactly one
object; no arithmetic). The ones we used constantly:

- **`__single`** (default) — one object or NULL. Can't be indexed or offset.
- **`__counted_by(n)`** — points to `n` elements; `n` is another expression in
  scope (a sibling struct field, a parameter, or a `const` local). This is the
  workhorse for arrays.
- **`__bidi_indexable`** — a "wide" pointer that carries its own lower/upper
  bounds (three words). What array decay and pointer arithmetic produce.
- **`__unsafe_indexable`** — a plain old C pointer with no bounds; the escape
  hatch and the ABI that system headers assume.

The key realization: **`__counted_by` and `__single` have the ABI of a normal C
pointer.** The bounds for `__counted_by` come from the *separate* count
expression, not from a fat-pointer representation. That's what makes the feature
adoptable at boundaries (more below). `__bidi_indexable` is the fat one, and it
exists mostly as an intermediate the compiler threads through expressions.

## What works well

**`__counted_by` on function parameters is frictionless and expressive.**
Passing `(pointer, count)` pairs is exactly how careful C already gets written;
the annotation just makes the relationship checkable. Our PNG encoder's
`adler32(uint8_t const *__counted_by(n) data, size_t n)` reads naturally and the
loop body needs nothing special — indexing is checked against `n` automatically.
`drawImage`'s bilinear sampler is the same story at its sharpest: a
`uint8_t const *__counted_by(slen)` source, four clamp-to-edge taps per output
pixel, every `src[(y*sw + x)*4 + k]` guarded — the canonical 2D image-sampling
hot path, and the annotations cost nothing to write or read.

**Slicing converts cleanly.** The single nicest surprise: to hand one subpath to
the stroker we write

```c
cnvs_vec2 *poly = cv->path.pts + sp.start;          // pts is __counted_by(pt_cap)
cnvs_stroke_polyline(poly, sp.count, ...);          // param is __counted_by(n)
```

`pts + sp.start` decays the counted pointer to a `__bidi_indexable` that *keeps*
the upper bound, and passing it into a `__counted_by(sp.count)` parameter inserts
one runtime check that `sp.start + sp.count` fits. No manual offset arithmetic
leaks into the callee, and the callee stays bounds-checked. This is the pattern
that made me stop worrying about "how do I express a subarray."

**The struct-as-growable-array shape is clean once internalized.** Every dynamic
buffer is

```c
typedef struct { T *__counted_by(cap) data; int len; int cap; } vec;
```

with the data pointer and its count as sibling fields. Reallocation just assigns
both together:

```c
T *nd = realloc(v->data, (size_t)newcap * sizeof *nd);
if (!nd) return false;
v->data = nd;        // pointer and its count, updated together
v->cap = newcap;
```

**Structure-of-arrays carves cleanly — it's just normal C.** We probed whether
splitting an array-of-padded-structs into parallel arrays fights the flag. It
doesn't. Three `__counted_by(cap)` arrays sharing one `cap` is the easy shape:
`realloc` each, then assign the three pointers and `cap` adjacently. Packing all
three into *one* allocation also works — slicing the block into typed
`__counted_by(cap)` sub-pointers keeps each one's bound with **no forge**:

```c
int  *__counted_by(cap) a = blk;          // [cap ints | cap ints | cap bools]
int  *__counted_by(cap) b = blk + cap;
bool *__counted_by(cap) c = (bool *)(blk + 2 * cap);
```

The only friction is general C, not bounds-safety: casting `char*`→`int*` trips
`-Wcast-align`, so you order the **largest-alignment array first** and type the
block as that element, letting the smaller arrays cast *down*. (`-fbounds-safety`
adds nothing here a careful C programmer wouldn't already do; the single-block
form also has to copy on growth, since the sub-array offsets move with `cap`,
which is why we kept things array-of-structs where padding is negligible.)

**The runtime guarantee is real and cheap.** An out-of-bounds index traps
(`SIGTRAP`, exit 133) **even at `-Os`**, and the check is elided where the
compiler can prove safety. Our `test_bounds_safety` forks a child that writes
`a[1000]` into an 8-element buffer and asserts the child dies from a signal — it
passes in both the optimized and sanitizer builds. Compile-time enforcement plus
a zero-cost-when-provable runtime guard is exactly the Rust-parity pitch.

**It composes with the sanitizers.** The debug build stacks
`-fsanitize=address,integer,undefined` on top of `-fbounds-safety` with
`-fno-sanitize-recover=all`, and the two are complementary, not redundant —
bounds-safety covers spatial OOB, the sanitizers cover UB/overflow/leaks.

**It barely fought `-Weverything`.** Across the whole core, the only warnings we
turned off were environmental or C++-compat lints (see the README's five). The
bounds annotations themselves never forced a disable. The self-checking PNG
encoder — which writes every byte through one `buf[at]` cursor sized up front —
is a good demonstration: if the size computation were ever wrong, it traps
instead of corrupting the heap.

**SIMD (`ext_vector_type`) cooperates cleanly.** `ext_vector_type`,
`__builtin_convertvector`, and `__builtin_reduce_add` compile under
`-Weverything`/`-fbounds-safety` with no friction (our adler32 is a 16-wide
example). The one real question — how a vector *load* reads a `__counted_by`
buffer — has a clean answer: spell it `memcpy(&v, p + i, sizeof v)`. The SDK
annotates `memcpy`/`memset` as `__sized_by`, so the load stays bounds-checked (it
traps on overrun) while the compiler lowers it to a single unaligned vector load.
A direct `*(u8x16 *)(p + i)` reinterpret trips `-Wcast-align` instead — an
alignment/UB warning, orthogonal to bounds-safety, and reason enough to prefer the
`memcpy` spelling. Because the checks that cost most are per-element, vectorizing a
hot loop tends to amortize its checks along with its arithmetic.

## What fought back

**`__counted_by` *locals* are surprisingly restricted.** This is the biggest
adjustment. A naive

```c
int n = ...;
int *__counted_by(n) a = malloc(...);
```

is rejected with `local variable 'a' must be declared right next to its
dependent decl` and, if `n` is later mutated, `assignment to 'n' requires
corresponding assignment to '...a'`. A local count that references a *parameter*
fails differently: `argument of '__counted_by' attribute cannot refer to
declaration of a different lifetime`. The rules that actually hold:

- A counted local works if its count is a **`const`** declared immediately
  adjacent (`int const n = ...; int *__counted_by(n) p = ...;`). We use exactly
  this in `canvas_write_png` and the PNG buffer.
- A *mutable* counted local drags its pointer along on every assignment to the
  count — usable but fiddly, so we just don't. Capacity math is done on a plain
  `int` with no pointer attached (`cnvs_grow_cap`), and the result is stored into
  a struct field, never a counted local.

The net effect is that the feature *pushes you* toward putting dynamic arrays in
structs and passing `(ptr, count)` params — which is good style — but the error
messages are cryptic until you've internalized why.

**Pointers to incomplete types must be `__single`, and that leaks to consumers.**
Our public handle is `typedef struct canvas canvas;`. A consumer who writes
`canvas *cv = canvas_create(...)` under `-fbounds-safety` gets an error —
`__bidi_indexable` (the default for a local pointer to a complete type) is
illegal for an incomplete type, so they must write `canvas *__single cv`. It's a
one-word fix the compiler suggests, and consumers *not* using `-fbounds-safety`
are unaffected (the macros vanish), but it is a real bit of API friction we
didn't anticipate. Our own tests carry the `__single` annotations as a result.

**`<ptrcheck.h>` must be included wherever the macros appear.** Forget it and
`__counted_by` isn't a macro — it's a reserved identifier, so you get
`identifier '__counted_by' is reserved because it starts with '__'` plus a
cascade of parse errors, rather than a clear "you forgot an include." Bit us once
when a header pulled in only `cnvs_math.h`.

**Generic callbacks lose the bounds at the boundary.** `qsort`/`bsearch` hand the
comparator a bare `const void *`: the counted pointer converts to `void *` fine
(always allowed), but inside you must `__unsafe_forge_single` it back to a typed
pointer, and the swaps trust `nmemb` — so a sorted array is checked everywhere
*except the sort itself*. The takeaway for any generic callback API: it's a hole
exactly at the callback, so for small hot routines a hand-rolled loop over the
`__counted_by` array (every `data[j]` checked) stays safe end to end. (The
rendering core happens not to sort at all — the coverage rasterizer accumulates
into a per-pixel buffer and prefix-sums — but it's the sharpest example of where
the checking stops.)

**Allocation needs the `void*` conversion.** `T *p = malloc(...)` assigns a
`void*` to a typed pointer; `-Weverything` flags that under
`-Wimplicit-void-ptr-cast` (a C++-compat lint). Either cast explicitly or, as we
chose, disable that one warning for a C-only project. Crucially this is *not* a
bounds-safety hole: assigning an undersized allocation to a `__counted_by(n)`
target still traps at runtime (we verified — exit 133). The size check is
independent of the cast diagnostic.

**An `alloc_size` wrapper can't faithfully return `malloc(0)`.** Our OOM fault
injector ([tests/oom_alloc.c](../tests/oom_alloc.c)) wraps malloc/realloc/calloc
behind `alloc_size`-annotated declarations so checked callers keep size tracking.
Compiled *checked*, the wrapper traps the moment anything allocates zero bytes:
on return, `-fbounds-safety` checks that a non-NULL result carries a non-empty
range — but macOS `malloc(0)`/`calloc(0, n)` return a non-NULL block of size
zero, and zero-size allocations legitimately occur (the Core Text shim callocs
zero runs when shaping an empty string). A six-line repro traps at every
optimization level. The call *site* is fine — a checked caller receiving the
zero-size result just gets a pointer it can't deref — it's only the checked
*definition*'s return check that fires. So the injector compiles unchecked like
the boundary shims (the `brk` was, fittingly, exit 133 again), and the
annotations live in its header where callers see them. The general lesson: a
wrapper that must reproduce libc's exact corner-case behaviour belongs on the
unchecked side of the boundary, with its contract expressed in the header.

**A `__counted_by` struct member can't grow its count in place.** Building the
emoji mip pyramid level by level, the natural shape is "append a level, bump
`nmips`" — but a pointer loaded from a counted member carries its *current*
count as its bounds, so incrementally raising the count under a live pointer
trips the dependency rules. The idiom (the same one every cache insert here
uses) is **build in a local, install atomically**: assemble the full level
array in a plain local — which carries complete bounds from `calloc`'s
`alloc_size` — then assign pointer and count together, adjacently. Two lines
once you know it, but it dictates program shape. Worth weighing against the
rest of that code's experience: the pyramid math itself — clamped
data-dependent neighbour indexing, four reads and a write per pixel against
`w*h*4` bounds, slab-slicing a counted member into chunk-sized
`__counted_by(n)` arguments — compiled and ran clean on the first complete
build, sanitizers and all. For byte-level image code that is not the usual
experience, and the bounds discipline forcing every size relationship to be
stated up front deserves part of the credit.

**Nested out-pointers (`T **`) carry no bounds.** There is no way to annotate
"pointer to a counted pointer," so a helper that wants to return both a buffer
and its count through out-params can't stay checked end to end. The fixes that
keep everything checked: return a small struct (a counted pointer and its
count travel together by value — works fine as a borrow-view), or restructure
so the callee fills caller-owned storage (the boundary's grow-and-refetch
pattern). Hit while factoring the glyph-curve fetch; the struct return won.

## Antialiasing: the strongest demonstration

Antialiasing is where the project makes its most pointed `-fbounds-safety`
argument, because **the thing a 2D renderer is judged on — edge quality — lives
entirely in bounds-checked C.** Coverage is computed analytically on the CPU
([cnvs_cover.c](../src/cnvs_cover.c)): each edge deposits the exact fractional area
it leaves to its right into a per-pixel accumulation buffer, and a per-row prefix
sum turns that into winding-weighted coverage the fill rule folds to `[0,1]` —
exact in *both* axes. Clipping is a per-pixel coverage mask, gradients are
evaluated per pixel, and the result is a finished tile the GPU only composites
([compositor_metal.m](../src/compositor_metal.m)).

The alternative — MSAA on the GPU — can't match it for a CPU-fed renderer: MSAA
only antialiases the geometry it's actually handed, and a scan-converted fill is a
stack of 1px-tall rectangles, so its top and bottom step in whole pixels no matter
the sample count. Doing coverage analytically sidesteps that, and puts the hot
loop squarely in `-fbounds-safety`'s wheelhouse: dense indexed-buffer work
(`acc[base + col] += ...` per edge per row, every index guarded against the
`__counted_by(cap)` buffer; the prefix-sum resolve; clip-mask intersection;
per-pixel tile assembly). All of it compiles and runs with zero annotation
friction, and the GPU does no rasterization, masking, or antialiasing at all.

## What it costs

We measure this directly: the `release` and `unsafe` builds are identical `-Os`
sources differing only in `-fbounds-safety`, and `ninja benchcmp` runs hyperfine
over each CPU-only kernel **in isolation** plus an end-to-end run. A recent run:

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

The isolation matters. The end-to-end ~1.07× is a blend that hides a wide spread
between phases — and a regression in a fast phase could disappear into it. What
the spread shows:

- **Per-element checks are the whole cost, and vectorizing amortizes them away.**
  The 2D blit *used* to be the worst case at **2.55×**: four byte loads and four
  stores per pixel across two buffers, with no arithmetic between them to hide the
  checks — the canonical C buffer-bug pattern, and the strongest case for having
  the checks at all. But each clipped row is one contiguous run, so the inner loop
  is really a `memcpy`; collapsing the eight per-pixel checks into a single per-row
  span check ran **13× faster, and dropped the overhead to ~1.0×**. PNG
  encode told the same story while it was stored-zlib: its CRC went from a
  byte-at-a-time table to ARMv8's `crc32` instruction (~7× faster, 1.00×).
- **The in-house deflate held the worst-case crown (~2.1×) for one day** —
  the blit's shape all over again: hash-chain walks and byte-at-a-time match
  verification, per-element checks with nothing to hide behind. The blit
  treatment then landed (8-byte vectorized match verify, XOR+ctz; 64-bit bit
  readers; table-driven Huffman decode; a 4-byte hash that made output
  *smaller* too): encode ended **10.5× faster at 1.32–1.43×**, decode 3.5×
  faster at 1.09×. One twist worth recording: the vectorized verify sped the
  unchecked build even more at first, briefly *raising* the ratio (2.0→2.2)
  before the chain-walk tuning collapsed it — amortizing checks and chasing
  absolute speed are correlated but not identical errands. What residue
  remains is data-dependent pointer chasing both builds pay for alike.
- **The coverage fill (~1.07×, down from ~1.22×)** got the same treatment twice:
  the resolve (per-row prefix sum, fill-rule fold, 8-bit convert) runs 8-wide,
  and the accumulate telescopes each row span's interior columns — all depositing
  the same constant area — into a contiguous add, also one whole-vector check per
  block. The only writes still scattered (`acc[base + col] += …`) are the partial
  columns at each span's ends, one or two per row segment.
- **The horizontal blur pass was the worst case (~1.55×) until the same recipe
  landed there too (~1.10×)**: as a scalar sliding-window sum its contiguous loads
  never stalled, so the checks sat on the critical path; producing eight windows
  per step from an in-register prefix sum amortizes them to one whole-vector
  check per load. The unchecked build barely moved (~4%) — the restructuring's
  entire value was the checks, the sharpest demonstration yet that the flag
  shifts the optimization landscape. Its strided twin — the vertical pass,
  identical math a row apart — looked like the opposite case: its scalar column
  walk had so much slack the checks rode free (1.00×). Then the same recipe
  (eight adjacent columns per step, a running sum per lane — no prefix sum
  needed) made it ~5.8× faster *and resurfaced the checks at 1.09×*: free
  checks are a property of an unoptimized loop, not of an axis. The pair is
  dissected in [stencil-blur.md](stencil-blur.md).
- **Flattening is nearly free (~1%)**: lots of float arithmetic (de Casteljau
  midpoints, the flatness test) between a handful of indexed pushes, so the checks
  are noise next to the FLOPs.
- Real canvas rendering is **GPU-bound**, so the end-to-end cost of safety is
  smaller still — but these are the honest prices on the hottest pure-C kernels.
  The compiler elides checks it can prove redundant; what's left is the cost of the
  ones it can't, and that cost is very workload-dependent.

## How close is this to Rust, really?

The project's pitch is "C can play with the big boys." After a security pass that
fuzzed the public API and fixed six findings, here's the honest scorecard — where
the `-fbounds-safety` + UBSan + ASan stack reaches Rust, and where it doesn't.
Each row is a real bug we hit (see `secreview/SECURITY-REVIEW.md`).

| Bug class | What we did in C | What Rust does | Verdict |
|---|---|---|---|
| **Spatial OOB** (a bad `__counted_by` bound → out-of-bounds index) | `-fbounds-safety` traps (`SIGTRAP`) in every build | mandatory bounds-check **panic** in every build | **parity** — this is the headline, and it holds |
| **Integer overflow** in a size/count (Findings 1, 2) | UB; UBSan catches in debug; the resulting OOB then traps under `-fbounds-safety` | debug build **panics**; release **wraps** (defined), and the OOB it causes still panics on index | **comparable at runtime**; neither catches it at compile time |
| **float→int / float→uint8** conversion (Finding 4) | UB; we wrote a saturating `cnvs_f2i`/`cnvs_f2u8` (NaN→0, clamp) by hand | `as` is **saturating by default** (the exact semantics we re-implemented) | **Rust stronger** — safe by construction, no opt-in |
| **Temporal** (use-after-free, double-free) | *not covered* — `-fbounds-safety` is spatial-only; ASan catches it in debug, nothing in release | **borrow checker, at compile time** | **Rust much stronger** — the real gap |
| **Resource / termination** (Findings 5, 6: unbounded alloc, infinite loop) | found by fuzzing | found by fuzzing (or timeouts) | **same** — outside both type systems |

Three things are worth stating plainly:

1. **For the spatial-memory class — the thing the project is actually about —
   the runtime guarantee is at parity with Rust.** A wrong bound becomes a
   deterministic abort either way. That's the win, and it's real.

2. **The difference is in *defaults and auditability*, not just capability.**
   Rust's `as` saturates no matter who writes it; reintroducing Finding 4 requires
   typing `unsafe { …to_int_unchecked() }`, which is greppable. Our model is the
   inverse: a teammate writes a fresh `(int)floorf(x)` and silently reintroduces
   the UB with *no compile error and no warning* — the explicit cast even
   suppresses `-Wconversion`. We rely on UBSan-in-debug + fuzzing to *re-catch* what
   Rust *prevents*. The unsafe surface in Rust is small, named, and auditable; in
   C it's the implicit default everywhere. Finding 4 is the vivid case: our fix is
   literally Rust's cast semantics, written out by hand, at every site we could find.

3. **Temporal safety is the honest gap.** `-fbounds-safety` doesn't attempt it.
   UAF/double-free are caught only by ASan in the *debug* build; the shipping
   release build is unguarded. Rust's borrow checker makes them compile errors.
   If the goal is "Rust-parity memory safety," this is the part C+`-fbounds-safety`
   does not deliver.

And a quieter point from Findings 5 and 6: neither language's type system catches
unbounded allocation or non-termination. Those are logic bugs that only fuzzing
(or runtime timeouts / allocation limits) surfaces — a reminder that "memory safe"
is not "correct," in either language.

The net: `-fbounds-safety` + the sanitizers + fuzzing can *reach* Rust's runtime
guarantees for the spatial and value-conversion classes — but Rust's are **default,
non-forgettable, partly compile-time, and extend to temporal safety**, which is
where the analogy stops.

## ABI and the C ↔ Objective-C boundary

The most important practical property: because `__counted_by`/`__single` have
plain-pointer ABI, the C core and the Objective-C Metal shim share `compositor.h`
verbatim. The shim is (currently) compiled without `-fbounds-safety`, so the
annotations there expand to nothing — and that's *sound*, not a fudge, precisely
because the representations match. `compositor_blend(compositor*, int x, int y,
int w, int h, _Float16 const *__counted_by(w*h*4) tile)` is a checked call on the C
side and an ordinary pointer-and-length on the ObjC side. No shims, no marshalling.

### Can the boundary itself be bounds-safe? No — and that's fine

We tried. Two findings, both verified:

1. **`-fbounds-safety` is C-only.** Adding it to the `.m` shim fails outright:
   `error: -fbounds-safety is supported only for C language`. There is no way to
   put an Objective-C translation unit under the flag.

2. You *can* drive Metal from a pure-C `.c` file via the Objective-C runtime
   (`objc_msgSend`, `<objc/*>`) and that compiles under `-fbounds-safety` — but
   the runtime and SDK hand back `__unsafe_indexable` pointers for every
   `id`/`SEL`/`Class`, and bounds-safety refuses to implicitly narrow them. The
   only way to build is to declare the **entire TU** `__unsafe_indexable`
   (`__ptrcheck_abi_assume_unsafe_indexable()`). A spike doing exactly this — a
   clear-to-red and pixel readback in pure C — compiles and runs, but the TU is
   then *entirely* unchecked, loses ARC (manual `retain`/`release`), and is
   fragile FFI (hardcoded selector strings, enum constants, hand-mirrored struct
   layouts).

So "blanket `-fbounds-safety`" is reachable only in the hollow sense that every
TU compiles with the flag; the GPU TU would check nothing. And there is nothing
to check there: the compositor forwards already-rendered RGBA16F tiles directly to
Metal as `void*`; all the geometry, coverage, gradient, and clip logic lives in
the C core, which is already fully covered.

The principled conclusion — and the design we keep — is a 100% bounds-safe C
core with a small, explicit, **isolated** Objective-C boundary (one `.m` file
that does nothing but blend tiles). That isolation is not a limitation to
apologise for; it's where the unsafe platform edge belongs, named and contained.
`-fbounds-safety`'s value is in the code that actually manipulates memory, and
that is all in C.

## Binding a *C* system library (Core Text): the adoption asymmetry

Text outlines come from Core Text — a pure-C framework (`CTFontCreatePathForGlyph`,
`CGPathApply`, `CFRelease`). So the Objective-C objection above doesn't apply, and
the interesting question becomes: can a checked `.c` bind it cleanly? We profiled
the seam empirically before deciding, and the result is a sharper version of the
qsort gotcha.

**Why `qsort` is clean but `CGPathApply` isn't — and it's *not* that one is "in
our code."** Both take a callback we define. The difference is **header
adoption**. `<stdlib.h>` opts into bounds-safety with a region pragma
(`_LIBC_SINGLE_BY_DEFAULT()` → `__ptrcheck_abi_assume_single()`), so from our TU
the comparator type is `int (*)(const void *__single, const void *__single)` — the
*same* `__single` default our own code emits, so it matches and compiles with no
cast. (And it's genuinely enforcing: spell the comparator's params
`__unsafe_indexable` or `__bidi_indexable` and it *does* warn.) `CGPath.h` has no
such pragma, so `CGPathApplierFunction`'s parameters are *attribute-free* — a state
distinct from every explicit flavor. Under `-Weverything`'s strict
function-pointer check, the callback we write (whose params the flag *forces* to
`__single`) can't match an attribute-free type, and **no spelling fixes it** — we
tried `__single`, `__unsafe_indexable`, `__bidi_indexable`, `__indexable`, all four
mismatch. The clean resolutions are a scoped
`#pragma clang diagnostic ignored "-Wincompatible-function-pointer-types-strict"`
around the one call, or an `__unsafe_forge`-style cast.

This is the honest framing of the strict fn-ptr warnings: they predate the bounds
model and lack a *compatibility lattice* for it. They parse and compare the
qualifiers (good — `__single`/`__unsafe_indexable` are thin, same ABI;
`__bidi_indexable` is fat, a real ABI break), but they collapse to demanding exact
textual identity, so against an un-adopted callback they're unsatisfiable from
inside a checked TU.

**Adopting the header *from our side* doesn't reach it either.** The obvious
move — wrap the `#include`s in `__ptrcheck_abi_assume_single()` to do for Core Text
what `<stdlib.h>` does for libc — we measured (regional *and* blanket whole-TU),
and it changes nothing for our two friction points:

- Opaque handles (`CTFontRef`, `CGPathRef`) are pointers to **incomplete** types;
  `assume_single` only re-defaults pointers to *complete* types, so they stay
  `__unsafe_indexable` and still need `__unsafe_forge_single`.
- The callback lives in a **function-pointer typedef**, and `assume_single`
  doesn't descend into a typedef's parameter list (verified: `CGPathApplierFunction`
  prints attribute-free even with the pragma immediately above the `#include`).
- The framework umbrellas carry their *own* region pragmas anyway; pragma regions
  *set* rather than stack, so an included header resets the ambient state.

So checked binding costs about two forges plus one scoped pragma — and crucially
buys **no real safety**, because the only buffer that grows (the output
`cnvs_path`) is owned by checked code regardless; the one genuinely unbounded read
is `CGPathElement.points`, whose length is encoded in a sibling enum
(`type` → 0–3 points) that `__counted_by` can't even name.

**The design we chose** mirrors the Metal boundary: an unchecked C shim
([cnvs_text_ct.c](../src/cnvs_text_ct.c)) behind a bounds-safe ABI
([cnvs_text.h](../src/cnvs_text.h)). With the flag off, the FFI is natural C — no
forges, no pragma, no fight — and the glyphs flow back as ordinary device-space
`cnvs_path`s the existing coverage rasterizer fills. One refinement over the `.m`:
a C shim still takes the debug sanitizers, so that unbounded `points[i]` read is
**ASan-instrumented in debug** — the boundary is unchecked at compile time but not
at run time. The forges-in-checked-code alternative was viable and is arguably the
tighter "C matches Rust" story (the forges *are* C's `unsafe { ffi() }`); we went
with the shim for boundary consistency and because, here, it cedes essentially
nothing.

## Writing a text parser: the `__null_terminated` seam

`canvas_replay_from()` reads a text *canvas program* (one `canvas_*` command per
line) and replays it; the parser ([src/cnvs_replay.c](../src/cnvs_replay.c)) is
deliberately a hostile-input target, so it's a good probe of what `-fbounds-safety`
feels like for the classic C minefield of tokenizing untrusted text.

**The ease (most of it).** Parsing *by index* over a `char const *__counted_by(len)`
buffer is friction-free and pleasant: the cursor is a `size_t`, every `data[i]`
is bounds-checked against `len` for free, and a line is just a `[start, end)`
slice. Comparing a token to a literal, scanning to whitespace, the strict
"unknown command / wrong arity → reject" structure — all of it is ordinary C that
needed *zero* annotations beyond the one `__counted_by` on the entry pointer. The
DoS guard (cap the line length) is a plain `if`. This is the feature at its best:
the dangerous loop is the indexed buffer walk, and it's checked with no effort.

**The friction (the C-library boundary).** Text parsing *forces* you across the
`__null_terminated` seam, because that's how libc is annotated:
- `strtof`'s first argument is `__null_terminated`; a `__bidi_indexable` cursor
  won't pass without `__unsafe_null_terminated_from_indexable()` (a linear scan)
  or a forge.
- `memcpy`'s source is `__sized_by`, so copying *out of* a `__null_terminated`
  string needs `__null_terminated_to_indexable()` first.
- a `__terminated_by` (i.e. `__null_terminated`) pointer **can't be subscripted**
  (`lit[k]` is an error — walk it with `*lit`/`lit++`) and **can't be offset by
  more than one** (`p + n` is an error — check `*end` instead of `end == p + n`).

None of these are hard once you know them, but they're invisible until the
compiler stops you. The first cut took the pragmatic route — keep the bulk of the
parser indexable and **confine the seam crossings to two leaf forges** (a copied,
NUL-terminated numeric token handed to `strtof`; a copied text tail handed to
`fill_text`). That's the honest, idiomatic answer: a forge is C's `unsafe {}`, two
one-liners asserting an invariant the type system can't see.

But the seam is avoidable entirely, and removing both forges is instructive:
- **Numbers:** drop `strtof` for a hand float parser that reads the token *in
  place by index* (sign, digits, `.`fraction, `e`exponent). It never builds a
  C string, so it never touches `__null_terminated`. It's also *stricter* than
  `strtof` (rejects `1.5.2`, trailing junk, hex/`inf`/`nan`), which is the right
  posture for untrusted input. (Its scaling now steps through *exact* powers of
  ten, so every `%.9g` the recorder emits reparses to the identical float32 —
  the text-block format made that round-trip a correctness requirement; see
  [text-boundary.md](text-boundary.md).)
- **Text:** give the engine a length-counted `canvas_fill_text_n` /
  `stroke_text_n` (`__counted_by(len)`), and the parser hands the tail straight
  through as a slice — `data + j` carries its own remaining count — with no copy,
  no NUL, no forge. The NUL-terminated `fill_text` stays as a convenience that
  delegates via the *safe* `__null_terminated_to_indexable`.

Reaching that length-counted API also forced the Core Text shim length-bounded
(it had stopped at a NUL), which **hardened the one unchecked TU** as a
side effect: it can no longer over-read a non-terminated buffer. So chasing zero
forges in the *checked* parser paid off in the *unchecked* shim — a nice
demonstration that the annotation pressure propagates somewhere useful.

**One gotcha worth flagging:** a parameter used *only* inside a `__counted_by(n)`
annotation reads as unused in the `unsafe` variant (where the macro expands to
nothing) and trips `-Werror=unused-parameter` — a build that's green under
`-fbounds-safety` can fail without it. The fix doubles as defense-in-depth: use
the bound in a real check (`if (ts + tlen > le) return false;`), which also
guards the unchecked `unsafe`/fuzz build where `__counted_by` is absent.

Net: `-fbounds-safety` made the *parsing* (the actual attack surface) safe for
free. The libc string boundary is the only place it pushes back — and even that
is optional: with a hand float parser and a length-counted text API, the parser
reaches **literally zero forges and zero `__null_terminated`**, staying entirely
in the indexable world. Fuzzed under ASan+UBSan with nothing found.

### The other direction: the *recorder* is strictly easier (zero forges)

`canvas_record_to()` ([src/cnvs_record.c](../src/cnvs_record.c)) is the write-side
inverse: it installs a recorder on the canvas, and each recordable public op
appends its line, so a live session is serialized to the same text format the
parser reads. Round-tripping is pinned by [tests/test_record.c](../tests/test_record.c)
two ways — replaying the file is *pixel-identical*, and replaying-while-recording
reproduces the file *byte-for-byte* (a drift guard on every command's spelling).

The striking thing is the **asymmetry**: writing crosses the same libc seam as
reading, but in the easy direction, so the recorder needs **no forges at all**.
Emitting is `__counted_by(n)` float runs (`v[i]`, bounds-checked for free) and
`__null_terminated` command names / text handed *straight to* `fputs`/`fprintf` —
and libc's sinks already *want* `__null_terminated`, so the conversion the parser
had to forge its way *out of* is exactly the direction the writer flows *into*,
for free. Parsing forces `indexable → __null_terminated` (no safe conversion, hence
the forges); emitting only ever does `__null_terminated → libc`, which type-checks.
Producing well-formed text is the safe direction; consuming hostile text is the
hard one — `-fbounds-safety` makes that split explicit.

The one genuinely interesting bit isn't bounds-safety at all but **re-entrancy**:
the public API composes (`arc` calls `ellipse`; `round_rect` calls `move_to`/`arc`/
`close_path`; `arc_to` calls `line_to`/`arc`), so a naive top-of-function hook
would record an op *and* its expansion and replay it twice. The fix is a
reference-counted suspend: a compound op writes its own line, then brackets its
sub-calls with `enter`/`leave` so they don't also record — the file keeps the op
the caller issued, and replay re-invokes the *same* function (bit-identical),
rather than relying on a decomposition staying equivalent. `arc_to` has early
returns, so its body moved to a single-exit `arc_to_impl` and the public wrapper
guarantees `leave` always balances `enter` (no `__attribute__((cleanup))`, which
`-fbounds-safety` rejects anyway).

## Regrets / things we'd reconsider

- **Hand-rolled per-type vectors.** `cnvs_verts` and the `cnvs_cover`/clip-mask
  buffers are copy-paste-shaped growable arrays. A generic macro would remove the
  duplication, but generic containers interact awkwardly with `-fbounds-safety`
  (you can't take the address of a `__counted_by` field and pass it around as
  `void**` without breaking the pointer/count coupling the compiler enforces), and
  macro-defined containers risk `-Wunused-macros` noise. Concrete types stayed
  clearer.
- **Full-canvas clip masks.** A clip is one coverage byte per canvas pixel, and
  `save()` deep-copies it. Correct and simple, but heavier than a bounding-box or
  reference-counted mask would be. Fine at the scales here.
- **Coverage rasterized over the whole bbox.** Even a thin shape allocates and
  resolves a full bounding-box coverage tile. A run-based or active-edge variant
  would touch fewer pixels; the accumulation approach is chosen for clarity and
  for being sort-free.

## Aspirations

- Richer text — complex shaping (we map code point → glyph 1:1, so no ligatures,
  contextual forms, or bidi), a glyph cache (outlines are re-fetched per
  `fill_text`), and text alignment/baselines beyond the default.

## Rules of thumb (the cheat-sheet we wish we'd had)

1. Dynamic arrays live in a struct: `T *__counted_by(cap) data; int cap;`. Update
   `data` and `cap` together after `realloc`.
2. Counted locals: only with a **`const`** count declared right next to the
   pointer. Otherwise compute sizes on a plain `int` and store into a struct.
3. Pass arrays as `(T *__counted_by(n) p, int n)` parameters — the smoothest path.
4. Slice with `base + offset`; it keeps bounds and re-checks at the callee.
5. Annotate handles to incomplete types as `__single` (the compiler will remind
   you).
6. `#include <ptrcheck.h>` in every header that uses the macros.
7. The unsafe boundary should be small, named, and obvious — here, two shims (a
   Metal `.m`, a Core Text `.c`), each behind a bounds-safe C ABI.
8. Generic callback APIs (`qsort`, `bsearch`) lose the bounds at the callback —
   forge inside, or hand-roll the small hot ones to stay checked end to end.
9. Binding an un-adopted system header from checked code fights the strict
   function-pointer check on its callbacks (no spelling matches an attribute-free
   typedef); when the gain is only cosmetic, an isolated unchecked shim is cleaner
   than scattering forges. A C shim keeps the debug sanitizers; an ObjC one can't.
