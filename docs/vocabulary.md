# Vocabulary: the survey and the rulings

Status: **survey complete, rulings in progress.** Same word should mean one
thing; one thing should have one word; generic looks generic, specific
specific.  Precedents, in order: the web spec's names where we implement it,
OpenCL's names for lane/vector vocabulary, Mike's len rule (size = bytes,
nfoo/foo_count = element counts, len = spatial).  Each family below carries
its sites and is awaiting or recording a call.

## Collisions (same word, different meanings)

### C1. `cap` — four meanings, the worst token in the tree
- capacity (cnvs_verts.cap, pt_cap/sp_cap, stack_cap, cov_cap, tile_cap,
  bitwr.cap, vcap/pcap, get_line_dash(out, cap)) — Go/stb-standard usage
- line cap (canvas_line_cap, emit_cap) — W3C term, immovable
- capture (cnvs_glyph_slot.cap_w/cap_h/cap_len) — `cap_len` reads as
  "capacity length", means "capture byte size"
- upper limit (read_uint(..., cap), "the 64 KiB line cap", REPLAY_*_MAX
  prose)
All but line-cap coexist inside cnvs_text.h/cnvs_replay.c simultaneously.

When working with line-cap, always keep the words line-cap in it.  If needed to
abbreviate, lc is better than cap.  Let's try to have "cap" refer a buffer's
capacity, and try to find better names for other use cases.

### C2. `len` — one name, four units
- bytes: canvas_read_rgba(out, len), owned_image.len, shape_slot.len,
  replay text_len, zlen/blen/glen/slen...
- pixels: compositor_read(out, len) — the direct seam partner of
  read_rgba's byte len; same name changes unit across one call
- elements: cnvs_verts.len, pt_len/sp_len, path2d.len, stack_len
- UTF-16 units: cnvs_shaped.text_len — while cnvs_shape()'s text_len
  parameter four lines away is UTF-8 BYTES

Try to keep geometric length spelled out as length, and avoid len otherwise...
byte counts should be "foo_size", counts of other things "nfoo" or "foo_count"
(you pick which you like and use it everywhere)


### C3. `quant8` — two unrelated operations
- blur.c quant8: exact rounded integer divide via reciprocal+snap
- canvas.c unpremul_quant8: float -> unorm8 conversion
- (cover_to_u8 IS the unorm8 quantize, named _to_u8; cnvs_f2u8 is a
  saturating convert of an already-0..255 value, NOT unorm)
Field standard: unorm8 (Vulkan/D3D/Metal UNORM) for [0,1]->u8.

Yep, unorm where appropriate, and maybe for the other case some sort of "exact"
wording rather than quant.

### C4. `k255` — FIXED (0f6a0af): compositor's 1/255 is now inv255;
canvas.c's 255.0 keeps k255.

Yeah, k255 should be 255, inv255 for 1/255.

### C5. `mask` — clip mask (u8 plane) / SIMD compare mask (short8, was
mask8 — type already renamed) / shadow silhouette mask.  Clip and shadow
masks are arguably one concept (a u8 coverage plane) under two names.

I don't mind clip and shadow both talking about masks, as long as it's just
within the context where clip or shadow is implicit.  Outside that
disambiguate: clip mask, shadow mask

### C6. `block` — 8-pixel planar block / .canvas format block / DEFLATE
block (RFC-fixed) / staged vertex block / 2x2 mip block.  Planar-vs-format
collide hard in cnvs_record/replay comments.

Hmm sometimes I've used `slab` for the idea of an 8-pixel block.  Does that
conflict with anything else?

### C7. `half` — the f16 type family vs win/2 rounding bias (blur quant8)
vs 0.5f bias (unpremul_quant8 declares `_Float16 const half` amid half8
types) vs half-width (stroke hw).

`half` on its own should be fp16, very idiomatic.  I don't see any conflict
as long as half-width keeps width in there.

### C8. `run` — glyph run / running winding sum (cover_to_u8(rule, run)) /
dash on-runs.
Again, these are fine in context of an area of code where it's obvious, and
then disambiguate with glyph run, running sum, etc should it ever come up.

### C9. `shape` — text shaping (cnvs_shape, shape blocks) vs geometric
shape (prose); `shade8` sits one letter away in the same file.
Yeah, this one I think we should probably use `shaping` for text shaping and
`shape` to mean the geometric concept.

### C10. `filter` — CSS filter vs PNG row filter.  Both spec-fixed;
unavoidable at the spec edge.
No big deal I think.  Not super ambiguous in context.

