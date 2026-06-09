# canvas2d — security assessment (focus: is `-fbounds-safety` actually holding?)

Scope: the established C core (canvas entry points, PNG encoder, image/blit,
gradients, CPU compositor) and the two unchecked boundary shims. The `pixvm`
experiment is **excluded** (under active development by another worktree).

The project's thesis is *spatial memory safety for C via `-fbounds-safety`*. So
the review question isn't "are there crashes" — it's **"where does that
guarantee actually hold, and where does it end?"** That framing is also the
answer to "where does a security researcher start": build the trust boundary and
the threat model *first*, then aim tools at the seams.

---

## 1. Where a security researcher starts: the threat model

**Trust boundary.** The public API in [`include/canvas.h`](../include/canvas.h)
is the boundary. Everything a caller controls is untrusted input:

| Input | Entry points | Notes |
|---|---|---|
| Canvas dimensions | `canvas_create(w,h)` | **no upper bound** (only `> 0`) |
| Geometry / coords | `move_to`, `bezier_curve_to`, `arc`, … | floats → path point counts grow |
| Pixel buffers + dims | `get/put_image_data`, `draw_image*`, `read_rgba` | caller passes `(ptr, len, w, h)` |
| Gradient stops | `add_*_color_stop` | fixed `CNVS_MAX_STOPS` array |
| Dash pattern | `set_line_dash(ptr, count)` | `__counted_by(count)` |
| Text + font name | `fill_text`, `measure_text`, `cnvs_font_create` | UTF-8, `__null_terminated` |

**What `-fbounds-safety` covers, and what it does NOT.** This is the crux. The
feature is *spatial only*. It converts an out-of-bounds `ptr[i]` into a
`SIGTRAP`. It does **not** cover:

1. **Integer overflow in the count/size expression itself.** `__counted_by(N)`
   checks `i < N` — but if `N` is a product of `int`s that overflowed, the check
   uses the *wrong* `N`. This is the #1 way to defeat the guarantee, and it is
   present in this codebase (see Finding 1).
2. **Temporal safety** — use-after-free, double-free. (ASan covers this in the
   debug build; nothing covers it in release.)
3. **The two unchecked translation units** — `compositor_metal.m` (Objective-C,
   flag is C-only) and `cnvs_font_ct.c` (`BOUNDARY_C`). Here a wrong bound *is*
   corruption. ASan instruments the C one in debug; neither is checked in release.
4. **Logic errors that compute a self-consistent but wrong bound** (didn't find
   one in the established code — the `(ptr,count)` discipline is good — but it's
   the class to keep watching).

The good news, established by reading every allocation/access pair: the core is
**disciplined about keeping each `(pointer, count)` consistent at allocation and
access**, so even when an overflowed value is used as the bound, it's used
*identically* at both ends, and the eventual access traps instead of corrupting.
That is exactly why Finding 1 lands as "trap, not corruption" in release.

---

## 2. Findings

### Finding 1 — Integer overflow defeats the size guards in the image-data path (verified)

**Severity:** memory-safety impact is contained by `-fbounds-safety` to a
controlled trap (DoS), but it is **undefined behavior** that is *fatal in the
debug/sanitizer build* and makes the documented API contract unenforceable.
**Confidence: verified, end-to-end PoC.**
**Status: FIXED** — `canvas_create` clamps to `CANVAS_MAX_DIM`, and `rgba8_dims_ok()`
validates caller dims in 64-bit at the three image entry points
([`canvas.c`](../src/canvas.c)); regression test [`tests/test_dims.c`](../tests/test_dims.c).

Multiple canvas entry points compute `width * height * 4` (and `width * height`)
in `int` before any widening:

- [`canvas.c:900`](../src/canvas.c) `read_unpremul`: `len < cv->width * cv->height * 4`
- [`canvas.c:903`](../src/canvas.c) `int const n = cv->width * cv->height`
- [`canvas.c:924`](../src/canvas.c) `canvas_write_png`: `int const len = cv->width * cv->height * 4`
- [`canvas.c:937`](../src/canvas.c) `canvas_get_image_data`: `len < w * h * 4`
- [`canvas.c:941`](../src/canvas.c) `int const clen = cv->width * cv->height * 4`
- [`canvas.c:954`](../src/canvas.c) `canvas_put_image_data`: `len < w * h * 4`

`canvas_create` validates only `w > 0 && h > 0` — **no upper bound** — so these
products overflow. The overflow turns the bounds guard `len < w*h*4` into a coin
flip on the input:

- `40000 × 40000`: `w*h*4` wraps to `+2105032704`, still `> len`, so the guard
  *accidentally* rejects the call. Safe by luck.
