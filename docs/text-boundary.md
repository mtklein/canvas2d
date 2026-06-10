# The text boundary under `-fbounds-safety`: shaping a glyph run

Text is where this project's real boundary to un-annotated unsafe code lives. Core
Text is a pure-C framework with no bounds annotations, so the shim that binds it is
built without `-fbounds-safety` (`configure.py BOUNDARY_C`). The question this probe
asks: as the text API grows from "draw a string" to real shaping — RTL, ligatures,
emoji, font fallback — what crosses that boundary, and how does the flag shape it?

> The two boundary shims this doc develops separately — the per-codepoint
> `cnvs_font_ct.c` and the shaping `cnvs_shape_ct.c` — have since been unified into a
> single text module, `src/cnvs_text_ct.c` (with checked core `src/cnvs_text.c` and
> header `src/cnvs_text.h`). The split below is the design narrative; the file names
> in links point at the merged module.

## Today's boundary is narrow and value-typed

`cnvs_text_ct.c` processes text *one codepoint → one glyph* (`CTFontGetGlyphsForCharacters(..., 1)`),
does all the Core Text array work internally with fixed count-1 buffers, and hands the
checked core only *finished* `cnvs_path` outlines and `float` metrics. **No glyph
array ever crosses the boundary.** That keeps the checked side forge-free, but it also
means every bit of text logic lives in the *unsafe* TU — fine while that logic is
trivial, a growing liability once it's shaping.

## Real shaping produces runtime-count runs

[../src/cnvs_text_ct.c](../src/cnvs_text_ct.c) shapes a UTF-8 string with Core Text
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

[../src/cnvs_text.h](../src/cnvs_text.h) declares the run as a struct of
`__counted_by(count)` pointers plus a sibling `int count`. That struct is **the same
layout in both TUs** — `__counted_by` ties the bound to the existing `count` field and
adds no hidden member — so the unsafe shim fills it (the attribute is a no-op there)
and the checked core reads it (the attribute is enforced there), with no marshalling
in between. Every `glyph[i]`, `xadv[i]`, `cluster[i]` in [../src/cnvs_text.c](../src/cnvs_text.c)
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

## Concurrency at the boundary

The shim holds no state of its own — no cached `CTFont`, no lazily built
`CFString`, no file-scope mutable anything; every call creates, uses, and
releases its CF objects locally, and anything that outlives a call (a retained
run font, a `cnvs_shaped`) is owned by one canvas's cache. That matches Core
Text's documented contract: individual functions are thread-safe and font
objects (`CTFont`, `CTFontDescriptor`) may be used from multiple threads
simultaneously, but *layout* objects (`CTLine`, `CTRun`, `CTTypesetter`) must
stay within a single thread — which they do here, never escaping the call that
made them. So distinct canvases shaping concurrently on distinct threads cross
this boundary safely (gated by `tests/test_threads.c` under the `tsan`
variant); sharing one canvas across threads is the caller's serialization
problem, same as the rest of the API.

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

## Color emoji: the bitmap boundary

Since a color glyph has no outline, it is rendered into a pixel buffer.
`cnvs_glyph_draw` is the second boundary shape: the checked core owns a
`uint8_t *__counted_by(w * h * 4)` RGBA8 buffer and hands `(px, w, h)` to the boundary,
which wraps it in a `CGBitmapContext` (`bytesPerRow = w*4`, `h` rows) and draws the
glyph with `CTFontDrawGlyphs`.

- **A pixel buffer crosses checked → boundary** — the 2D mirror of the glyph-run
  hand-off (and of the font-name output buffer). `(ptr, dims)` go in; the boundary
  writes *within* the dimensions it was told (`w*4 × h = w*h*4` bytes). Same `(ptr,
  count)` trust model, opposite direction, **no forge**. Bounded data now crosses this
  boundary in every shape we've needed: glyph arrays in, name/pixel buffers out.
- **Color works.** On current macOS `CTFontDrawGlyphs` renders the emoji *in colour*
  (the test asserts non-grayscale pixels and real alpha), so the outline gap is fully
  covered. Text rendering therefore has exactly two boundary shapes — **vector outline**
  for normal glyphs, **raster draw-into-buffer** for color glyphs — and both pass
  bounded data across cleanly.
