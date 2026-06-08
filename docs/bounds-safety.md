# Building with `-fbounds-safety`: what worked, what fought back

This is a field report from writing the `canvas2d` core — ~2k lines of C23 —
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
| gradient eval | 1.00× |
| cubic-Bézier flattening | 1.07× |
| PNG encode | 1.08× |
| end-to-end | 1.10× |
| stroke expansion | 1.11× |
| analytic coverage fill | 1.16× |
| 2D RGBA8 blit | **2.55×** |

The isolation matters. The end-to-end ~1.1× is a blend that hides a wide spread
between phases — and a regression in a fast phase could disappear into it. What
the spread shows:

- **The 2D blit pays the most (~2.5×)** because it is *only* checked indexing:
  four byte loads and four stores per pixel across two buffers, with no arithmetic
  between them to amortize the checks. This is the canonical C buffer-bug
  pattern — and the strongest case for having the checks at all.
- **Flattening is nearly free (~7%)**: lots of float arithmetic (de Casteljau
  midpoints, the flatness test) between a handful of indexed pushes, so the checks
  are noise next to the FLOPs.
- Real canvas rendering is **GPU-bound**, so the end-to-end cost of safety is
  smaller still — but these are the honest prices on the hottest pure-C kernels.
  The compiler elides checks it can prove redundant; what's left is the cost of the
  ones it can't, and that cost is very workload-dependent.

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
to check there: the compositor forwards already-rendered RGBA16F tiles straight to
Metal as `void*`; all the geometry, coverage, gradient, and clip logic lives in
the C core, which is already fully covered.

The principled conclusion — and the design we keep — is a 100% bounds-safe C
core with a small, explicit, **isolated** Objective-C boundary (one `.m` file
that does nothing but blend tiles). That isolation is not a limitation to
apologise for; it's where the unsafe platform edge belongs, named and contained.
`-fbounds-safety`'s value is in the code that actually manipulates memory, and
that is all in C.

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

- Text — glyph rasterization and an atlas, the remaining Canvas 2D pillar.

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
7. The unsafe boundary should be small, named, and obvious — one `.m` file here.
8. Generic callback APIs (`qsort`, `bsearch`) lose the bounds at the callback —
   forge inside, or hand-roll the small hot ones to stay checked end to end.
