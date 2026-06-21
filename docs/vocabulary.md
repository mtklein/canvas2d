# Vocabulary: the survey and the rulings

Status: **rulings implemented; this doc is the standing vocabulary
reference.**  Each family below carries a "Landed:" line saying what the tree now
does. The naming goals: one word per meaning, one meaning per word; generic names
for generic concepts, specific names for specific ones.  Precedents, in order:
the web spec's names where the project implements it, OpenCL's names for
lane/vector vocabulary, Mike's len rule (size = bytes, nfoo/foo_count = element
counts, len = spatial).

## Prefixes: one namespace, visibility by location

One prefix.  Every C identifier in the project carries `canvas2d_` (macros
`CANVAS2D_`): the rendering context, its companion objects, the standalone
utilities, and the internal implementation alike.  Visibility is the header
*location*, not the name: a header in include/ is public, one in src/ is
internal.  So the same prefix spans both layers, and `-Iinclude -Isrc` lets a
bare `#include "canvas2d_x.h"` resolve wherever the file sits.

Anchors: the main type is `struct canvas2d_context`; the constructor is
`canvas2d()`; companion objects are `struct canvas2d_path2d` and `struct
canvas2d_image`.

Public utilities live in include/, each a standalone `canvas2d_` header: zlib
(`canvas2d_zlib.h`), the PNG encoder (`canvas2d_png.h` -- buffer-based:
`canvas2d_png_encode` takes a raw uint16 RGBA buffer, not a canvas),
colour-space conversion (`canvas2d_color.h`), the matrix/homography math
(`canvas2d_matrix.h`), and the fill/stroke style enums (`canvas2d_paint_style.h`,
re-included by `canvas2d.h`).  Internal implementation headers (the context
state, the rasterizer, the stroker, the recorder/replay, the blit and the
internal Path2D representation) sit in src/.  Two src/ headers carry domain
names that disambiguate them from a same-stem public header: `canvas2d_blit.h`
(the RGBA/f16 row copiers, distinct from the public image objects in
`canvas2d_image.h`) and `canvas2d_path2d_internal.h` (the recorded-command
representation, distinct from the public builder in `canvas2d_path2d.h`).

The single exception is the SIMD lane vocabulary -- the unprefixed lane vector
types (`f16x4`, `f32x8`, `u32x16`, `i16x8`, `u8x4`, ...) and the lane helpers
that live with them in `src/canvas2d_math.h`.  These keep their bare names
pending a separate normalization pass; they are not part of this namespace.

## Collisions (same word, different meanings)

### C1. `cap` — four meanings
- capacity (canvas2d_verts.cap, pt_cap/sp_cap, stack_cap, cov_cap, tile_cap,
  bitwr.cap, vcap/pcap, get_line_dash(out, cap)) — Go/stb-standard usage
- line cap (canvas2d_line_cap, emit_cap) — W3C term, immovable
- capture (canvas2d_glyph_slot.cap_w/cap_h/cap_len) — `cap_len` reads as
  "capacity length", means "capture byte size"
- upper limit (read_uint(..., cap), "the 64 KiB line cap", REPLAY_*_MAX
  prose)
All but line-cap coexist inside canvas2d_text.h/canvas2d_replay.c simultaneously.

When working with line-cap, always keep the words line-cap in it.  If needed to
abbreviate, lc is better than cap.  Let's try to have "cap" refer a buffer's
capacity, and try to find better names for other use cases.

Landed: cap is capacity only.  The capture fields spell capture out
(capture_w/capture_h/capture_size); upper limits live under *_MAX; line-cap
keeps its full name.

### C2. `len` — one name, four units
- bytes: canvas2d_read_rgba(out, len), owned_image.len, shape_slot.len,
  replay text_len, zlen/blen/glen/slen...
- pixels: compositor_read(out, len) — the direct seam partner of
  read_rgba's byte len; same name changes unit across one call
- elements: canvas2d_verts.len, pt_len/sp_len, path2d.len, stack_len
- UTF-16 units: canvas2d_shaped.text_len — while canvas2d_shape()'s text_len
  parameter four lines away is UTF-8 BYTES