### C11. `saturate`/`sat` — clamping conversion (cnvs_f2i docs) / CSS
saturate() / HSL saturation (sat8, set_sat8 — reads as "saturating 8-bit",
is not).
Let's leave sat/saturate to only where it corresponds to something external
like CSS.  Maybe we can use clamped as the term for something that's guaranteed
in a range.  clamp() and clamp01() are my favorites for producing those things
for sure, so clamped seems natural.

### C12. `op` — composite op (public) / "the op" (a draw) / path verb
(p2d_op) / fuzz opcode + recorder command (cnvs_rec_op).  p2d_op vs
cnvs_glyph_verb: same concept, two axis nouns.
I think op is just too short and ambiguous to use outside very local settings.
As you've written, compositing operation, a draw, a path verb.. these things
kind of are better spelled out in broad contexts, all fine short in local
contexts.  Definitely pick verb over op if we're using both for paths.

### C13. `stamp` — LRU last-use tick (shape_slot.stamp, fed by cache.tick)
vs stamping coverage into a mask (verb).
Last-use or timestamp I guess?

### C14. `bitmap` — format token vs the canvas backing store; and the
format token (`bitmap`) names what the code calls a `capture`.

bitmap should pretty much always be a dense 2D array of pixels.  anything else
we should be scrupulous to find another word.

### C15. `px` — buffer pointer / font size px / blur stdDev px / loop
index / cnvs_px8 (8 pixels) vs tests' px4 (4 channels of ONE pixel —
numeric suffix counts different things).

px should always at least refer to a pixel.  It's okay as both a "this pixel"
value/pointer, and as a unit measurement.  If px4 is 4 channels of one pixel,
that's wrong, that should be just px or rgba, channel4 at worst.  px4 sounds
like 4 pixels.

### C16. Single letters
- r: radius vs red — ADJACENT in emit_shadow (int r radius; cr = sc.r red)
- a: alpha / matrix entry / first operand / quadratic coefficient (in a
  function also holding colour alpha)
- k: tail count / coverage fraction / scale factor / COSINE
  (cnvs_mat_rotate) — count is dominant
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

### C17. Font ids — `fid` (interned cache id) = `name_id` (same thing in a
run) vs `id` (the FILE-LOCAL id, a different space, mapped via map[]).
'id' is like 'op', meaningless unless given more context.  in a very local
case it's fine, but don't let it leak out without a little more description

## Drift (different words, one meaning)

### D1. clamp family — clamp01 DEFINED TWICE with DIFFERENT NaN behavior
(canvas.c NaN->0 via !(v>0); cnvs_gradient.c plain min/max lets NaN
through).  Bug-adjacent.  Plus clamp_lo, clampi, vclamp01,
cnvs_px8_clamp_premul (whose comment says "pins").  "pin" = clamp in
prose only; "pin" also means anchor-in-place (patterns, fonts) — two
concepts sharing the prose word.

I like any clamp() function to absolutely strongly guarantee that its output
will be between the requested bounds no matter what.  clamp01(NaN) needs to
return _some_ value in [0,1], etc.  (of course if NaN is one of the boundaries
this is a meaningless promise but why would we ever do that)

### D2. Composite axis — "op" (public canvas_composite_op, CANVAS_OP_*,
SOURCE/DESTINATION) vs "blend mode" (compositor_blend_mode,
COMPOSITOR_SRC_*/DST_*) vs "composite" (state field cur.composite).
Mirrored by static_asserts.  Skia says blend mode; the web says composite
operation.

I do like the term "blend", blend mode, blend_fn, just blend... these are any
of those f(px,px) -> px operations we do at the end of the pipeline, whether
porter duff, separable, non-separable, user-defined, whatever.  Of course at
the interface we want to hew to canvas 2D's terms, but I don't mind switching
right away to blend-type names inside.

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

### D4. reset/clear — empty-keep-storage is `reset` 3x and `clear` 1x
(pattern_clear); free-everything is `clear` (text_cache) and `free`
elsewhere.  canvas_reset/clear_rect are web-fixed.

clear for places where we're doing something like bzero, reset for things that
are managing memory and state tracking.  There's some overlap probably, but
that should be the general gist.

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

### D6. Read verbs — load (mem->reg AND file->mem: canvas_load_png),
read (bulk copy-out AND parse AND cnvs_png_read = file), get (web +
accessors + getbits), fetch/peek (prose).  File I/O is load publicly,
read internally, for the same operation.

