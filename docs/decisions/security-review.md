# canvas2d — security assessment (focus: is `-fbounds-safety` actually holding?)

> **Point-in-time review, preserved as a decision record (2026-06-10).** This was
> a single security pass over the established C core, framed as "where does
> `-fbounds-safety` hold and where does it end." It is kept here as the record of
> that pass and as a map from each finding to the living test/fuzzer that now
> guards it. It has been **lightly refreshed** to stay true to today's tree:
> file renames are fixed (`canvas2d_font_ct.c` → [`canvas2d_text_ct.c`](../../src/canvas2d_text_ct.c)),
> and Finding 3 is rewritten because the Metal backend was deleted this week (see
> [metal-backend.md](metal-backend.md) and [backend-differential.md](backend-differential.md)),
> so there is now exactly **one** unchecked boundary TU, not two. Each finding
> carries a **"Now guarded by"** line pointing at its regression guard. The
> findings themselves are not re-audited; they are a snapshot, and all seven were
> fixed at the time of the pass.

Scope: the established C core (canvas entry points, PNG encoder, image/blit,
gradients, CPU compositor) and the one unchecked boundary shim (the Core Text
font shim). The `pixvm` experiment is **excluded** (under active development by
another worktree).

The project's thesis is *spatial memory safety for C via `-fbounds-safety`*. The
review question is **"where does that guarantee hold, and where does it end?"**
The approach: define the trust boundary and the threat model *first*, then aim
tools at the seams.

---

## 1. The threat model

**Trust boundary.** The public API in [`include/canvas2d.h`](../../include/canvas2d.h)
is the boundary. Everything a caller controls is untrusted input:

| Input | Entry points | Notes |
|---|---|---|
| Canvas dimensions | `canvas2d_create(w,h)` | **no upper bound** (only `> 0`) |
| Geometry / coords | `move_to`, `bezier_curve_to`, `arc`, … | floats → path point counts grow |
| Pixel buffers + dims | `get/put_image_data`, `draw_image*`, `read_rgba` | caller passes `(ptr, len, w, h)` |
| Gradient stops | `add_*_color_stop` | fixed `CANVAS2D_MAX_STOPS` array |
| Dash pattern | `set_line_dash(ptr, count)` | `__counted_by(count)` |
| Text + font name | `fill_text`, `measure_text`, `set_font` | UTF-8 across the Core Text shim |

**What `-fbounds-safety` covers, and what it does NOT.** The
feature is *spatial only*. It converts an out-of-bounds `ptr[i]` into a
`SIGTRAP`. It does **not** cover:

1. **Integer overflow in the count/size expression itself.** `__counted_by(N)`
   checks `i < N` — but if `N` is a product of `int`s that overflowed, the check
   uses the *wrong* `N`. This is the primary way to defeat the guarantee, and it is
   present in this codebase (see Finding 1).
2. **Temporal safety** — use-after-free, double-free. (ASan covers this in the
   debug build; nothing covers it in release.)
3. **The one unchecked translation unit** — [`canvas2d_text_ct.c`](../../src/canvas2d_text_ct.c)
   (`BOUNDARY_C`, the Core Text font shim), built without `-fbounds-safety` to
   bind the un-annotated system headers. Here a wrong bound *is* corruption. ASan
   instruments it in debug; it is unchecked in release. (At the time of the
   original pass there was a second one, the Objective-C Metal compositor shim,
   built without the flag because it is C-only; that backend has since been
   deleted — see Finding 3.)
4. **Logic errors that compute a self-consistent but wrong bound** (none found
   in the established code — the `(ptr,count)` discipline holds — but it is
   the class to keep watching).

Established by reading every allocation/access pair: the core keeps each
`(pointer, count)` consistent at allocation and
access, so even when an overflowed value is used as the bound, it is used
*identically* at both ends, and the eventual access traps instead of corrupting.
That is why Finding 1 lands as "trap, not corruption" in release.

---

## 2. Findings