Try to keep geometric length spelled out as length, and avoid len otherwise...
byte counts should be "foo_size", counts of other things "nfoo" or "foo_count"
(you pick which you like and use it everywhere)

Landed: element counts are nfoo (npts, nsubs, nverts, ncmds, nsaved);
canvas2d_shaped.utf16s names its UTF-16 unit; canvas2d_blend_read counts pixels by
name; byte counts keep the __counted_by-int len dialect (capture_size where
the brief named it).


### C3. `quant8` — two unrelated operations
- blur.c quant8: exact rounded integer divide via reciprocal+snap
- canvas.c unpremul_quant8: float -> unorm8 conversion
- (cover_to_u8 IS the unorm8 quantize, named _to_u8; canvas2d_f2u8 is a
  saturating convert of an already-0..255 value, NOT unorm)
Field standard: unorm8 (Vulkan/D3D/Metal UNORM) for [0,1]->u8.

Yep, unorm where appropriate, and maybe for the other case some sort of "exact"
wording rather than quant.

Landed: blur's exact rounded divide is div_round8; the readback conversion is
unpremul_to_unorm8.

### C4. `k255` — FIXED (0f6a0af): compositor's 1/255 is now inv255;
canvas.c's 255.0 keeps k255.

Yeah, k255 should be 255, inv255 for 1/255.

Landed: holds -- k255 is 255.0, inv255 is 1/255.

### C5. `mask` — clip mask (u8 plane) / SIMD compare mask (i16x8, was
mask8 — type already renamed) / shadow silhouette mask.  Clip and shadow
masks are arguably one concept (a u8 coverage plane) under two names.

I don't mind clip and shadow both talking about masks, as long as it's just
within the context where clip or shadow is implicit.  Outside that
disambiguate: clip mask, shadow mask

Landed: holds as ruled; cross-context mentions say clip mask / shadow mask.

### C6. `block` — 8-pixel planar block / .canvas format block / DEFLATE
block (RFC-fixed) / staged vertex block / 2x2 mip block.  Planar-vs-format
collide hard in canvas2d_record/replay comments.

Hmm sometimes I've used `slab` for the idea of an 8-pixel block.  Does that
conflict with anything else?

Landed: slab is the 8-pixel planar unit (defined in canvas2d_planar.h: a canvas2d_px8 IS
one slab); format, DEFLATE, staged-vertex, and mip blocks keep "block".

### C7. `half` — the f16 type family vs win/2 rounding bias (blur quant8)
vs 0.5f bias (unpremul_quant8 declares `_Float16 const half` amid f16x8
types) vs half-width (stroke hw).

`half` on its own should be fp16, very idiomatic.  I don't see any conflict
as long as half-width keeps width in there.

Landed: half alone means _Float16; the rounding-bias locals are bias and the
stroke abbreviation hw is spelled half_width.

### C8. `run` — glyph run / running winding sum (cover_to_u8(rule, run)) /
dash on-runs.
Again, these are fine in context of an area of code where it's obvious, and
then disambiguate with glyph run, running sum, etc should it ever come up.

Landed: holds as ruled, in-context uses stay.

### C9. `shape` — text shaping (canvas2d_shape, shape blocks) vs geometric
shape (prose); `shade8` sits one letter away in the same file.
Yeah, this one I think we should probably use `shaping` for text shaping and
`shape` to mean the geometric concept.

Landed: shaping it is -- canvas2d_shape_text, struct canvas2d_shaping_slot,
canvas2d_text_cache.shaping[], CANVAS2D_SHAPING_CACHE_N, shaping_hits/misses,
canvas2d_text_cache_shaping(_slot)/put_shaping, tests/test_shaping.c, and the
FORMAT token `shaping` (re-recorded everywhere; the strict parser rejects the
old spelling).  struct canvas2d_shaped stays: a shaped line is shaping's result.

### C10. `filter` — CSS filter vs PNG row filter.  Both spec-fixed;
unavoidable at the spec edge.
No big deal I think.  Not super ambiguous in context.

Landed: no action, as ruled.

