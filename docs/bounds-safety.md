# Building with `-fbounds-safety`: what worked, what fought back

This is a field report from writing the `canvas2d` core — ~1.3k lines of C23 —
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

**Sorting a `__counted_by` array: the generic-callback gap.** The scanline fill
sorts its edge table. `qsort(edges->data, len, sizeof, cmp)` *works* — the counted
pointer converts to the `void *` the SDK wants (always allowed) — but the bounds
evaporate inside the comparator, which receives bare `const void *` arguments;
you `__unsafe_forge_single` each back to a `cnvs_edge const *`. qsort's own element
swaps are unchecked too (you're trusting `nmemb`). So the array is checked
everywhere *except the sort itself*. By contrast the per-scanline crossings use a
hand-rolled insertion sort over the `__counted_by` array, where every
`data[j]`/`data[j+1]` stays checked. The lesson: any generic callback API
(`qsort`, `bsearch`, …) is a hole exactly at the callback boundary — for the small,
hot ones, writing your own keeps the checking end to end.

**Allocation needs the `void*` conversion.** `T *p = malloc(...)` assigns a
`void*` to a typed pointer; `-Weverything` flags that under
`-Wimplicit-void-ptr-cast` (a C++-compat lint). Either cast explicitly or, as we
chose, disable that one warning for a C-only project. Crucially this is *not* a
bounds-safety hole: assigning an undersized allocation to a `__counted_by(n)`
target still traps at runtime (we verified — exit 133). The size check is
independent of the cast diagnostic.

## What it costs

We measure this directly: the `release` and `unsafe` builds are identical `-Os`
sources differing only in `-fbounds-safety`, and `ninja benchcmp` runs hyperfine
over each CPU-only kernel **in isolation** plus an end-to-end run. A recent run:

| phase | overhead |
|---|---|
| cubic-Bézier flattening | 1.07× |
| stroke expansion | 1.22× |
| PNG encode | 1.27× |
| scanline fill | 1.47× |
| 2D RGBA8 blit | **2.43×** |
| end-to-end | 1.27× |

The isolation matters. The end-to-end 1.27× is a blend that hides a wide spread
between phases — and a regression in a fast phase could disappear into it. What
the spread shows:

- **The 2D blit pays the most (~2.4×)** because it is *only* checked indexing:
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
plain-pointer ABI, the C core and the Objective-C Metal shim share `gpu.h`
verbatim. The shim is (currently) compiled without `-fbounds-safety`, so the
annotations there expand to nothing — and that's *sound*, not a fudge, precisely
because the representations match. `gpu_draw_solid(gpu*, gpu_vert const
*__counted_by(count), int count, ...)` is a checked call on the C side and an
ordinary pointer-and-length on the ObjC side. No shims, no marshalling.

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
to check there: the backend forwards already-bounds-checked vertices and pixels
straight to Metal as `void*`; all the CPU buffer logic lives in the C core,
which is already fully covered.

The principled conclusion — and the design we keep — is a 100% bounds-safe C
core with a small, explicit, **isolated** Objective-C boundary (one ~190-line
`.m` file). That isolation is not a limitation to apologise for; it's where the
unsafe platform edge belongs, named and contained. `-fbounds-safety`'s value is
in the code that actually manipulates memory, and that is all in C.

## Regrets / things we'd reconsider

- **Two nearly identical 2D point types.** `cnvs_vec2` (math) and `gpu_vert`
  (the GPU ABI) are both `{float x, y;}`. Keeping them separate avoids coupling
  the math layer to the rendering ABI, but it costs a few trivial conversions.
  Defensible, mildly annoying.
- **Hand-rolled per-type vectors.** `cnvs_verts`, `cnvs_edges`, `cnvs_xings` are
  copy-paste-shaped. A generic macro would remove the duplication, but generic
  containers interact awkwardly with `-fbounds-safety` (you can't take the
  address of a `__counted_by` field and pass it around as `void**` without
  breaking the pointer/count coupling the compiler enforces), and macro-defined
  containers risk `-Wunused-macros` noise. Concrete types stayed clearer.
- **Correctness-first GPU submission.** One command buffer per draw with
  `waitUntilCompleted` is simple and obviously correct, and useless for
  throughput. The perf story needs batching.
- **The fill is geometry-heavy.** The scanline rasterizer emits roughly
  `rows × spans` quads rather than a minimal triangulation — correct for every
  winding case, but a lot of triangles. Span coalescing across rows, or an
  incremental active-edge table, would cut both the geometry and the per-row work.
- **Stroke shortcuts.** `stroke()` does bevel joins / butt caps with inner-side
  double-cover — an honest scope cut, documented in the header, not a bug.

## Aspirations

- **Anti-aliasing** (MSAA), miter/round joins, gradients, clipping.
- **Batched GPU submission** — one command buffer per frame instead of per draw.
- Images: `drawImage` / `getImageData` (2D strided blits — the next rich
  bounds-safety target).

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