- **ASan-clean.** Drawing into the checked heap buffer through CoreGraphics does not
  trip AddressSanitizer; CG writes inside the dimensions, so the boundary's contract
  ("dims match the buffer") holds at runtime too.

So the whole text path — shape, fall back, lay out, outline or draw — keeps its
index-heavy logic on the checked side and its irreducibly-CT work (shaping, glyph
paths, glyph rasterization) on the unsafe side, with every crossing a plain
`(pointer, count)` and not one forge among them.

**Wired into the public API.** `canvas_fill_text`/`stroke_text` now lay out through
`cnvs_shape`: outline runs accumulate into one device-space path and fill/stroke as
before (so a fallback run for a missing script renders too, not a `.notdef` box),
while a *color* run's glyphs are drawn with `cnvs_glyph_draw` into a checked RGBA8
buffer and composited through the CTM by the very same code as `drawImage` — so an
emoji takes the transform, clip, global alpha, and shadow like any other image.
(`CGBitmapContext` hands back premultiplied, top-row-first RGBA, so the core only
unpremultiplies before that hand-off — no row flip; an early version added one and
rendered every emoji upside down.)  Measurement is unified onto the same path:
`measureText`, the advance used for `textAlign`, and the `maxWidth` condense all read
the shaped line (`cnvs_shaped_metrics` measures each glyph in its own fallback font),
so a string measures the way it draws.  Only the font-wide metrics
(ascent/descent/em/baselines) still come from the primary font handle — cheap, and
text-independent.

## Bidi caret and selection: the intricate part adds no boundary at all

Caret placement and selection are the most index-heavy text feature, and they are the
cleanest result: **they add zero unsafe surface.** `cnvs_shaped_x_at_index` (logical
index → visual x) and `cnvs_shaped_selection` (logical range → visual spans) are pure
cross-indexing over the cluster map the boundary already handed across — no CT call,
no new crossing.

The bidi behaviour falls out of the index logic. A *full* selection stays one span
over the whole line; a logical range that straddles the LTR↔RTL boundary maps to
*non-contiguous* visual positions and splits — selecting logical `[1,5)` of
`"Hi שלום!"` yields two spans (the `"i "` piece on the left and a Hebrew sub-piece
further right), because the selected glyphs are interrupted in visual order by
unselected ones. The "selected?" toggle walking visual order produces the split for
free.

Every piece of that is checked: each `cluster[i]` read against the run count, each
`out[n++]` against the `__counted_by(max)` output buffer (the span count is capped, so
a pathological input can't overflow the caller's array), the cluster value
range-validated before it's trusted as a source index. **No forge, no CT, all
checked.** The thesis the whole exploration kept hitting lands hardest here: put the
indexed data in a `(pointer, count)` form once at the boundary, and even the most
intricate downstream logic — bidi — is ordinary bounds-checked C. The unsafe surface
of real text never grew past shape + outline + draw.

## The outline boundary, re-cut: canonical curves in font units

The positioned-outline design above had the boundary emit a *finished* outline:
`cnvs_glyph_outline` took the pen origin, the device transform, and a flatness
tolerance, and the `CGPathApply` walk wrote already-transformed, already-flattened
line segments straight into the `cnvs_path`. Correct, but the boundary's output was
device- and call-specific — the bytes that crossed were useless for any other pen,
size, or transform.

The boundary now speaks canonical data. `cnvs_glyph_curves` hands across one glyph's
outline as verb + point arrays (move/line/quad/cubic/close) in **font units** — the
font's design grid, `units_per_em` units to the em, y up, baseline-relative — plus
that `units_per_em` as the only metadata. Core Text's `CGPathApply` reports points at
the font's *point size*, so the shim multiplies by `units_per_em / size` on the way
out, and nothing call-specific survives the crossing: the same bytes describe the
glyph at every size, pen, and transform.

Everything that used to run in the shim moved to the checked side
(`cnvs_glyph_outline`, now in [../src/cnvs_text.c](../src/cnvs_text.c)): scale by
`size_px / units_per_em` (the shaped line records the size it was shaped at), flip y
into canvas's y-down user space, place at the pen, map through the CTM, and flatten
quads/cubics with the same adaptive flattener every other path in the system takes,
at the same device-space tolerance as before. The flattening maths didn't change —
only which TU runs it, and now every index in it is bounds-checked.