### C11. `saturate`/`sat` — clamping conversion (canvas2d_f2i docs) / CSS
saturate() / HSL saturation (sat8, set_sat8 — reads as "saturating 8-bit",
is not).
Let's leave sat/saturate to only where it corresponds to something external
like CSS.  Maybe we can use clamped as the term for something that's guaranteed
in a range.  clamp() and clamp01() are my favorites for producing those things
for sure, so clamped seems natural.

Landed: the HSL pair is saturation8/set_saturation8; sat/saturate appear only at
external seams (CSS saturate(), saturating converts).

### C12. `op` — composite op (public) / "the op" (a draw) / path verb
(p2d_op) / fuzz opcode + recorder command (canvas2d_rec_op).  p2d_op vs
canvas2d_glyph_verb: same concept, two axis nouns.
I think op is just too short and ambiguous to use outside very local settings.
As you've written, compositing operation, a draw, a path verb.. these things
kind of are better spelled out in broad contexts, all fine short in local
contexts.  Definitely pick verb over op if we're using both for paths.

Landed: paths speak verb on both axes -- enum p2d_verb with p2d_cmd.verb beside
enum canvas2d_glyph_verb; the public enum canvas2d_composite_op keeps the web's term.

### C13. `stamp` — LRU last-use tick (shape_slot.stamp, fed by cache.tick)
vs stamping coverage into a mask (verb).
Last-use or timestamp I guess?

Landed: the LRU pair is (tick, last_use): canvas2d_shaping_slot.last_use, fed by
cache.tick.  stamp survives only as the prose verb.

### C14. `bitmap` — format token vs the canvas backing store; and the
format token (`bitmap`) names what the code calls a `capture`.

bitmap should pretty much always be a dense 2D array of pixels.  anything else
we should be scrupulous to find another word.

Landed: audited -- every bitmap (backing store, capture, strike, glyph bitmap
space, the format token) is a dense pixel rectangle.  No renames needed.

### C15. `px` — buffer pointer / font size px / blur stdDev px / loop
index / canvas2d_px8 (8 pixels) vs tests' px4 (4 channels of ONE pixel —
numeric suffix counts different things).

px should always at least refer to a pixel.  It's okay as both a "this pixel"
value/pointer, and as a unit measurement.  If px4 is 4 channels of one pixel,
that's wrong, that should be just px or rgba, channel4 at worst.  px4 sounds
like 4 pixels.

Landed: the tests' one-pixel record is struct rgba (was px4).

### C16. Single letters
- r: radius vs red — ADJACENT in emit_shadow (int r radius; cr = sc.r red)
- a: alpha / matrix entry / first operand / quadratic coefficient (in a
  function also holding colour alpha)
- k: tail count / coverage fraction / scale factor / COSINE
  (canvas2d_matrix_rotate) — count is dominant
- q: quarter-pi / quotient / query point / staged pixel

This is pretty much unavoidable.  r,g,b,a are going to be color channels, x,y
coordinates.  Don't mind using upper case to distinguish, letting the upper
case stand in for the more mathy blackboard-font kind of things like R =
radius.  k should pretty much always be a small integer constant.  i,j
iteration indices.  for coverage fraction, I've found I prefer `cov` or
`coverage` spelled out... k feels too constant.
Also worth mentioning that 'd' and 'dst' are good for things dealing with
the underlying destination buffer, 's' or 'src' for the source colors we're
working on.  s' = blend(s,d), d' = lerp(d,s',cov), that sort of thing.

Landed: cov_lerp8 takes cov (with icov = 1 - cov); canvas2d_matrix_rotate pairs c with
s; emit_shadow's radius is spelled out beside the colour channels; i/j/k keep
their index/count roles; s/d destination-source letters hold.

### C17. Font ids — `fid` (interned cache id) = `name_id` (same thing in a
run) vs `id` (the FILE-LOCAL id, a different space, mapped via map[]).
'id' is like 'op', meaningless unless given more context.  in a very local
case it's fine, but don't let it leak out without a little more description

Landed: the replay parser's file-local font ids read file_id where they meet the
interned-id map (b->map[file_id] = fid).

## Drift (different words, one meaning)

