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

### C2. `len` — one name, four units
- bytes: canvas_read_rgba(out, len), owned_image.len, shape_slot.len,
  replay text_len, zlen/blen/glen/slen...
- pixels: compositor_read(out, len) — the direct seam partner of
  read_rgba's byte len; same name changes unit across one call
- elements: cnvs_verts.len, pt_len/sp_len, path2d.len, stack_len
- UTF-16 units: cnvs_shaped.text_len — while cnvs_shape()'s text_len
  parameter four lines away is UTF-8 BYTES

### C3. `quant8` — two unrelated operations
- blur.c quant8: exact rounded integer divide via reciprocal+snap
- canvas.c unpremul_quant8: float -> unorm8 conversion
- (cover_to_u8 IS the unorm8 quantize, named _to_u8; cnvs_f2u8 is a
  saturating convert of an already-0..255 value, NOT unorm)
Field standard: unorm8 (Vulkan/D3D/Metal UNORM) for [0,1]->u8.

### C4. `k255` — FIXED (0f6a0af): compositor's 1/255 is now inv255;
canvas.c's 255.0 keeps k255.

### C5. `mask` — clip mask (u8 plane) / SIMD compare mask (short8, was
mask8 — type already renamed) / shadow silhouette mask.  Clip and shadow
masks are arguably one concept (a u8 coverage plane) under two names.

### C6. `block` — 8-pixel planar block / .canvas format block / DEFLATE
block (RFC-fixed) / staged vertex block / 2x2 mip block.  Planar-vs-format
collide hard in cnvs_record/replay comments.

### C7. `half` — the f16 type family vs win/2 rounding bias (blur quant8)
vs 0.5f bias (unpremul_quant8 declares `_Float16 const half` amid half8
types) vs half-width (stroke hw).

### C8. `run` — glyph run / running winding sum (cover_to_u8(rule, run)) /
dash on-runs.

### C9. `shape` — text shaping (cnvs_shape, shape blocks) vs geometric
shape (prose); `shade8` sits one letter away in the same file.

### C10. `filter` — CSS filter vs PNG row filter.  Both spec-fixed;
unavoidable at the spec edge.

### C11. `saturate`/`sat` — clamping conversion (cnvs_f2i docs) / CSS
saturate() / HSL saturation (sat8, set_sat8 — reads as "saturating 8-bit",
is not).

### C12. `op` — composite op (public) / "the op" (a draw) / path verb
(p2d_op) / fuzz opcode + recorder command (cnvs_rec_op).  p2d_op vs
cnvs_glyph_verb: same concept, two axis nouns.

### C13. `stamp` — LRU last-use tick (shape_slot.stamp, fed by cache.tick)
vs stamping coverage into a mask (verb).

### C14. `bitmap` — format token vs the canvas backing store; and the
format token (`bitmap`) names what the code calls a `capture`.

### C15. `px` — buffer pointer / font size px / blur stdDev px / loop
index / cnvs_px8 (8 pixels) vs tests' px4 (4 channels of ONE pixel —
numeric suffix counts different things).

### C16. Single letters
- r: radius vs red — ADJACENT in emit_shadow (int r radius; cr = sc.r red)
- a: alpha / matrix entry / first operand / quadratic coefficient (in a
  function also holding colour alpha)
- k: tail count / coverage fraction / scale factor / COSINE
  (cnvs_mat_rotate) — count is dominant
- q: quarter-pi / quotient / query point / staged pixel

### C17. Font ids — `fid` (interned cache id) = `name_id` (same thing in a
run) vs `id` (the FILE-LOCAL id, a different space, mapped via map[]).

## Drift (different words, one meaning)

### D1. clamp family — clamp01 DEFINED TWICE with DIFFERENT NaN behavior
(canvas.c NaN->0 via !(v>0); cnvs_gradient.c plain min/max lets NaN
through).  Bug-adjacent.  Plus clamp_lo, clampi, vclamp01,
cnvs_px8_clamp_premul (whose comment says "pins").  "pin" = clamp in
prose only; "pin" also means anchor-in-place (patterns, fonts) — two
concepts sharing the prose word.