### D7. premul family — coherent rule emerges: abbreviated
premul/unpremul = the data format; spelled-out premultiply = the
operation.  One breaker: unpremul_quant8 (operation, abbreviated).

I do like your convention of premul/unpremul for the data,
premultiply/unpremultiply for the operations that translate between the two.

### D8. Curves — internal quad/cubic consistent; public web names
immovable; fuzz_ops.h's OP_BEZIER_TO mixes layers (should be
OP_CUBIC_TO).

Yeah, cubic/quad are better than just Bezier.

### D9. intern/cache — cache_intern (interns), cache_font (ALSO interns,
name doesn't say), prose calls one structure cache/memo/lookup.

### D10. boxes — de-facto rule largely holds: cbbox = device int x/y/w/h;
bounds/x0..y1 = float corner pairs; dims = w/h.  emit_shadow's
sx0/sy0/sx1/sy1 is the one hand-rolled unnamed box.  blur_box_* = box
FILTER, spec-standard, unrelated.

### D11. parameter letters — t (gradient param, consistent), u (stop lerp
fraction AND pattern uv — two meanings in one chain), k (coverage
fraction in cov_lerp8 vs counts elsewhere), frac (UI fractions,
consistent).
t is good for gradient parameters, and i'd be happy with that for lerp
parameters too.  frac should almost always be named something like foo_frac and
be the fractional part of foo.

### D12. axis nouns — kind (paint, gradient) is the internal convention;
rule/quality web-fixed; path commands split op (p2d) vs verb (glyph);
filter entries are the one untagged union.

### D13. solve/eval/sample — near-coherent: solve/eval in prose,
param/color/sample in identifiers.  Wrinkle: gradient_sample folds
alpha; pattern_sample/sample_src don't.
in general eval() for running math the forward obvious way, solve()
for doing the inverted thing like finding an equation's zero.
sample we should keep to mean "pluck the best color out of some buffer
as if we could index a C array with these float coordinates".  different
strategies for sampling are nearest neighbor, bilerp, etc.

### D14. row/tile/plane/target — clean; "scanline" appears twice
(cnvs_cover.c) as a stray synonym for row.
Don't mind scanline or framebuffer, since they dont' really mean
anything else, but in general row is just as clear I think.

### D15. begin/end, enter/leave, open/close — healthy specialization, no
action.

### D16. degenerate/blank/empty/flat — healthy specialization, no action.

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

### D18. colour vs color — 64 vs 169 in prose; identifiers all color
(spec).  Pick one for prose.
Funny thing is that I am American and write color and grey, so I am
not entirely consistent.  Don't mind either way as long as you pick
one.  I have found your usage of colour quite charming so far.

### D19. limit constants — MAX_ prefix vs _MAX suffix vs _N suffix (which
means slots in TABLE_N but insert-cap in CACHE_N) vs lowercase.

I like a MAX suffix, uppercase for #defines.

### D20. slot (storage cell) vs entry (occupied slot) — held in practice,
unstated.  tick (counter) vs stamp (per-slot snapshot).
This sounds about right.  A table could have 32 slots, with 13 entries and 19
empty slots.  Another term we've used for tick and stamp is generation ID.

### D21. parallel public/internal enums — five mirrored vocabularies
(fill_rule, line_join, line_cap, composite/blend, matrix), each pinned at
the seam.  Deliberate layering; the cost is sync.
I don't mind this and think it's healthy, especially with asserts that keep them lockstep.

### D22. guard/select/discard — consistent.  half8_if_then_else (renamed
from _sel); vsel_bits is the stray spelling (vsel_ prefix vs _sel suffix
era; now _if_then_else era).

## The len/size/count audit

The project's strongest convention is nfoo (nruns, nfonts, nimg, npath,
nverbs, npts, ncmds, nlines, npix...).  A consistent "len = bytes (int)"
dialect exists, largely forced by __counted_by wanting int sibling fields
(size_t is rare by construction).  True offenders — element counts named
len: cnvs_verts.len, pt_len/sp_len, path2d.len (replay already calls the
same quantity ncmds), rec_path.len, stack_len; the unit-colliding
cnvs_shaped.text_len (UTF-16 units beside UTF-8-byte text_lens, C2); and
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

## Rulings

(recorded as made)
- 2026-06-11: lane types are OpenCL-isomorphic — halfN/floatN/intN/shortN/
  ucharN; same spelling as OpenCL = same meaning, different spelling needs
  a reason (half8_if_then_else vs OpenCL select(): different argument
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