### D1. clamp family — clamp01 DEFINED TWICE with DIFFERENT NaN behavior
(canvas.c NaN->0 via !(v>0); canvas2d_gradient.c plain min/max lets NaN
through).  Bug-adjacent.  Plus clamp_lo, clampi, vclamp01,
canvas2d_px8_clamp_premul (whose comment says "pins").  "pin" = clamp in
prose only; "pin" also means anchor-in-place (patterns, fonts) — two
concepts sharing the prose word.

I like any clamp() function to absolutely strongly guarantee that its output
will be between the requested bounds no matter what.  clamp01(NaN) needs to
return _some_ value in [0,1], etc.  (of course if NaN is one of the boundaries
this is a meaningless promise but why would we ever do that)

Landed: one clamp01, one home -- canvas2d_clamp01 + f32x8_clamp01 in canvas2d_math.h,
both NaN-laundering; the gradient's NaN-passing copy and its vclamp01 are
gone, clamp_lo joined the guarantee, and "pins" no longer means clamp in
prose.  Zero pixels moved (gallery byte-identical, oracles green).

### D2. Composite axis — "op" (public canvas2d_composite_op, CANVAS2D_OP_*,
SOURCE/DESTINATION) vs "blend mode" (compositor_blend_mode,
COMPOSITOR_SRC_*/DST_*) vs "composite" (state field cur.composite).
Mirrored by static_asserts.  Skia says blend mode; the web says composite
operation.

I do like the term "blend", blend mode, blend_fn, just blend... these are any
of those f(px,px) -> px operations we do at the end of the pipeline, whether
porter duff, separable, non-separable, user-defined, whatever.  Of course at
the interface we want to hew to canvas 2D's terms, but I don't mind switching
right away to blend-type names inside.

Landed: the compositor is gone; inside it is blend everywhere (canvas2d_blend,
blend8, blend_term8, tests/test_blend.c), with the web's composite-operation
name at the public enum.

### D3. Lifecycle verbs — create/destroy (canvas, compositor, path2d,
font), open/close (recorder), init/free-ish (path, cover, verts:
reset/free), init/clear-as-destructor (text_cache), release (font, CF
echo), drop (replay parser state).  Hazard: text_cache_clear FREES;
verts_reset does NOT.

Personally I like constructors to just be the type name and them to be paired
with a type_free() (that allows null, just like free() itself):

   struct foo *__single foo(...);
   void foo_free(struct foo *__single);

For things where we're not managing the memory, foo_init() makes sense to me,
and usually foo_reset() or just no clean up at all is necessary.  It's rare
I think that we need all of init, destroy, and reset, and hardly ever a burden
to favor reset (destory and make usable again fresh) over just destroy.

I do like begin/end to mark boundaries rather than open/close.
begin_recording, end_recording.  open and close sound too close to FILE* operations,
and also too close to path2d.

Landed: constructors are the type name with a NULL-ok _free -- canvas2d()/
canvas2d_free(), canvas2d_path2d()/canvas2d_path2d_free(), canvas2d_font()/
canvas2d_font_free() -- and the recorder begins/ends (canvas2d_recorder_begin/
canvas2d_recorder_end).

### D4. reset/clear — empty-keep-storage is `reset` 3x and `clear` 1x
(pattern_clear); free-everything is `clear` (text_cache) and `free`
elsewhere.  canvas2d_reset/clear_rect are web-fixed.

clear for places where we're doing something like bzero, reset for things that
are managing memory and state tracking.  There's some overlap probably, but
that should be the general gist.

Landed: canvas2d_text_cache_reset (frees and reinits) and pattern_reset; clear keeps
the bzero-ish web-fixed uses (clear_rect, the bitmap clears).

### D5. Write verbs — store (reg->mem), write (->file), put (web API /
cache insert / bit emission / curve sink — QUADRUPLE duty), emit (stroke
tessellation / shadow compositing / recorder serialization — emit_shadow
composites pixels, emit_quad appends triangles), set
(cache_set_vmetrics is actually a first-wins insert), deposit (cover —
unique, good), stamp.

load/store to me are the best terms for getting pixels from a framebuffer
into working format registers and the reverse.  read and write do sound
file like.  I think put probably just has better things, like insert, emit.
deposit is fine, could be scatter?  there's a lot here that i think we can
just keep evolving on rather than needing a whole holistic fix all at once.