The hand-off reuses the `cnvs_glyph_draw` pattern, twice over: the checked caller
owns the verb and point buffers and passes `(ptr, cap)` pairs; the boundary fills
within the caps and reports the *true* counts, which may exceed them — the caller
grows and fetches again (stack buffers cover the typical glyph, so the retry is
rare). No forge, as ever. And the curve stream itself is untrusted boundary data, so
the core walks it defensively: a byte that isn't a verb, or a verb whose points would
run past the count, stops the walk instead of being trusted as an index — the same
posture as the cluster-map range check.

Why re-cut a boundary that rendered correctly? Because canonical curves are the
prerequisite for what comes next: a lookup from call parameters to derived glyph
data, checked before Core Text is ever called, and a self-contained serialization of
glyph geometry into the canvas-program format. Both need the boundary's output to be
*reusable* — keyed by font + glyph id, valid at any transform — which is exactly what
device-space flattened points can never be.

## The lookup in front: the boundary becomes a cache-miss path

That lookup now exists ([../src/cnvs_text.h](../src/cnvs_text.h)'s `cnvs_text_cache`,
one per canvas): a params → derived-data memo consulted **before** every Core Text
call and populated live from what the boundary hands back. Two maps, mirroring the
two things that cross:

- **Shaped lines**, keyed by `(size_px bits, paragraph direction, text bytes)`.
  `fillText`, `strokeText`, and both measure paths used to shape and free a
  `cnvs_shaped` per call; they now *borrow* the cached line (the cache owns it,
  retained `CTFontRef`s and all), so the universal measure-then-draw pattern shapes
  once. The direction bit is in the key because the same bytes shape differently
  under ltr and rtl paragraph bases (run order, neutral resolution) — the canvas
  `direction` attribute supplies it. 64 slots, LRU-evicted — a frame's
  repeated labels stay hot, and a 64-entry scan is cheaper than anything clever.
- **Glyph curves**, keyed by `(font name, glyph id)`. This is what the canonical
  re-cut bought: the cached verbs/points are font-unit bytes, valid at every size,
  pen, and transform, so one entry serves every draw — including the rare overflow
  glyph, whose grow-and-refetch now happens once per glyph with its exact-size
  buffers donated to the cache. The key is the *interned font name* (one
  `cnvs_run_font_name` fetch per run), not the `CTFontRef` pointer: names are stable
  across processes, which the serialized half of this lookup needs. Blanks cache
  too — "no outline" is itself a boundary answer, so a space costs one fetch ever.

The cache is **transparent**: a hit replays the boundary's exact bytes through the
same checked transform/flatten a miss runs, so warm and cold renders are
byte-identical (`test_textcache` pins this, and the gallery PNGs did not move), and
any cache-side allocation failure degrades that lookup to a plain boundary call —
never an op failure (the OOM sweep's text scene covers the cache's allocation
sites). `reset()` clears it back to cold along with the rest of the canvas state.

The bounds-safety story stays the good one: the memo is ordinary checked C —
`__counted_by` key bytes and curve arrays, every probe and comparison in the sized
model — and it *narrows* the runtime trust surface: a warm canvas re-trusts nothing,
because the unsafe TU isn't called at all. On a text-heavy tight loop (2000×
`fill_text` + `measure_text` of one string) the caches are worth 1.43× end to end;
the whole-gallery number doesn't move because each scene renders once on a fresh
canvas — cold caches, by construction.

This is the **live half** of the params → derived-data lookup. The serialized half
reads straight from these cache entries: stable name-keyed, size-independent bytes
are already the wire format.

## The serialized half: one self-contained file per program

A recorded canvas program ([canvas.h](../include/canvas.h)'s
`canvas_record_to`/`canvas_replay_from`) now carries the lookup's contents inline —
no sidecar, one file. Before each text op that first needs them, the recorder
([../src/cnvs_record.c](../src/cnvs_record.c)) emits block lines straight from the
cache entries the op is about to use:

- **`font <id> <ascent> <descent> <name…>`** — an interned font name (the rest of
  the line; names contain spaces) with its vertical metrics normalized to size 1.0.
  Ascent/descent are linear in size, so one block serves every font size — the
  consumer multiplies by `size_px`. The canvas's baseline placement and font-wide
  TextMetrics read the same per-name record, deriving even the *live* values
  through the stored floats, so recording and replay place baselines identically.