- `23171 × 23171`: `w*h*4` wraps to `-2147386332`; `len < negative` is **false**,
  so the guard is **bypassed** and the code proceeds to blit with a too-small
  output buffer.

Verified with [`overflow_poc.c`](overflow_poc.c) (drives the **public** API as an
external, non-bounds-safety consumer), `23171 × 23171`, 256-byte `out` buffer:

| Build | Result |
|---|---|
| `unsafe` (feature off) + ASan | **`heap-buffer-overflow` WRITE** in `cnvs_blit_rgba ← canvas_get_image_data` — a real OOB write (CWE-787) |
| `release` (`-Os`, `-fbounds-safety`) | **`SIGTRAP`, exit 133** — OOB write converted to a deterministic trap; no corruption |
| `debug` (`-fbounds-safety` + UBSan) | **fatal** at `canvas.c:937` — signed-overflow caught at the source |

**Interpretation.** This is simultaneously (a) a genuine bug — the API's "`len`
must be `w*h*4`" contract cannot be enforced for large dimensions, and the debug
build (CI, tests, any sanitizer-based fuzzing) aborts — and (b) a clean proof the
raison d'être works: with the feature off it's an exploitable heap overflow; with
it on, it's a safe abort.

**Fix (cheap, and the codebase already knows the pattern).** The PNG *encoder*
already does the right thing — [`cnvs_png.c:122-128`](../src/cnvs_png.c) clamps
`width,height ≤ 16384` and does its size math in `size_t`. Apply the same at the
real root, `canvas_create`, and re-derive byte sizes in `size_t`/`int64_t` at the
six sites above. One bound at creation closes the whole class.

### Finding 2 — Unbounded doubling in `cnvs_grow_cap` (by inspection)

**Severity:** UB; same containment as Finding 1 (trap in release, UBSan-fatal in
debug). **Confidence: medium, not PoC'd.**
**Status: FIXED** — the doubling loop in [`cnvs_mem.c`](../src/cnvs_mem.c) now
guards `n > INT_MAX / 2` and falls back to the exact `need` (callers null-check
the realloc); regression test [`tests/test_mem.c`](../tests/test_mem.c).

[`cnvs_mem.c:3-9`](../src/cnvs_mem.c): `int n = ...; while (n < need) n *= 2;` —
if `need > 2^30`, `n *= 2` overflows `int` (signed → UB). Reachable by driving a
path to hundreds of millions of points (`line_to` in a loop). The downstream
store is `__counted_by`-checked, so in release the consequence traps rather than
corrupts; in debug UBSan makes it fatal. Worth a `size_t`/saturating-growth fix
for the same reason as Finding 1: don't rely on "UB happens to trap downstream."

### Finding 3 — The two unchecked shims are the real unguarded surface (audited, no bug found)

`-fbounds-safety` is off in [`compositor_metal.m`](../src/compositor_metal.m) and
[`cnvs_font_ct.c`](../src/cnvs_font_ct.c). I read both closely:

- **Metal shim** bounds every op (`compositor_blend` checks
  `x,y,w,h` against `width/height`; `compositor_read` checks `len`), and is
  *implicitly* bounded by Metal's own max texture dimension (≈16384) — a canvas
  larger than that fails `newTextureWithDescriptor` → `canvas_create` returns
  NULL. So the int-overflow class can't reach it via the GPU path. **No bug
  found**, but note this safety net is the *GPU driver's*, not the code's.
- **Core Text shim**: the UTF-8 decoder ([`cnvs_font_ct.c:90`](../src/cnvs_font_ct.c))
  is careful — malformed/truncated sequences stop at the NUL and never read past
  it. `CGPathElement.points` is indexed by element type per CG's contract
  (move/line=1, quad=2, cubic=3) — correct, and ASan-instrumented in debug.
  **No bug found.**

These are small and correct today, but they are where a *future* edit has no
compile-time net, so they deserve the heaviest fuzzing (Section 4).

### Non-findings (checked, clean)

- **PNG encoder** ([`cnvs_png.c`](../src/cnvs_png.c)) is the strongest example of
  the thesis: every byte goes through a cursor (`put8 → buf[at]`) into a
  pre-sized `__counted_by(cap)` buffer, so *any* size-estimate error traps at the
  write instead of corrupting the heap; a too-large estimate fails the final
  `w.at == total` check. I looked specifically for an alloc-size vs.
  write-extent divergence and found none. Solid.