Landed: evolving as ruled; no holistic rename.

### D6. Read verbs — load (mem->reg AND file->mem: canvas2d_load_png),
read (bulk copy-out AND parse AND canvas2d_png_read = file), get (web +
accessors + getbits), fetch/peek (prose).  File I/O is load publicly,
read internally, for the same operation.

Landed: file I/O reads were named canvas2d_read_png / canvas2d_png_read (matching
canvas2d_write_png), then removed with the PNG decoder -- canvas2d_write_png is now
the only file verb.  load/store stay register traffic.

### D7. premul family — coherent rule emerges: abbreviated
premul/unpremul = the data format; spelled-out premultiply = the
operation.  One breaker: unpremul_quant8 (operation, abbreviated).

I do like your convention of premul/unpremul for the data,
premultiply/unpremultiply for the operations that translate between the two.

Landed: holds; the one breaker is renamed by its endpoints
(unpremul_to_unorm8).

### D8. Curves — internal quad/cubic consistent; public web names
immovable; fuzz_ops.h's OP_BEZIER_TO mixes layers (should be
OP_CUBIC_TO).

Yeah, cubic/quad are better than just Bezier.

Landed: fuzz_ops.h's OP_BEZIER_TO is OP_CUBIC_TO (same opcode value; the corpus
replays unchanged).

### D9. intern/cache — cache_intern (interns), cache_font (ALSO interns,
name doesn't say), prose calls one structure cache/memo/lookup.

Landed: no ruling recorded; unchanged.

### D10. boxes — de-facto rule largely holds: cbbox = device int x/y/w/h;
bounds/x0..y1 = float corner pairs; dims = w/h.  emit_shadow's
sx0/sy0/sx1/sy1 is the one hand-rolled unnamed box.  blur_box_* = box
FILTER, spec-standard, unrelated.

Landed: no ruling recorded; unchanged.

### D11. parameter letters — t (gradient param, consistent), u (stop lerp
fraction AND pattern uv — two meanings in one chain), k (coverage
fraction in cov_lerp8 vs counts elsewhere), frac (UI fractions,
consistent).
t is good for gradient parameters, and i'd be happy with that for lerp
parameters too.  frac should almost always be named something like foo_frac and
be the fractional part of foo.

Landed: the stop-span lerp fraction is lerp_t; pattern uv keeps the external
texture-coordinate term; gradient t unchanged.

### D12. axis nouns — kind (paint, gradient) is the internal convention;
rule/quality web-fixed; path commands split op (p2d) vs verb (glyph);
filter entries are the one untagged union.

Landed: the p2d/glyph split is healed -- both axes say verb (C12).

### D13. solve/eval/sample — near-coherent: solve/eval in prose,
param/color/sample in identifiers.  Wrinkle: gradient_sample folds
alpha; pattern_sample/sample_src don't.
in general eval() for running math the forward obvious way, solve()
for doing the inverted thing like finding an equation's zero.
sample we should keep to mean "pluck the best color out of some buffer
as if we could index a C array with these float coordinates".  different
strategies for sampling are nearest neighbor, bilerp, etc.

Landed: holds as ruled; no renames forced.

### D14. row/tile/plane/target — clean; "scanline" appears twice
(canvas2d_cover.c) as a stray synonym for row.
Don't mind scanline or framebuffer, since they dont' really mean
anything else, but in general row is just as clear I think.

Landed: no action, as ruled.

### D15. begin/end, enter/leave, open/close — healthy specialization, no
action.

Landed: open/close retired with the recorder's begin/end (D3); the rest
holds.

### D16. degenerate/blank/empty/flat — healthy specialization, no action.

Landed: holds.

### D17. test/tool operands — got/ref (tests), ref/wt + before/after +
pa/pb (gallery_diff uses three vocabularies for its two sides), oracle
(reference impl), s/d (spec).  dst-before-src argument order is
consistent project-wide (memcpy convention).
I like "before/after" or "want/got" depending on how strongly we're talking
about the comparison.  If it's in something like gallery, before/after is sort
of the value neutral term... we don't really know which we want.  In other more
focused automated tests, got/want works pretty well.  I got this, but wanted this.