### D2. Composite axis — "op" (public canvas_composite_op, CANVAS_OP_*,
SOURCE/DESTINATION) vs "blend mode" (compositor_blend_mode,
COMPOSITOR_SRC_*/DST_*) vs "composite" (state field cur.composite).
Mirrored by static_asserts.  Skia says blend mode; the web says composite
operation.

### D3. Lifecycle verbs — create/destroy (canvas, compositor, path2d,
font), open/close (recorder), init/free-ish (path, cover, verts:
reset/free), init/clear-as-destructor (text_cache), release (font, CF
echo), drop (replay parser state).  Hazard: text_cache_clear FREES;
verts_reset does NOT.

### D4. reset/clear — empty-keep-storage is `reset` 3x and `clear` 1x
(pattern_clear); free-everything is `clear` (text_cache) and `free`
elsewhere.  canvas_reset/clear_rect are web-fixed.

### D5. Write verbs — store (reg->mem), write (->file), put (web API /
cache insert / bit emission / curve sink — QUADRUPLE duty), emit (stroke
tessellation / shadow compositing / recorder serialization — emit_shadow
composites pixels, emit_quad appends triangles), set
(cache_set_vmetrics is actually a first-wins insert), deposit (cover —
unique, good), stamp.

### D6. Read verbs — load (mem->reg AND file->mem: canvas_load_png),
read (bulk copy-out AND parse AND cnvs_png_read = file), get (web +
accessors + getbits), fetch/peek (prose).  File I/O is load publicly,
read internally, for the same operation.

### D7. premul family — coherent rule emerges: abbreviated
premul/unpremul = the data format; spelled-out premultiply = the
operation.  One breaker: unpremul_quant8 (operation, abbreviated).

### D8. Curves — internal quad/cubic consistent; public web names
immovable; fuzz_ops.h's OP_BEZIER_TO mixes layers (should be
OP_CUBIC_TO).

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

### D12. axis nouns — kind (paint, gradient) is the internal convention;
rule/quality web-fixed; path commands split op (p2d) vs verb (glyph);
filter entries are the one untagged union.

### D13. solve/eval/sample — near-coherent: solve/eval in prose,
param/color/sample in identifiers.  Wrinkle: gradient_sample folds
alpha; pattern_sample/sample_src don't.

### D14. row/tile/plane/target — clean; "scanline" appears twice
(cnvs_cover.c) as a stray synonym for row.

### D15. begin/end, enter/leave, open/close — healthy specialization, no
action.

### D16. degenerate/blank/empty/flat — healthy specialization, no action.

### D17. test/tool operands — got/ref (tests), ref/wt + before/after +
pa/pb (gallery_diff uses three vocabularies for its two sides), oracle
(reference impl), s/d (spec).  dst-before-src argument order is
consistent project-wide (memcpy convention).

### D18. colour vs color — 64 vs 169 in prose; identifiers all color
(spec).  Pick one for prose.

### D19. limit constants — MAX_ prefix vs _MAX suffix vs _N suffix (which
means slots in TABLE_N but insert-cap in CACHE_N) vs lowercase.

### D20. slot (storage cell) vs entry (occupied slot) — held in practice,
unstated.  tick (counter) vs stamp (per-slot snapshot).

### D21. parallel public/internal enums — five mirrored vocabularies
(fill_rule, line_join, line_cap, composite/blend, matrix), each pinned at
the seam.  Deliberate layering; the cost is sync.

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

## Rulings

(recorded as made)
- 2026-06-11: lane types are OpenCL-isomorphic — halfN/floatN/intN/shortN/
  ucharN; same spelling as OpenCL = same meaning, different spelling needs
  a reason (half8_if_then_else vs OpenCL select(): different argument
  order, hence different name).
- 2026-06-11: k255 split — inv255 is 1/255; k255 is 255 (0f6a0af).