- **`glyph <font-id> <gid> <units-per-em> <ink x0 y0 x1 y1> <m/l/q/c/z curves…>`**
  — one glyph's canonical data, exactly the cache entry: verb tokens with their
  control points *and* the tight ink box, all in font units (y up,
  baseline-relative), so one block serves every size, pen, and transform — and
  `measureText` replays deterministically too, with no format revision needed when
  CI wants metrics scenes. A blank glyph is `units-per-em` 0 with no curves
  ("known to have no outline" serializes like it caches).
- **`shape <size_px> <rtl> <utf16-len> <nruns> <byte-len> <text…>`** followed by
  `nruns` **`run <font-id|-1> <rtl> <color> <nglyphs> (gid adv cluster)*`** lines —
  everything `cnvs_shaped` carries, keyed exactly as the live cache keys it
  (`size_px` bits + paragraph direction + raw text bytes, length-prefixed to end
  of line). The shape-level `rtl` is the paragraph base direction the line was
  shaped under: it must ride the block because it is half the cache key — the
  same bytes shape differently under ltr and rtl, and replay keying its insert
  without it would alias the two. The line-based format already cannot represent
  text containing newlines, so one line per shape block is faithful.

Blocks are deduplicated within the file by per-slot `emitted` marks (a new
recording clears them), so a frame's repeated label costs one block set and then
one op line per draw.

Replay ([../src/cnvs_replay.c](../src/cnvs_replay.c)) parses the blocks **strictly**
— the slice-A "untrusted verb stream" posture, now at the parser: ids
range-checked and declared-before-use, verb tokens validated with their point
counts, cluster indices checked against the shape's UTF-16 length, counts bounded
by the line, non-finite numbers rejected where the recorder writes finite ones,
and the `shape`→`run` cross-line state tolerating no interleaving. Any violation
stops replay false; the canvas stays valid. Parsed blocks pre-populate the text
cache, and a rebuilt run carries its *interned name id* with `font == NULL` — the
CTFontRef-free path: outline drawing reads curves from the cache by name id,
baseline placement reads the vmetrics record, metrics read the ink boxes, and the
run's `is_color`/`rtl` ride in the struct. No boundary call is needed to draw a
recorded non-emoji line; `test_record_text` pins the proof through the stats
surface (replaying a recorded text program performs **zero** shape/glyph boundary
calls) plus byte-identical pixels and bit-identical measureText.

Float round-trip is a correctness requirement, not a nicety — the shape key is
`size_px`'s *bits*, and byte-identical rendering needs every advance and control
point back exactly. Numbers are written with `%.9g` (nine significant digits
uniquely identify any float32, and the file stays human-readable); the parser's
number reader scales by *exact* powers of ten (10^k is exact in a double for k ≤
22, stepped), so its at-most-three roundings sit orders of magnitude inside the
half-ulp margin and every emitted float reparses to the identical float32 —
property-tested across denormals, −0, and both extremes, while hostile exponents
keep the old saturate-to-inf/0 clamping.

Deferred at this point (and closed in the next section): **color (emoji) glyphs
are bitmaps**, not curves, and bitmap serialization needed its own design. Until
it landed, a fontless replay drew color runs as blank advances. Degradations
stay best-effort throughout, mirroring the live cache: a full glyph table or
intern table during recording simply leaves those blocks out (replay falls back
to live shaping where fonts exist), and a cache-side allocation failure during
replay drops the entry rather than failing the parse — only *malformed input* or
an allocation failure while rebuilding a block returns false.

## Emoji canonicalization: the bitmap boundary crosses once

The original color-glyph path (`cnvs_glyph_draw` above) crossed the bitmap
boundary **per draw**: every `fillText` with an emoji asked Core Text to
rasterize the glyph at device size into a fresh checked buffer. Correct, but
the boundary's output was call-specific — the same problem the outline re-cut
solved for curves, in raster form. And it made emoji unserializable: there was
no stable artifact to record.