And yes, happy to follow memcpy() convention.  dst,n or dst,src,n.  A lot of style guides
put mutable parameters at the back, but what I prefer is to put _essential_ parameters
at the front.  No point in doing a memcpy() unless there's a destination, that sort
of reasoining.

Landed: tests compare got against want (the ref operands renamed); reference-
implementation functions keep their ref_/oracle names; argument order already
follows memcpy convention.

### D18. colour vs color — 64 vs 169 in prose; identifiers all color
(spec).  Pick one for prose.
Funny thing is that I am American and write color and grey, so I am
not entirely consistent.  Don't mind either way as long as you pick
one.  I have found your usage of colour quite charming so far.

Landed: colour stays the prose spelling (the signature); identifiers stay color
(spec).

### D19. limit constants — MAX_ prefix vs _MAX suffix vs _N suffix (which
means slots in TABLE_N but insert-cap in CACHE_N) vs lowercase.

I like a MAX suffix, uppercase for #defines.

Landed: CANVAS2D_DIM_MAX, CANVAS2D_DASH_MAX, CANVAS2D_STOPS_MAX join the already-suffixed
REPLAY_*_MAX / CANVAS2D_REC_*_MAX.

### D20. slot (storage cell) vs entry (occupied slot) — held in practice,
unstated.  tick (counter) vs stamp (per-slot snapshot).
This sounds about right.  A table could have 32 slots, with 13 entries and 19
empty slots.  Another term we've used for tick and stamp is generation ID.

Landed: slot/entry hold; the counter pair is (tick, last_use) -- see C13.

### D21. parallel public/internal enums — formerly five mirrored
vocabularies (fill_rule, line_join, line_cap, composite/blend, matrix).

Landed: the single namespace removed the prefix split that justified the
mirror, so `fill_rule`, `line_join`, and `line_cap` are now one enum each
(`enum canvas2d_fill_rule`, `enum canvas2d_line_join`, `enum
canvas2d_line_cap`), defined once in `include/canvas2d_paint_style.h` and
consumed directly by both the public API and the leaf modules (the
rasterizer, the stroker).  The seam `_Static_assert`s and the
public-to-internal conversion casts are gone -- they were identities.
`composite/blend` stays a distinct pair: `enum canvas2d_composite_op` (the
public, web-named operator) versus the internal blend enum -- distinct base
names, not mirrors, so no merge applies.  The matrix mirror is gone: the former
public 6-element affine `canvas2d_matrix` and the internal 9-element
`canvas2d_mat` homography collapsed into a single `canvas2d_matrix` (the 3x3
homography in `include/canvas2d_matrix.h`).  `canvas2d_get_transform` returns
that CTM whole; callers use `canvas2d_matrix_is_affine` to tell the affine
subset (bottom row (0, 0, 1)) from a perspective transform.

### D22. guard/select/discard — consistent.  f16x8_if_then_else (renamed
from _sel); vsel_bits is the stray spelling (vsel_ prefix vs _sel suffix
era; now _if_then_else era).

Landed: f32x8_if_then_else (was vsel_bits), the f32 twin of
f16x8_if_then_else.

## The len/size/count audit

The project's dominant convention is nfoo (nruns, nfonts, nimg, npath,
nverbs, npts, ncmds, nlines, npix...).  A consistent "len = bytes (int)"
dialect exists, largely forced by __counted_by wanting int sibling fields
(size_t is rare by construction).  True offenders — element counts named
len: canvas2d_verts.len, pt_len/sp_len, path2d.len (replay already calls the
same quantity ncmds), rec_path.len, stack_len; the unit-colliding
canvas2d_shaped.text_len (UTF-16 units beside UTF-8-byte text_lens, C2); and
compositor_read(out, len) counting PIXELS one seam from byte lens.
compositor.tn/cn are cryptic counts.  `cnt` is a third count spelling
(geom append, zlib bits).  Notably: nothing spatial is named len — the
"len = spatial" slot is currently empty.

Ideally I would like byte-counts to be counted with size_t, named foo_size and
all other things counted with ints, either nfoo or foos or foo_count.  But we
bend here to what fbounds-safety allows for sure... whole point of the project.

