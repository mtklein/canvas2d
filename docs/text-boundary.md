# The text boundary under `-fbounds-safety`: shaping a glyph run

Text is where this project's real boundary to un-annotated unsafe code lives. Core
Text is a pure-C framework with no bounds annotations, so the shim that binds it
(`cnvs_font_ct.c`, and now `cnvs_shape_ct.c`) is built without `-fbounds-safety`
(`configure.py BOUNDARY_C`). The question this probe asks: as the text API grows from
"draw a string" to real shaping — RTL, ligatures, emoji, font fallback — what crosses
that boundary, and how does the flag shape it?

## Today's boundary is narrow and value-typed

`cnvs_font_ct.c` processes text *one codepoint → one glyph* (`CTFontGetGlyphsForCharacters(..., 1)`),
does all the Core Text array work internally with fixed count-1 buffers, and hands the
checked core only *finished* `cnvs_path` outlines and `float` metrics. **No glyph
array ever crosses the boundary.** That keeps the checked side forge-free, but it also
means every bit of text logic lives in the *unsafe* TU — fine while that logic is
trivial, a growing liability once it's shaping.

## Real shaping produces runtime-count runs

[../src/cnvs_shape_ct.c](../src/cnvs_shape_ct.c) shapes a UTF-8 string with Core Text
(`CTLine` → `CTRun`s) and, for each run, hands back a glyph count, the glyph ids, the
advances, and a **cluster map** (`cluster[i]` = the logical UTF-16 index in the source
for glyph `i`). Real CT output shows why each feature matters:

| input | runs | what shows up |
|---|---|---|
| `"ffi waffle"` (10 UTF-16) | 1 run, **8** glyphs | ligatures: fewer glyphs than chars, cluster *gaps* `[0 1 3 4 5 6 7 9]` |
| `"a😀b"` (4 UTF-16) | **3** runs | emoji is a fallback-font run; the cluster jumps `0→1→3` (surrogate pair) |
| Hebrew `"שלום"` | 1 run, `rtl=1` | clusters **descend** `[3 2 1 0]` — visual-order glyphs, logical indices |
| `"Hi שלום!"` | 3 runs | per-run direction; runs in visual order |

## The run crosses by `(pointer, count)` — no forge

[../src/cnvs_shape.h](../src/cnvs_shape.h) declares the run as a struct of
`__counted_by(count)` pointers plus a sibling `int count`. That struct is **the same
layout in both TUs** — `__counted_by` ties the bound to the existing `count` field and
adds no hidden member — so the unsafe shim fills it (the attribute is a no-op there)
and the checked core reads it (the attribute is enforced there), with no marshalling
in between. Every `glyph[i]`, `xadv[i]`, `cluster[i]` in [../src/cnvs_shape.c](../src/cnvs_shape.c)
is bounds-checked against the count the boundary supplied.

This is the friendliest boundary in the whole exploration:

| boundary | how data crosses | cost |
|---|---|---|
| `qsort`/`CGPathApply` callback | `void *` context | `__unsafe_forge_single` |
| tagged pointer | `uintptr_t` round-trip | `__unsafe_forge` (bound lost) |
| **shaped glyph run (C↔C)** | **`(ptr, count)` ABI** | **none** |

Two consequences worth stating:

- **Put the rich logic in the checked core.** Because a run crosses for free, layout,
  cluster mapping, RTL handling, and hit-testing belong on the checked side — the
  unsafe TU shrinks to "call CT, copy out `(ptr, count)`". The opposite of where the
  per-codepoint design was heading.
- **The counted-local gotcha is sidestepped.** A `__counted_by(n)` *local* can't be
  allocated against a parameter `n` in checked code — but here the runtime-count
  `malloc` happens in the *unsafe* shim (no flag, no gotcha), and the checked core only
  ever *reads* counted pointers it was handed. The boundary naturally lands the
  allocation on the side where the restriction doesn't apply.

## Frictions the real features introduce

- **Ligature runs are non-contiguous → the `Ptr` accessors return NULL.**
  `CTRunGetGlyphsPtr`/`StringIndicesPtr` returned `NULL` for `"ffi waffle"`. The copy
  variants (`CTRunGetGlyphs`, …) always work, so the boundary always copies — which is
  also what lets the run *outlive* the `CTLine` and become checked-owned. (Had we
  wanted zero-copy, NULL-able `Ptr` would mean `__counted_by_or_null` plus a fallback.)
- **The cluster cross-index is a checked, data-dependent index.** `text[cluster[i]]`
  is exactly the gather pattern: the flag checks it per access (it won't fold the
  range), so a *bad* cluster value from a CT bug traps cleanly instead of reading out
  of bounds. The core also range-checks `cluster[i]` against `text_len` explicitly and
  returns "no hit" rather than trapping, since a hostile or buggy map is foreseeable.