A color glyph now has a **canonical capture**: one premultiplied RGBA8 render
per (font name, glyph id), rasterized by the boundary at a fixed, documented
size — `CNVS_CAPTURE_EM` = 160 px to the em, AppleColorEmoji's largest bitmap
strike, so the capture loses nothing the font has to give. The capture is a
square 160x160 buffer placed by the same machinery the per-draw path used:
`cnvs_glyph_bounds` (at the capture size, via a `cnvs_font_resized` handle)
gives the ink box in capture px, and `cnvs_glyph_draw` renders with the ink
box's bottom-left pinned to the buffer's corner. The ink box rides along in
the glyph slot, so placement and `measureText` both derive from cached data —
scale by `size_px / CNVS_CAPTURE_EM`, the exact analogue of the outline path's
`size_px / units_per_em`. The boundary crossing is the same `(ptr, w, h)`
hand-off as before; it just happens **once per glyph ever** instead of once
per draw.

Everything derived from the capture is checked C:

- **The mip pyramid.** Repeated 2x2 box halving of the premultiplied capture
  down to 1x1 (`cnvs_mip_halve`), ceil-halving odd dimensions with the edge
  row/column replicated. One shared rounding (`(sum + 2) >> 2`) keeps the
  premul invariant `r,g,b <= a` exact at every level. Levels are separate
  `__counted_by(w*h*4)` allocations rather than offsets into one arena: each
  level's bound is exact (an overrun traps at *that level's* edge), the
  ownership story is trivial, and a failed level keeps the prefix — selection
  degrades to the coarsest level that built, worst case the capture itself.
  The pyramid is built lazily on first draw and **never serialized**: the
  capture alone is canonical, so the derived form can change shape at zero
  format cost.
- **Drawing.** The glyph quad's device footprint (its longer CTM-mapped edge)
  selects the *smallest level still >= the footprint*, and the existing
  transform-aware bilinear image path samples within it — bilinear then never
  downscales by more than 2x, which box-halved sources handle without visible
  softness or aliasing (the gallery's emoji scenes moved imperceptibly;
  trilinear stays in reserve if continuous-zoom popping ever matters). The
  sampler grew a premultiplied-source flag rather than a second copy of the
  quad walk — interpolating premultiplied bytes is fringe-free, and emoji keep
  taking the transform, clip, global alpha, and shadow like any image. No
  `CTFontRef` is needed to draw: a capture from the cache (or a replayed
  block) renders fontlessly, and the per-draw boundary render survives only as
  the degraded path when the cache cannot serve and a live handle exists.
- **Serialization.** The capture is the recorded form, deflated: a `bitmap`
  block (`bitmap <font-id> <gid> <w> <h> <ink box> <zlen> <nlines>`) followed
  by exactly `nlines` base64 `bits` lines carrying a `zlen`-byte zlib stream
  (the in-house `cnvs_zlib`, the same compressor under the PNG encoder) that
  must inflate back to exactly `w*h*4` bytes. The chunking is forced by
  arithmetic — 160x160x4 = 102,400 raw bytes would be ~137 KB encoded, against
  the parser's 64 KiB line cap — so the recorder emits 12,288-byte
  (16,384-char) lines, divisible by 3 so only the final line pads; the deflate
  is what pays for file size. Captures compress to between a third and a half
  (anti-aliased gradient art is noisy input for the greedy fixed-Huffman
  matcher): a one-emoji program that measured ~137 KB raw records at ~57 KB
  (🍕), with the spectrum spanning roughly 40–70 KB. The strict parser extends
  through the new layer: declared-before-use ids, capped dimensions (bounding
  the decoded allocation at 1 MiB), `zlen` capped by `cnvs_zlib_bound(w*h*4)`
  and `nlines` by `ceil(zlen/3)` — both checked *before* either buffer is
  allocated — exact chunk counts, padding legal only in the final group of the
  final line, the decoded total required to equal `zlen` exactly, and the
  already-strict inflate (header, Huffman structure, adler, trailing garbage,
  overflow) required to fill `w*h*4` exactly. Premul sanity of the *pixels* is
  deliberately not validated — bytes are bytes, and a hostile capture can only
  mis-render its own quad.

The `-fbounds-safety` story here is the pyramid: byte-level image math over
related buffer sizes, all checked, with the one real friction being that a
slot's `__counted_by(nmips) mip` array cannot grow its count in place (a
loaded pointer's bounds are its *current* count), so the build assembles the
level array in a local and installs pointer + count once, adjacent — the same
grouped-assignment idiom the rest of the cache uses.