Incidentally I quite like the style of arrays being singular and their counts
being their plurals, e.g.

    struct px *__counted_by(pixels) pixel = ...;
    int const pixels = ...;
    for (int i = 0; i < pixels; i++) {
        pixel[i] = foo(pixel+i, ...);
    }

pixel[i] meaning the i-th pixel and pixel+i as pointer to the i-th pixel
both read very clear to me.

Landed: the offenders took the project's dominant nfoo spelling (npts, nsubs,
nverts, ncmds, nsaved -- see C2); the singular/plural idiom remains available
for new code.

## Rulings

(recorded as made)
- 2026-06-11: lane types are OpenCL-isomorphic — halfN/floatN/intN/shortN/
  ucharN; same spelling as OpenCL = same meaning, different spelling needs
  a reason (f16x8_if_then_else vs OpenCL select(): different argument
  order, hence different name).
- 2026-06-11: k255 split — inv255 is 1/255; k255 is 255 (0f6a0af).
- 2026-06-11 (structural, from the abstraction docket): the compositor stops
  existing — no object, no ABI, no compositor_cpu.c; blending integrates
  holistically into canvas.  Web names at the public surface, blend-family
  names inside.  Enum mirrors elsewhere: coordinator's call — the leaf
  modules (cover, stroke) keep their minimal vocabularies (genuinely
  standalone algorithms; asserts pin the seams); the matrix pair stays.
  APIs bend toward the web: per-call beats stateful (fill rule moves to the
  call, set_fill_rule dies; create_image_data drops its ignored canvas).
  The selection/caret text API is REAL, not neglected: gallery demos coming.
  Large coherent files are fine — one concern per file, large concerns make
  large files.
- 2026-06-11 (types): enums always `enum foo`, never typedef'd.  Structs
  split by LITERAL C usage, read off the call signatures: worked with
  almost always by copying values -> typedef (f32x8, canvas2d_vec2, canvas2d_px8,
  canvas2d_matrix); worked with almost always through pointer indirection ->
  tagged `struct foo`, no typedef, spelled at every use (struct canvas2d_context,
  struct canvas2d_gradient -- its functions all take pointers, the occasional
  save/restore copy notwithstanding).  Anything with a constructor or free
  is tagged by nature.  Constructors are the bare type name
  (`struct canvas2d_context *canvas2d(...)`); destructors are `foo_free()`,
  NULL-accepting like free() itself.
- 2026-06-11 (implementation): all of the above is in the tree.  Tagged:
  struct canvas2d_context, canvas2d_path2d, canvas2d_recorder, canvas2d_gradient, canvas2d_pattern,
  canvas2d_shaped, canvas2d_cover, canvas2d_verts, canvas2d_path, canvas2d_text_cache,
  canvas2d_font, canvas2d_glyph_slot, canvas2d_shaping_slot, canvas2d_font_name, rec_image,
  rec_path.  Typedefs kept (value types): canvas2d_vec2, canvas2d_matrix, cbbox,
  canvas2d_premul, canvas2d_unpremul, canvas2d_px8, gradpx8, rgb8, foldv8, the lane
  types, canvas2d_stop, canvas2d_subpath, canvas2d_filter, canvas2d_mip, canvas2d_xspan,
  p2d_cmd, canvas2d_text_metrics, canvas2d_shaped_metrics.
  AWAITING RULING: canvas2d_glyph_run -- genuinely mixed (~50/50; the checked
  core copies whole runs by value per loop, the recorder/replay walk them
  by pointer inside a __counted_by array); keeps its typedef until called.
  The per-call structural ruling is live too: canvas2d_fill(cv, rule) and
  canvas2d_clip(cv, rule), set_fill_rule and its state field gone, the
  format's fill/clip tokens carrying the rule by name, and
  canvas2d_create_image_data taking no canvas.
- 2026-06-11: canvas2d_glyph_run is TAGGED (`struct canvas2d_glyph_run`) — the punted
  50/50 resolved by Mike: "it's complex, got internal pointers."  Internal
  pointers join the criterion: a by-value copy of such a struct is shallow
  (copies share the arrays), so it is reference-flavored even where loops
  copy it — complexity + internal pointers tag a type regardless of the
  occasional value copy.