### Finding 1 — Integer overflow defeats the size guards in the image-data path (verified)

**Severity:** memory-safety impact is contained by `-fbounds-safety` to a
controlled trap (DoS), but it is **undefined behavior** that is *fatal in the
debug/sanitizer build* and makes the documented API contract unenforceable.
**Confidence: verified, end-to-end PoC.**
**Status: FIXED** — `canvas2d_create` clamps to `CANVAS2D_MAX_DIM`, and `rgba8_dims_ok()`
validates caller dims in 64-bit at the three image entry points
([`canvas.c`](../../src/canvas.c)); regression test [`tests/test_dims.c`](../../tests/test_dims.c).
**Now guarded by:** [`tests/test_dims.c`](../../tests/test_dims.c) — it pins
`canvas2d_create(23171, 23171) == NULL` (the genuinely dangerous wrap, see below)
*and* the `16385`/boundary-`16384` clamp, and drives `canvas2d_get_image_data`
/ `canvas2d_put_image_data` with the overflowing region to confirm the guard
rejects before any write. Under the debug variant's UBSan the whole test aborts
if the `w*h*4` arithmetic regresses to signed-int. (The original
proof-of-concept, which drove the public API as an external consumer, has been
**retired** — `test_dims.c` exercises its exact scenario, and the more dangerous
wrap-negative case, inside the gated suite.)

Multiple canvas entry points compute `width * height * 4` (and `width * height`)
in `int` before any widening:

- [`canvas.c:900`](../../src/canvas.c) `read_unpremul`: `len < cv->width * cv->height * 4`
- [`canvas.c:903`](../../src/canvas.c) `int const n = cv->width * cv->height`
- [`canvas.c:924`](../../src/canvas.c) `canvas2d_write_png`: `int const len = cv->width * cv->height * 4`
- [`canvas.c:937`](../../src/canvas.c) `canvas2d_get_image_data`: `len < w * h * 4`
- [`canvas.c:941`](../../src/canvas.c) `int const clen = cv->width * cv->height * 4`
- [`canvas.c:954`](../../src/canvas.c) `canvas2d_put_image_data`: `len < w * h * 4`

`canvas2d_create` validates only `w > 0 && h > 0` — **no upper bound** — so these
products overflow. The overflow makes the bounds guard `len < w*h*4` depend on
the specific input:

- `40000 × 40000`: `w*h*4` wraps to `+2105032704`, still `> len`, so the guard
  rejects the call.
- `23171 × 23171`: `w*h*4` wraps to `-2147386332`; `len < negative` is **false**,
  so the guard is **bypassed** and the code proceeds to blit with a too-small
  output buffer.

Verified at the time with a standalone PoC that drove the **public** API as an
external, non-bounds-safety consumer (`23171 × 23171`, 256-byte `out` buffer;
since retired in favor of the gated [`tests/test_dims.c`](../../tests/test_dims.c)):

| Build | Result |
|---|---|
| `unsafe` (feature off) + ASan | **`heap-buffer-overflow` WRITE** in `canvas2d_blit_rgba ← canvas2d_get_image_data` — a real OOB write (CWE-787) |
| `release` (`-Os`, `-fbounds-safety`) | **`SIGTRAP`, exit 133** — OOB write converted to a deterministic trap; no corruption |
| `debug` (`-fbounds-safety` + UBSan) | **fatal** at `canvas.c:937` — signed-overflow caught at the source |

**Interpretation.** This is both (a) a bug — the API's "`len`
must be `w*h*4`" contract cannot be enforced for large dimensions, and the debug
build (CI, tests, any sanitizer-based fuzzing) aborts — and (b) a demonstration of
the feature: with it off it is an exploitable heap overflow; with
it on, it is a controlled abort.

**Fix.** The PNG *encoder*
already does this — [`canvas2d_png.c:122-128`](../../src/canvas2d_png.c) clamps
`width,height ≤ 16384` and does its size math in `size_t`. Apply the same at the
root, `canvas2d_create`, and re-derive byte sizes in `size_t`/`int64_t` at the
six sites above. One bound at creation closes the class.