- **RTL is just data.** Glyphs arrive in visual order with descending logical clusters
  and an `rtl` flag; the checked hit-test sweeps left-to-right and reads `cluster[i]`
  uniformly — no special-casing, no new bounds concern.
- **Emoji/fallback means a *sequence* of runs.** The line is `__counted_by(nruns)`;
  iterating it is one more checked loop. Color-glyph *rendering* (not covered here)
  would need `CTFontDrawGlyphs`/bitmaps rather than the outline-path API — a different
  boundary shape, but the same `(ptr, count)` hand-off for the glyph data.

## The trust boundary

The checked core is safe *given an honest count*. If the shim reported a count larger
than the arrays it allocated, the core would read within the claimed count and not
trap — so the boundary's contract is "count matches arrays," kept trivially because the
same code mallocs and counts. Everything downstream of that — including untrusted
*values* like cluster indices — is either bounds-checked or explicitly range-checked.
So the unsafe surface for real text shaping is small and fixed (the CT calls plus the
copy), and grows with feature richness far slower than the per-codepoint design would.

## Font fallback: opaque handles, output buffers, and the string-model bridge

A mixed string shows fallback directly — `"A😀Z"` splits into three runs with fonts
**Helvetica / AppleColorEmoji / Helvetica**. Each run carries its own font so it can
be outlined or measured later, which adds two more boundary shapes.

- **An opaque handle crosses for free.** The per-run font is a `CTFontRef`, stored as
  `void *__single font` and passed back to the boundary for CT calls. It needs **no
  forge** — an opaque handle carries no bounds metadata to lose, so it isn't an
  `__unsafe_indexable` problem at all. Its only cost is *lifetime*: `CFRetain` when
  copied out of the `CTLine`, `CFRelease` in `cnvs_shaped_free`. (Contrast the tagged
  pointer, where the *value* was the bound and the round-trip destroyed it.)
- **Output buffers cross the other direction, same ABI.** `cnvs_run_font_name` hands
  the boundary `(buf, cap)` and the boundary fills within `cap` (`CFStringGetCString`
  respects it). It is the mirror of the glyph-run hand-off: `(ptr, count)` in, the
  unchecked side trusting the caller's `cap`. Bounded data crosses cleanly in *both*
  directions.
- **The string-model bridge is asymmetric.** Text is full of C strings, and this is
  where the `__null_terminated` model meets the `__counted_by` one. `strcmp`/`strlen`
  are typed `__null_terminated` (`__terminated_by(0)`); a plain indexable `char[N]`
  does **not** implicitly convert — you must assert termination with
  `__unsafe_null_terminated_from_indexable`, which the compiler can't verify. The
  conversion is one-way-safe: **null-terminated → indexable is safe** (it scans for
  the NUL within the known bounds), but **indexable → null-terminated is unsafe** (it
  trusts you that a NUL is there). The lesson for self-owned buffers: stay in the
  *sized* model — compare with the returned length + `memcmp` (which is `__sized_by`
  and converts from indexable safely) rather than reaching for `str*()`. The test does
  exactly this; `str*()` on your own buffers buys an unnecessary unsafe assertion.

## Positioned outlines: layout checked, the outline at the boundary

`cnvs_shaped_outline` renders a shaped line by walking the runs and outlining each
glyph at its pen position. The split falls out naturally:

- **Layout is checked.** The pen advance — `pen += run.xadv[i]` over the visual-order
  glyphs — runs in the core, every `xadv[i]` bounds-checked. (Visual order means the
  same left-to-right sweep places RTL runs correctly, no special case.)
- **The outline is the thin boundary op.** Per glyph, the core calls back to
  `cnvs_glyph_outline(run.font, glyph, x, y, …)`, which does the two things that need
  the un-annotated headers: `CTFontCreatePathForGlyph` and the `CGPathApply` walk. The
  opaque font handle is finally *used* here (passed back to CT), confirming the
  handle-plus-lifetime model end to end. The `CGPathApply` callback takes a `void *`
  context — the same shape that needed `__unsafe_forge_single` in checked code — but
  in the unsafe boundary TU it is just a plain `void *` cast, no forge. So the one
  genuinely awkward CT idiom (the callback) costs nothing precisely because it lives on
  the unsafe side of a boundary the run already crossed cheaply.

This is the architecture the boundary findings argued for: the index-heavy work
(layout) is checked; only the irreducibly-CT bits (path fetch, callback walk) are
unsafe, and they're per-glyph leaves with no bounds logic of their own.

### The color-glyph gap

`CTFontCreatePathForGlyph` returns `NULL` for a color (emoji) glyph — there is no
outline. Outlining a lone emoji therefore yields a real *advance* but an *empty path*
(`test_shape` asserts `pt_len == 0`). Outlines are a dead end for color glyphs; they
need to be **drawn**, not traced — which is a different boundary shape (a pixel buffer
the boundary fills), taken up next.
