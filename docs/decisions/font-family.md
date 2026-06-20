# Memo: the font-family project — un-pinning the typeface

**Scope read:** `src/canvas.c` (`k_font_family`, `ensure_font`, `shape_text`, the text
state in `cur`), `src/cnvs_text.{h,c}` (the shaping cache keyed by `(size, direction,
letterSpacing, wordSpacing, text)`; glyph data keyed by `(font name, glyph id)` with
`CNVS_FONT_INTERN_N = 16` name slots; `cnvs_run_font_name`), `src/cnvs_text_ct.c` (the
Core Text boundary: `CTFontCreateWithName`, `CTFontCreateCopyWithAttributes`,
`CTFontGetSymbolicTraits`), `src/cnvs_record.c` / `src/cnvs_replay.c`, `docs/text-boundary.md`,
`docs/roadmap.md` (the "Partial — Text styling" gaps).

## Context

The typeface is pinned (`k_font_family = "Libian TC"`).  Most multi-font machinery
already exists: shaping takes a font name, the glyph cache keys by resolved font name
(fallback fonts already occupy distinct name slots), and record/replay embeds glyph/shape
blocks so the fontless CI runner replays byte-for-byte with no Core Text call.  The
remaining text-styling gaps (`docs/roadmap.md`) — family/weight/style and the shaping
toggles (`fontKerning`, `lang`, `textRendering`, `fontVariantCaps`, `fontStretch`) — are all
font-feature-dependent: they have no observable effect on a single Chinese clerical face,
so they belong together here, not as standalone work on the pinned font.  letterSpacing/
wordSpacing and the selection/caret queries were the font-independent (geometric) items and
are already done.

## Decisions

1. **Libian TC stays the default family.**  Retained deliberately; keeping it also avoids
   re-baselining every existing text scene.  New families are opt-in via the setter.  The
   initial font is spec-mandated initial *drawing state* (`10px`, a default family), which
   the no-favored-default rule exempts.
2. **The Libian model for every font.**  As far as `.canvas` is concerned a font is its
   embedded glyph data — outline curves and (colour) bitmaps — keyed by resolved font name.
   New families record on the authoring machine (macOS system fonts: Helvetica, Georgia,
   Menlo, …) and replay from the embedded blocks with no Core Text, so the fontless CI gate
   holds.  As with the emoji captures, recorded outlines are macOS-version-sensitive at
   record time; replay is not.
3. **Synthesis is minimized; faking styles is accepted as inherently hacky.**  Resolved
   after F2: Core Text synthesizes bold/italic only at *raster* time
   (`CTFontDrawGlyphs`), not in the *outline* (`CTFontCreatePathForGlyph`), so the
   outline-recording (Libian) model cannot capture a synth-bold.  Outcome:
   - **Synth-italic** is done by baking the slant into the font matrix, which genuinely
     skews the recorded outline — kept.
   - **Synth-bold** is NOT done: a family with no real bold face falls back to its nearest
     real weight.  Faux-bold by *outline dilation* (stroking/offsetting the recorded
     curves) is a possible future approach, deliberately deferred — fake styling is hacky
     however it is done, and real bold faces cover the overwhelming majority.

## Plan — four phases, each a reviewable/mergeable chunk

Every attribute that affects shaping or glyph identity must (a) join the shaping cache key,
(b) be a recorded sanitized state op (like `set_font_size`), and (c) ride the block
serialization so fontless replay reproduces it.  The byte gate is the backstop.

**F1 — font family as state.**  `canvas_set_font_family(cv, name)`; family copied into
`cur` (rides save/restore; `reset()` → Libian TC).  `ensure_font`/`shape_text` use
`cur.font_family`; `ensure_font` rebuilds `cv->font` on family change as well as size.  The
family joins the shaping cache key and the `shaping` block line (the existing name-keyed
glyph cache already separates the glyphs).  `set_font_family` recorded op.  Gallery scene
with 2–3 system fonts; fallback test (bogus family → Core Text cascade → recorded as the
resolved name).  Existing scenes' `.canvas` gain a family token on their shaping lines; no
`.png` change (default is still Libian TC).

**F2 — weight & style.**  `canvas_set_font_weight` / `canvas_set_font_style`.  Font identity
becomes `(family, weight, style)`, resolved through a `CTFontDescriptor` with symbolic
traits (Core Text synthesizes when the face lacks them, per decision 3).  Glyph-cache key
and font-block serialization gain weight/style so bold-A ≠ regular-A; setters + recorded
ops; gallery bold/italic; tests on differing advances/outlines.

**F3 — the shaping toggles.**  State + setter + recorded op + shaping cache key +
serialization + the Core Text attribute/feature for each.  Split into **F3a**
(`fontKerning`, `textRendering`, `lang` — the attribute-on-the-run toggles, done)
and a later **F3b** (`fontVariantCaps`, `fontStretch` — the descriptor/feature
toggles).  F3a's three join the shaping cache key alone (they change advances and
glyph selection, not glyph-outline identity, so the glyph/font block is
untouched); the shaping block line gained `<kerning> <rendering> <lang-len>
<lang>` tokens, and `set_font_kerning`/`set_text_rendering`/`set_lang` are recorded
state ops.  textRendering maps pragmatically: optimizeSpeed disables BOTH kerning
and ligatures (Core Text has no single "speed" attribute), the other values leave
the defaults; AUTO/AUTO/"" reproduce today's shaping byte-for-byte.

| toggle | Core Text mechanism |
|---|---|
| `fontKerning` | `kCTKernAttributeName` 0 = off; `none`→off, `auto`/`normal`→default |
| `textRendering` | kerning + ligature toggles (`optimizeSpeed`→off) |
| `lang` | `kCTLanguageAttributeName` (string) on the attributed run |
| `fontVariantCaps` | `smcp`/`c2sc` OpenType features via the descriptor |
| `fontStretch` | `kCTFontWidthTrait` / `wdth` axis (selects a width face) |

`lang` adds a string to the cache key (extra care).  Each is tested against a font that
actually exercises it (a kerned face, a small-caps face, a width family, CJK for `lang`).
Gallery toggle showcase.

**F4 — CSS `font` shorthand.**  Not planned.  In a C API the individual setters
(family/weight/style/size) are complete; a CSS-string shorthand parser is a poor fit and is
left out unless a concrete need appears.

## Risks

- Determinism rides on embedded blocks (proven).  The new sensitivity is record-time font
  availability and CT-version-dependent outlines — the same posture as the emoji captures.
- The synthetic bold/italic output (decision 3) is whatever Core Text produces; we accept
  and embed it.  If it reads poorly we revisit (real faces only, or a synthesis tweak).