### Finding 2 — Unbounded doubling in `canvas2d_grow_cap` (by inspection)

**Severity:** UB; same containment as Finding 1 (trap in release, UBSan-fatal in
debug). **Confidence: medium, not PoC'd.**
**Status: FIXED** — the doubling loop in [`canvas2d_mem.c`](../../src/canvas2d_mem.c) now
guards `n > INT_MAX / 2` and falls back to the exact `need` (callers null-check
the realloc); regression test [`tests/test_mem.c`](../../tests/test_mem.c).
**Now guarded by:** [`tests/test_mem.c`](../../tests/test_mem.c) — it calls
`canvas2d_grow_cap` right at the top of the range (`INT_MAX - 1`, `1<<30`,
`INT_MAX/2 + 1`) where the unguarded `n *= 2` would have overflowed, asserting
the result never drops below `need`; UBSan-fatal in debug if the guard regresses.

[`canvas2d_mem.c:3-9`](../../src/canvas2d_mem.c): `int n = ...; while (n < need) n *= 2;` —
if `need > 2^30`, `n *= 2` overflows `int` (signed → UB). Reachable by driving a
path to hundreds of millions of points (`line_to` in a loop). The downstream
store is `__counted_by`-checked, so in release the consequence traps rather than
corrupts; in debug UBSan makes it fatal. Worth a `size_t`/saturating-growth fix
for the same reason as Finding 1: don't rely on "UB happens to trap downstream."

### Finding 3 — The one unchecked shim is the unguarded surface (audited, no bug found)

*(Rewritten 2026-06-10: the original pass found two TUs built without
`-fbounds-safety` — the Objective-C Metal compositor shim and the Core Text font
shim. The Metal backend was deleted this week, taking `compositor_metal.m` and
`shaders/compositor.metal` with it; the compositor is now
[`compositor_cpu.c`](../../src/compositor_cpu.c), plain checked core under the flag
like the rest of the renderer — no longer a boundary at all. See
[metal-backend.md](metal-backend.md) and [backend-differential.md](backend-differential.md).
That leaves **exactly one** unchecked boundary TU. The finding's lesson is
unchanged: the boundary shim is the one place a future edit has no compile-time
net, so it carries the heaviest fuzzing.)*

`-fbounds-safety` is off in exactly one place — [`canvas2d_text_ct.c`](../../src/canvas2d_text_ct.c)
(`BOUNDARY_C` in [`configure.py`](../../configure.py)), the Core Text font shim,
built without the flag because it binds the un-annotated CoreText/CoreGraphics
headers. Read closely:

- **Core Text shim**: the checked side hands every string across this boundary as
  a counted `(bytes, len)` slice — never a NUL contract — and `str_from_bytes`
  passes exactly that to `CFStringCreateWithBytes`, which reads `len` bytes and
  not one more (so the adversarial-UTF-8 case is structural, not a hand decoder
  that could walk off the end). `CGPathElement.points` is indexed by element type
  per CG's contract (move/line = 1, quad = 2, cubic = 3) in
  [`canvas2d_text_ct.c:129`](../../src/canvas2d_text_ct.c) `emit` — correct, and
  ASan-instrumented in debug. Each shaped run is copied into a checked-owned
  `canvas2d_glyph_run` array before it re-enters the checked core. **No bug found**,
  and **fuzzed**: [`fuzz/fuzz_text.c`](../../fuzz/fuzz_text.c) drives
  `measure/fill/stroke_text` with adversarial UTF-8 (truncated, lone-continuation,
  astral, mixed) under ASan, so the counted-slice property is exercised
  empirically. This TU is unchecked, so ASan is its only net.

It is small and correct today, but it is where a *future* edit has no compile-time
net, so it carries the heaviest fuzzing (Section 4).