- **Temporal safety of `save`/`restore` clip masks**
  ([`canvas.c:151-180`](../src/canvas.c)): `save` deep-copies the mask into the
  stack entry; `restore` frees `cur`'s then adopts the stack entry's. No alias,
  no double-free. (Not covered by bounds-safety; ASan would catch a regression.)
- **Gradient stops**: fixed `CNVS_MAX_STOPS` array, `add_stop` guards the count.
- **CPU compositor** ([`compositor_cpu.c`](../src/compositor_cpu.c)): fully
  checked; keeps `target/__counted_by(tn)` consistent, so overflowed dimensions
  trap on access rather than corrupt.

---

## 3. Tooling — what you need, and the catch on this machine

| Tool | Status here | Use |
|---|---|---|
| **ASan** | ✅ ships with Apple clang (debug build uses it) | temporal bugs + the OOB the feature would trap; the oracle for the `unsafe` build |
| **UBSan** (`integer,undefined`) | ✅ (debug build uses it) | **the** tool for Finding-1's class — catches the overflow at its source |
| **`-fbounds-safety`** | ✅ Apple clang 21 | the release oracle: OOB → trap |
| **libFuzzer** | ❌ **runtime not shipped** with this Apple clang | coverage-guided fuzzing |
| **AFL++** | ❌ not installed | alternative coverage-guided engine |

The catch: `clang -fsanitize=fuzzer` **fails to link** here —
`libclang_rt.fuzzer_osx.a` is absent from the Xcode toolchain — and there's no
Homebrew LLVM or AFL++. Instrumentation-only (`-fsanitize=fuzzer-no-link`) *does*
compile, so the gap is just the driver runtime. Two ways forward:

1. **Works today, zero install** — write standard `LLVMFuzzerTestOneInput`
   harnesses plus a ~10-line `main()` that replays a corpus through them, built
   with the project's existing debug flags (`-fbounds-safety` +
   `-fsanitize=address,integer,undefined`). No coverage feedback, but it makes
   ASan/UBSan/bounds-safety the oracle immediately and runs in CI. Drive it with
   a dumb mutator or recorded corpora.
2. **Coverage-guided** — `brew install llvm` (bundles the libFuzzer runtime) or
   `brew install afl++`. **Caveat to verify:** `-fbounds-safety` is an
   Apple/upstream-LLVM feature; confirm the Homebrew clang you install accepts it.
   If it doesn't, fuzz the **`unsafe` + ASan** build for *discovery* (ASan is the
   oracle), then **replay every crash in the `-fbounds-safety` build** to confirm
   the trap fires. That split is a feature, not a workaround — see Section 4.

---

## 4. The differential method (the high-value idea for this project)

Because the whole point is the feature, the most informative oracle is
**differential across the existing variants**, run on one shared corpus:

```
        unsafe + ASan   →  finds the real OOB/UAF (discovery)
        debug (UBSan)   →  catches the integer-overflow ROOT CAUSE
        release (-fbs)  →  confirms OOB becomes a controlled trap (the thesis)
```

A finding is *interesting* when the three disagree. Finding 1 is the template:
ASan-overflow in `unsafe`, UBSan-fatal in `debug`, clean `SIGTRAP` in `release`.
Cataloguing every such divergence is precisely "is the mechanism robust,
and where do its guarantees stop."

### Harnesses to write (priority order)

1. **API state-machine fuzzer** — map fuzz bytes to a sequence of canvas calls
   (random `create` dims incl. huge, paths, `get/put_image_data` with mismatched
   `len/w/h`, gradients, dashes, text). Highest coverage of the trust boundary.
2. **`cnvs_png_write` harness** — dimensions + pixel buffer in, assert it never
   writes outside `total` (bounds-safety already enforces this; the harness turns
   it into a fast, focused check and exercises the size arithmetic).
3. **Core Text shim harness** — random byte strings (valid + invalid UTF-8) →
   `measure_text`/`fill_text`. This is unchecked code; ASan is the only net, so
   fuzz it hardest. Structure-aware (favor multibyte lead bytes + truncation).
4. **`cnvs_blit_rgba` harness** — random `(dw,dh,dx,dy,sw,sh,sx,sy,w,h)` clip
   geometry; the clamping logic in [`cnvs_image.c:9-21`](../src/cnvs_image.c) is
   exactly the kind of off-by-one-prone arithmetic worth hammering.

The PoC in this directory ([`overflow_poc.c`](overflow_poc.c)) is the seed for
harness #1.
```sh
# reproduce Finding 1 (from repo root, after `python3 configure.py`):
ninja build/release-cpu/test_png build/debug-cpu/test_png   # builds the -cpu core objects
# then compile overflow_poc.c against build/{release,debug}-cpu/obj/*.o  (see PoC header)
```