With this, the bitmap boundary matches the outline boundary's shape exactly:
**canonical, keyed, size-independent bytes cross once**, the cache serves
every draw, and the serialized form is the cache entry, losslessly deflated. The last
boundary-per-draw path in text is gone — a warm canvas renders emoji, and a
replayed program renders *and measures* them, without Core Text existing at
all.

## The capstone: proving it on a machine without the fonts

Every claim above — canonical curves cross the boundary, a cache sits in front
of it, the program format embeds the derived data — `test_record_text` already
pins *locally*, where the fonts exist: it records a text scene, replays it, and
asserts byte-identical pixels with **zero** shape/glyph boundary misses. But the
real claim is cross-machine: a program recorded on a machine with Libian TC and
AppleColorEmoji must replay byte-for-byte on a machine that has neither. The CI
runner is exactly that machine — Libian TC is download-on-demand, so it isn't
there — which is why the byte-for-byte gate (`gate.yml`) only covers the ten
text-free scenes. The fontless proof had to come from inside the suite, which
the runner runs via bare `ninja`.

It now does. Every gallery scene — all 33, the eight text scenes (`text`,
`textgrid`, `textmetrics`, `textmaxwidth`, `emoji`, `emojiscale`, `shaping`,
`rtl`) among them — records a self-contained `gallery/<scene>.canvas` program
alongside its committed PNG
([../examples/gallery.c](../examples/gallery.c)'s `record_scene`), and
[../tests/test_replay_gallery.c](../tests/test_replay_gallery.c) replays each
one onto a fresh canvas and proves **both directions**:

- **The byte compare.** The replayed `read_rgba` must equal the committed PNG
  (decoded by the in-house loader) byte for byte. A divergence means the
  program didn't reconstruct the render — a stale `.canvas`, a missing op, or,
  on the fontless runner, a glyph the embedded blocks failed to carry and the
  fallback drew wrong or blank.
- **The zero-miss assertion.** The replay must take **zero** shape/glyph
  boundary cache-misses (emoji captures bump `glyph_misses` on a boundary fetch,
  so the one counter covers outlines and color glyphs alike). A single miss is a
  Core Text fallback — precisely what a fontless machine *cannot* do — so the
  assertion catches a font-fallback even where the byte compare might
  coincidentally match (and on the fontless runner the fallback would also move
  pixels, so the two checks reinforce each other). Stripping a program of its
  blocks and leaving only the op lines trips this with `shape_miss`/`glyph_miss`
  in the dozens; emptying a program diverges on the byte compare. A scene with
  no text passes the assertion trivially, so the eight text scenes additionally
  assert the cache saw real traffic (`shape_hits`/`glyph_hits` > 0) — the
  zero-miss claim can't go vacuous where it matters.

Closing the format for the (then) seven text scenes took two ops the recorder didn't
yet cover — `textmetrics`'s `stroke_rect` and `textmaxwidth`'s `fill_text_max`
(the latter a slice-only variant, `canvas_fill_text_max_n`, so the parser stays
in the counted world; `max_width` rides the op line, since the shaped line keys
on size+text alone) — plus the four shadow setters the `emoji` scene's drop
shadow needs, all serialized as plain floats and parsed strictly. With them,
all seven replayed byte-identically with zero boundary calls — emoji captures
included, under the gallery's transforms, shadows, and global alpha.

The format has since closed over the **whole pixel-affecting API** (canvas.h's
`canvas_record_to` doc is the authoritative statement): drawImage /
putImageData / pattern sources ride numbered `image` blocks through the very
machinery the emoji captures built (deflate + base64 `bits` lines, deduplicated
by content within a file), Path2D draws ride numbered `path` blocks (one verb
line per builder command, serialized at first draw — the canvas-free builders
have nothing to hook until then), and the scalar stragglers — conic gradients,
`round_rect_radii`, image smoothing, the filter list, `reset`/`resize` —
record as plain op lines. So the determinism gate covers all 33 gallery
scenes, not just the text ones, byte-for-byte.

That is the arc's end state: glyph outlines and emoji captures cross the Core
Text boundary as canonical, keyed, size-independent bytes; a cache serves every
warm draw without re-crossing; the program format embeds those bytes — and
every image, path, and op a scene uses — so a recorded program is
self-contained; and every recorded gallery scene reproduces its committed
render **byte-for-byte on a machine that has none of the fonts** — gated, in
lockstep with the renderer, by a test that runs everywhere `ninja` does.