**Now guarded by:** [`fuzz/fuzz_text.c`](../../fuzz/fuzz_text.c) (the dedicated Core
Text shim fuzzer, ASan-only — the unchecked TU's sole automated net) plus the text
unit tests ([`tests/test_text.c`](../../tests/test_text.c),
[`tests/test_measuretext.c`](../../tests/test_measuretext.c),
[`tests/test_emoji.c`](../../tests/test_emoji.c)) built under the debug variant's
`-fbounds-safety` + ASan + UBSan.

### Finding 4 — Non-finite/huge float coordinates cause `(int)`-cast UB in `points_bbox` (fuzzer-found)

**Severity:** undefined behavior (float-cast-overflow); not spatial, so
`-fbounds-safety` does **not** cover it — it is UBSan-fatal in debug and silent
in release. **Confidence: verified, fuzzer-found + reproduced.** **Status: FIXED.**
**Now guarded by:** [`tests/test_sanitize.c`](../../tests/test_sanitize.c) — it
feeds the reported finite-huge crash value plus `±INFINITY`/`NaN`/other huge
magnitudes through every float→int site (clear_rect/points_bbox, the fill
coverage rasterizer, ellipse and stroke segment counts) and the float→uint8
colour quantization, aborting under the debug variant's UBSan if any regresses;
continuously re-found by the coverage fuzzers
[`fuzz/fuzz_api.c`](../../fuzz/fuzz_api.c) (raw 4-byte floats into the path/transform
math, run under ASan+UBSan with the flag stubbed) and
[`fuzz/fuzz_ops.h`](../../fuzz/fuzz_ops.h)'s shared op stream.

The public path/rect API takes coordinates as `float` with no finiteness or range
check, and the device-space transform plus the `(int)` casts overflow on
non-finite/huge values. It is a **class spanning multiple sites**, two confirmed:
[`points_bbox`](../../src/canvas.c) at [`canvas.c:334`](../../src/canvas.c)
(`int x0 = (int)fx0; ...`) and the transform helper `xf` at
[`canvas.c:504`](../../src/canvas.c) (reached via `canvas2d_ellipse`/`draw_image`).
Found by the Role-A API fuzzer ([`fuzz/`](../../fuzz/)) — first on the second random
input (coverage-less), then re-found by the libFuzzer build with coverage in
seconds — and reproduced under the diagnostic build:

```
src/canvas.c:334:29: runtime error: 9.35078e+13 is outside the range of
                     representable values of type 'int'
    #0 points_bbox        canvas.c:334
    #1 canvas2d_clear_rect  canvas.c:418   (any fill/clip/rect path reaches it)
```

The class turned out broader than the first two sites: float→int casts also live
in the coverage rasterizer ([`canvas2d_cover.c`](../../src/canvas2d_cover.c): 56, 81, 82,
119, 120), the stroker ([`canvas2d_stroke.c`](../../src/canvas2d_stroke.c):36), and the
float→uint8 colour quantization ([`canvas.c`](../../src/canvas.c) read-back, and
`canvas2d_cover.c`:152). Stroke vertices in particular bypass the transform chokepoint
`xf` (they are width×miter offsets), so a per-entry non-finite check alone would
miss them — and the crash value was *finite*-huge anyway.

**Fix (the saturating-conversion approach):** a shared
`canvas2d_f2i`/`canvas2d_f2u8` ([`canvas2d_math.c`](../../src/canvas2d_math.c)) makes every float→int
and float→uint8 conversion in the renderer total (NaN→0, out-of-range clamps) —
this is exactly Rust's `as`-cast semantics, hand-written. Colour components clamp
to `[0,1]` at the public setters ([`canvas.c`](../../src/canvas.c) `clamp01`). The
gradient evaluator is already NaN-robust (returns a finite stop). Regression test
[`tests/test_sanitize.c`](../../tests/test_sanitize.c). (A strict per-entry no-op on
non-finite args, for exact Canvas-spec parity, remains an optional refinement on
top.)

### Finding 5 — Unbounded vertex allocation in dashed stroking (fuzzer-found)

**Severity:** resource-exhaustion DoS (not memory corruption — the `realloc` is
null-checked, so it degrades to a no-op if the allocation fails). **Confidence:
verified, fuzzer-found.** **Status: FIXED.**
**Now guarded by:** [`tests/test_sanitize.c`](../../tests/test_sanitize.c) — its
Finding-5 case strokes a sub-pixel (`1e-4`) dash over a 1e6-long segment, which
without the span cap drove the ~2 GB `realloc`; it now returns promptly instead
of spinning/OOMing. The dash path is also exercised continuously by
[`fuzz/fuzz_api.c`](../../fuzz/fuzz_api.c) (`OP_SET_LINE_DASH` / `OP_SET_DASH_OFFSET`
with fuzzed spans).

A pathological dashed stroke drives the vertex buffer to ~2^28 elements (a ~2 GB
`realloc`): `canvas2d_stroke_dashed` → `emit_quad` → `canvas2d_verts_tri` →
`verts_reserve` ([`canvas2d_geom.c:12`](../../src/canvas2d_geom.c)). Surfaced by libFuzzer
*after* the Finding 4 fix unlocked deeper coverage (`malloc(2147483648)`). With
[[Finding 2]] fixed the growth no longer overflows, so it cleanly reaches the OOM
rather than wrapping. **Fix:** a span cap in `canvas2d_stroke_dashed`
([`canvas2d_stroke.c`](../../src/canvas2d_stroke.c)) bounds the inner dash loop (it also
stops the CPU spin from the "off" spans), truncating a pathological dash after a
bounded amount instead of allocating ~2 GB. Regression test in
[`tests/test_sanitize.c`](../../tests/test_sanitize.c).

### Finding 6 — Infinite loop in `canvas2d_ellipse` angle normalization (fuzzer-found)

**Severity:** non-termination DoS (hang). **Confidence: verified, found by the
Finding-4 regression test.** **Status: FIXED.**
**Now guarded by:** [`tests/test_sanitize.c`](../../tests/test_sanitize.c) — the
same loop that feeds `±INFINITY`/`-3e30`/huge end angles into `canvas2d_ellipse`
(the input that originally hung); it now returns instead of spinning. The arc/
ellipse path is also fuzzed via [`fuzz/fuzz_api.c`](../../fuzz/fuzz_api.c)
`OP_ELLIPSE` with raw-float angles.

[`canvas2d_ellipse`](../../src/canvas.c) normalized the sweep with
`while (sweep < 0) sweep += 2π` / `while (sweep > 0) sweep -= 2π`. For a
huge-magnitude or infinite angle the step falls below the float ULP (and `±inf`
never crosses zero), so the loop never terminates — a hang on any thread that
strokes/fills such an arc. Found when `test_sanitize` passed `-3e30`/`-INFINITY`
as an end angle and hung (the test doing its job). **Fix:** fold the sign
correction in one step with `floorf`/`ceilf` and no-op on a non-finite sweep —
behaviour-identical for finite inputs, O(1), can't hang. Covered by
[`tests/test_sanitize.c`](../../tests/test_sanitize.c).

Note (vs. Rust): neither Finding 5 nor Finding 6 is a memory-safety bug, and
**neither Rust nor `-fbounds-safety` catches them** — termination and resource
bounds are outside both. They are logic bugs that only fuzzing (or timeouts /
allocation limits) surfaces.

### Finding 7 — Signed-overflow UB in the replay parser's float exponent (review-found)

**Severity:** UB / potential DoS, not memory-unsafe. **Confidence: verified by
inspection.** **Status: FIXED.**
**Now guarded by:** [`tests/test_replay.c`](../../tests/test_replay.c) — it replays
`move_to 0 1e99999999999` (saturates to +inf), `line_to 0 1e-99999999999`
(saturates to 0), and `set_global_alpha 1e2000000000`, each of which overflowed
the pre-clamp `int eexp` accumulator and is UBSan-fatal in the debug variant if
the saturation regresses; the replay parser is also fuzzed under ASan+UBSan by
[`fuzz/fuzz_replay.c`](../../fuzz/fuzz_replay.c) with exponent-seeded inputs.

The hand float parser added when [`canvas2d_replay.c`](../../src/canvas2d_replay.c) went
forge-free accumulated the decimal exponent with `eexp = eexp*10 + d` and no
bound. A numeric token like `1e99999999999` (within the line cap, and accepted by
the strict whole-token check) overflows `int eexp` — signed-overflow **UB** —
with follow-on `-eexp` / `-e` UB at `INT_MIN` and, in a non-trapping build, a
scale loop that can spin up to ~`INT_MAX`. Not a spatial bug (`-fbounds-safety`
still holds), but it traps under the debug variant's UBSan and is a clean DoS
vector; it was latent only because nothing exercised it (the corpus never mutated
a numeric field into that shape). **Fix:** saturate the exponent magnitude during
accumulation (`if (eexp < 1000) ...`) — any `|exp|` past a few dozen already
over/underflows float32 to inf/0, so the result is unchanged while the overflow,
the bad negation, and the unbounded loop all disappear. Covered by a long-exponent
case in [`tests/test_replay.c`](../../tests/test_replay.c) (pinned under UBSan) and
re-fuzzed clean (~12k execs, ASan+UBSan, with exponent-seeded inputs).

This one *is* a `-fbounds-safety` blind spot worth naming: bounds-safety makes the
parser spatially sound for free, but arithmetic UB on untrusted numbers is still
on you — the same class Rust would catch only in debug (overflow panics) and
silently wrap in release.

### Non-findings (checked, clean)

- **PNG encoder** ([`canvas2d_png.c`](../../src/canvas2d_png.c)): every byte goes through a
  cursor (`put8 → buf[at]`) into a
  pre-sized `__counted_by(cap)` buffer, so *any* size-estimate error traps at the
  write instead of corrupting the heap; a too-large estimate fails the final
  `w.at == total` check. Checked specifically for an alloc-size vs.
  write-extent divergence; found none. Now **fuzzed**:
  [`fuzz/fuzz_png.c`](../../fuzz/fuzz_png.c) drives `canvas2d_png_write` on fuzzed
  dimensions + pixels with `-fbounds-safety` off, so ASan alone must witness the
  cursor stays in bounds — 83k execs, **clean**.
- **Temporal safety of `save`/`restore` clip masks**
  ([`canvas.c:151-180`](../../src/canvas.c)): `save` deep-copies the mask into the
  stack entry; `restore` frees `cur`'s then adopts the stack entry's. No alias,
  no double-free. (Not covered by bounds-safety; ASan would catch a regression.)
- **Gradient stops**: fixed `CANVAS2D_MAX_STOPS` array, `add_stop` guards the count.
- **CPU compositor** ([`compositor_cpu.c`](../../src/compositor_cpu.c)): fully
  checked; keeps `target/__counted_by(tn)` consistent, so overflowed dimensions
  trap on access rather than corrupt.
- **Text-program parser** ([`canvas2d_replay.c`](../../src/canvas2d_replay.c), behind the new
  public `canvas2d_replay_from`): a fresh untrusted-input surface (tokenizing,
  number parsing, line handling). Parsed by index over a `__counted_by(len)`
  buffer with a line-length cap and strict rejection, and it reaches **zero forges
  and zero `__null_terminated`** — numbers go through an in-place hand float parser
  (no `strtof`) and the text tail is passed as a `__counted_by` slice to the
  length-counted `canvas2d_*_text_n` (no copy/NUL/forge). Fuzzed by
  [`fuzz/fuzz_replay.c`](../../fuzz/fuzz_replay.c) under ASan+UBSan, **clean**, plus a
  round-trip + malformed-rejection test ([`tests/test_replay.c`](../../tests/test_replay.c)).
  The `-fbounds-safety` ease/friction is written up in
  [docs/bounds-safety.md](../bounds-safety.md).

- **Text-program recorder** ([`canvas2d_record.c`](../../src/canvas2d_record.c), behind the
  new public `canvas2d_record_to`): the write-side inverse of the parser — each
  recordable public op appends its line as it runs. No new *untrusted-input*
  surface (it emits, it doesn't consume), and notably **zero forges**: emission is
  `__counted_by(n)` float runs plus `__null_terminated` names/text handed straight
  to `fputs`/`fprintf`, the easy direction across the libc seam (consuming hostile
  text is the hard direction). The only subtlety is re-entrancy — compound ops
  (`arc`/`round_rect`/`arc_to`) record themselves and a reference-counted suspend
  swallows the public sub-calls they make, so the open/close and `enter`/`leave`
  stay balanced (verified leak-clean). Round-trip is pinned by
  [`tests/test_record.c`](../../tests/test_record.c): replay is pixel-identical and
  re-recording is byte-identical. Write-up in
  [docs/bounds-safety.md](../bounds-safety.md).

---

## 3. Tooling — what is needed, and the constraint on this machine

| Tool | Status here | Use |
|---|---|---|
| **ASan** | ✅ Apple + Homebrew clang | temporal bugs + the OOB the feature would trap; the oracle for the non-`-fbounds-safety` build |
| **UBSan** (`integer,undefined`) | ✅ both | **the** tool for the integer/float-cast classes (Findings 1, 2, 4) — catches the UB at its source |
| **`-fbounds-safety`** | ✅ Apple clang 21 only | the release oracle: OOB → trap |
| **libFuzzer** | ✅ **via Homebrew clang** (runtime bundled) | coverage-guided, **in-process, rootless** — the chosen engine |
| **AFL++** | ⚠️ installed, but **rejected** | coverage needs `sudo afl-system-config` (SysV shm); root not used here |

After trying both engines: Apple clang lacks the libFuzzer
runtime, and AFL++ on macOS needs root to raise the SysV shm limits — but
**Homebrew clang ships libFuzzer**, which runs in-process with no shm/forkserver/
sudo. Since `-fbounds-safety` is Apple-clang-only (Homebrew clang rejects it), the
duties split (Section 4):

1. **Discovery** — Homebrew clang + libFuzzer + ASan + UBSan, CPU backend, no
   `-fbounds-safety` (annotations vanish via a stub `ptrcheck.h`). Coverage-guided,
   diagnostics inline. This is [`fuzz/`](../../fuzz/); it found Finding 4.
2. **Confirmation** — replay each crasher under the Apple-clang `-fbounds-safety`
   build to confirm an OOB-write class becomes a deterministic trap.

---

## 4. The differential method

Because the subject is the feature itself, the most informative oracle is
**differential across the existing variants**, run on one shared corpus:

```
        unsafe + ASan   →  finds the real OOB/UAF (discovery)
        debug (UBSan)   →  catches the integer-overflow ROOT CAUSE
        release (-fbs)  →  confirms OOB becomes a controlled trap (the thesis)
```

A finding is significant when the three disagree. Finding 1 is the template:
ASan-overflow in `unsafe`, UBSan-fatal in `debug`, `SIGTRAP` in `release`.
Cataloguing every such divergence maps where the mechanism holds
and where its guarantees stop.

### Harnesses to write (priority order) — since built

The four harnesses the pass recommended now live in [`fuzz/`](../../fuzz/):

1. **API state-machine fuzzer** — fuzz bytes → a sequence of canvas calls
   (random `create` dims incl. huge, paths, `get/put_image_data` with mismatched
   `len/w/h`, gradients, dashes, text). Built as [`fuzz/fuzz_api.c`](../../fuzz/fuzz_api.c)
   (+ [`fuzz/fuzz_state.c`](../../fuzz/fuzz_state.c) over the shared
   [`fuzz/fuzz_ops.h`](../../fuzz/fuzz_ops.h) op stream). Highest coverage of the
   trust boundary; it re-finds Finding 4.
2. **`canvas2d_png_write` harness** — [`fuzz/fuzz_png.c`](../../fuzz/fuzz_png.c):
   dimensions + pixel buffer in, ASan witnesses the cursor never leaves `total`.
3. **Core Text shim harness** — [`fuzz/fuzz_text.c`](../../fuzz/fuzz_text.c):
   valid + invalid UTF-8 → `measure_text`/`fill_text`/`stroke_text`. This is the
   one unchecked TU; ASan is its only net (Finding 3).
4. **PNG decode / inflate harnesses** — [`fuzz/fuzz_pngdec.c`](../../fuzz/fuzz_pngdec.c)
   and [`fuzz/fuzz_inflate.c`](../../fuzz/fuzz_inflate.c) cover the decode side of
   the codec arithmetic the original `canvas2d_blit_rgba` clamp idea pointed at
   ([`canvas2d_image.c:9-21`](../../src/canvas2d_image.c) remains the off-by-one-prone clip
   math to keep an eye on).

The original `overflow_poc.c` that seeded harness #1 has been **retired**: its
exact scenario (`canvas2d_get_image_data` with `int`-overflowing `w*h*4`) is now a
gated regression case in [`tests/test_dims.c`](../../tests/test_dims.c), which pins
the more dangerous wrap-negative dimension (`23171 × 23171`) inside the suite
rather than as standalone, never-compiled dead code.
```sh
# reproduce the Finding 1 class today (from repo root, after `python3 configure.py`):
ninja build/debug/test_dims    # debug variant: -fbounds-safety + ASan + UBSan
./build/debug/test_dims        # UBSan-fatal if the w*h*4 guard regresses to signed int
```

## 5. Temporal safety: tooling and review

`-fbounds-safety` is spatial-only; there is **no temporal analog for C**. Clang's
Lifetime Safety analysis is the nearest effort but it is C++-only and experimental
([LifetimeSafety.html](https://clang.llvm.org/docs/LifetimeSafety.html)). With no
threading yet, TSan is out of scope. So temporal safety here is *assembled* from
detection + discipline, not enforced by a type system. What we added:

- **`ninja analyze`** — the Clang Static Analyzer (`unix.Malloc`: path-sensitive
  use-after-free / double-free / leak) over the checked C, `-analyzer-werror` to
  gate. The closest thing to *static* temporal checking. Scoped to memory-safety
  checkers (dead-store style noise dropped). **0 findings** across the core.
- **Strengthened debug ASan** — added `-fsanitize-address-use-after-scope` and
  `-fsanitize-address-use-after-return=always` to widen *temporal* dynamic
  coverage (stack use-after-scope/return), paired with the existing fuzzing.
- **`ninja leakcheck`** — runs the non-ASan `release` build under the macOS
  `leaks` tool, because **LeakSanitizer is broken on Apple-Silicon macOS**
  (libobjc false positives; `detect_leaks` unusable —
  [llvm#115992](https://github.com/llvm/llvm-project/issues/115992)). `tests/test_leak.c`
  churns the ownership-transfer paths (save/restore clip-mask copies, gradients,
  image-data round-trips, the font cache). **0 leaks.**

All three are in `all`, so a bare `ninja` runs them; both new gates are idempotent.

**Review (interprocedural, which the analyzer can't follow):** clip-mask
save→restore→destroy (deep-copies, no alias, no double-free) and the font cache
(`ensure_font`: destroy-then-recreate, NULL-safe, callers guard NULL) are clean.

**Limitation found — `__attribute__((cleanup))` does not compose with
`-fbounds-safety`.** The RAII-lite scope-bound-free idiom (a standard C
temporal-safety mechanism) needs `&local`, and the address of a *checked* local
pointer has a flavor (`T *__bidi_indexable *…`) that no `cleanup` function
signature can match — every spelling mismatches. So that prevention technique is
unavailable under the flag; temporal hygiene rests on the single-owner / single-
free discipline plus the `leakcheck` + `analyze` gates. (Same family as the
`&__counted_by` friction noted in docs/bounds-safety.md.)
