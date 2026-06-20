#pragma once

// The text subsystem: fonts, Core Text shaping, glyph outlines/bitmaps, and text
// metrics.  Core Text shapes UTF-8 into glyph runs inside the unsafe boundary TU
// (cnvs_text_ct.c, built without -fbounds-safety to bind the un-annotated CoreText
// headers); that TU copies each run's glyphs, advances, and cluster map into
// checked-owned __counted_by arrays and hands them back.  The checked core
// (cnvs_text.c) then does layout and hit-testing fully bounds-checked.
//
// The run crosses the C<->C boundary by plain (pointer, count) ABI -- no forge --
// because __counted_by(count) ties the bound to the sibling `count` field and adds
// no hidden field.  The only trust placed in the unsafe side is that `count` matches
// the arrays; a bad cluster *value* is caught by an explicit range check in the core.
// See docs/text-boundary.md.

#include "cnvs_math.h"
#include "cnvs_path.h"

#include <ptrcheck.h>
#include <stdint.h>

// One shaped run: a single font and direction.  Glyphs are in visual (left-to-right)
// order; cluster[i] is the logical UTF-16 index in the source for glyph i (so it
// descends across an RTL run, and skips for a ligature that merged several chars).
struct cnvs_glyph_run {
    uint16_t *__counted_by(count) glyph;    // glyph ids
    float *__counted_by(count) xadv;         // x advance per glyph, user px
    int32_t *__counted_by(count) cluster;    // source UTF-16 index per glyph (logical)
    int count;
    bool rtl;
    bool is_color;  // a color (emoji) font's run: glyphs are drawn, not outlined.
                    // Set at the boundary when the run is copied out, so the draw
                    // walk never needs a per-run boundary query.
    int name_id;    // interned font-name id in the owning canvas's text cache, or
                    // -1 = resolve through `font`.  Live runs from the boundary
                    // carry -1; runs rebuilt by replay carry the recorded id and
                    // may have font == NULL -- their curves and metrics come from
                    // the cache by name id, no CTFontRef needed to draw.
    void *__single font;   // opaque CTFontRef for this run (font fallback),
                           // retained; NULL for a replay-built run
};

struct cnvs_shaped {
    struct cnvs_glyph_run *__counted_by(nruns) run;
    int nruns;
    int utf16s;     // source length in UTF-16 units, the bound for cluster indices
    float size_px;  // font size the line was shaped at, user px (every run's font,
                    // fallback included, is at this size; the numerator of the
                    // canonical-curve px scale)
    int weight;     // CSS 100..900 the line was shaped at; part of the glyph-cache
    bool italic;    // identity (with the family) so a SYNTHESIZED bold/italic --
                    // which reports the regular face's name -- does not alias it
};

// Shape UTF-8 `text` with font `name` at `size_px`.  Runs come back in visual order.
// `rtl` is the PARAGRAPH base direction (the canvas direction attribute): it sets
// the bidi base level the runs are ordered against, so a mixed-direction string
// reorders -- and its neutrals resolve -- differently under ltr and rtl.  Always
// explicit, never first-strong: the spec resolves the attribute, not the text.
// `weight` (CSS 100..900) and `italic` build the shaping font through a font
// descriptor's traits, so the family's nearest real bold/italic face is matched
// -- or, when it has none, the platform synthesizes one (the requested trait is
// honoured, never silently dropped to regular).  `kerning` and `rendering` are
// the canvas_font_kerning / canvas_text_rendering enums as ints: they set
// kerning and ligature attributes on the shaped run (kerning NONE or rendering
// OPTIMIZE_SPEED disables kerning; OPTIMIZE_SPEED also disables ligatures; the
// other values leave Core Text's defaults).  `lang` is a BCP-47 tag (lang_len
// bytes, may be empty) set as the run's language for locale-dependent glyph
// selection; empty sets none.  These are shaping INPUTS (they change advances/
// glyph choice, baked into the runs), not glyph-outline identity.
// NULL on failure.  Implemented in the unsafe boundary TU.  All strings cross the
// boundary as counted (bytes, len) slices -- CFStringCreateWithBytes takes exactly
// that -- so checked callers hand over the (ptr, len) pairs they already hold: no
// NUL-terminated copy exists anywhere internal, and the shim cannot over-read a
// non-terminated buffer.  (The public canvas.h API keeps its __null_terminated
// ergonomics; the strlen happens once at that entry point, in-rules.)
struct cnvs_shaped *__single cnvs_shape_text(char const *__counted_by(name_len) name, int name_len,
                                 float size_px, bool rtl, int weight, bool italic,
                                 int kerning, int rendering,
                                 char const *__counted_by(lang_len) lang, int lang_len,
                                 char const *__counted_by(text_len) text, int text_len);
void cnvs_shaped_free(struct cnvs_shaped *__single s);

// Checked-core consumers.
float cnvs_shaped_width(struct cnvs_shaped const *__single s);                // sum of advances

// Bake CSS letterSpacing/wordSpacing into a shaped line's advances, in place,
// before it is interned in the cache (so both measureText and the draw walk see
// them, and replay -- which has no source text -- reproduces them from the cached
// advances alone).  `ls` rides the last glyph of every typographic cluster (a
// cluster = a maximal run of glyphs sharing one cluster[] value, the final
// cluster included); `ws` rides the last glyph of each cluster whose source
// character is U+0020 SPACE, found by mapping cluster[] (a UTF-16 index) to the
// source code point with a UTF-8<->UTF-16 walk over `text`.  Advances stay
// positive in ltr and rtl runs alike.  ls == 0 && ws == 0 is a pure no-op,
// leaving every advance bit-identical.  Both may be negative.
void cnvs_shaped_apply_spacing(struct cnvs_shaped *__single s,
                               char const *__counted_by(text_len) text, int text_len,
                               float ls, float ws);
int cnvs_shaped_index_at_x(struct cnvs_shaped const *__single s, float x);    // hit-test -> UTF-16 index, or -1

// A visual x range [x0,x1) in user px from the line start (a selection highlight rect).
typedef struct {
    float x0, x1;
} cnvs_xspan;

// Caret: visual x for a logical UTF-16 index -- the left visual edge of the glyph
// whose cluster contains it.  An index inside a cluster (a surrogate pair's low
// half, a ligature's interior) snaps to that cluster's edge; an index at or past
// the source's end is the caret after the last glyph (the line's advance width).
float cnvs_shaped_x_at_index(struct cnvs_shaped const *__single s, int index);

// Selection: visual x-spans covering the logical range [lo,hi).  A bidi range maps to
// non-contiguous visual positions and so splits into multiple spans; writes up to
// `max`, returns the count.  Pure index logic: the cluster map drives it and every
// access is bounds-checked -- this is where the checked side earns its keep.
int cnvs_shaped_selection(struct cnvs_shaped const *__single s, int lo, int hi,
                          cnvs_xspan *__counted_by(max) out, int max);

// Copy a run's font name into `buf` (UTF-8, NUL-terminated within `cap`); returns the
// byte length, or -1.  Boundary helper: the opaque font handle goes in, an output
// buffer the boundary fills within `cap` comes back.  Used to confirm fallback (a run
// that fell back to a different font reports a different name).
int cnvs_run_font_name(void const *__single font, char *__counted_by(cap) buf, int cap);

// A glyph outline crosses the boundary as canonical curve data: verbs plus control
// points in FONT UNITS -- the font's design grid, `units_per_em` units to the em,
// y up, baseline-relative, x from the glyph origin.  The same bytes describe the
// glyph at every size, pen, and transform; the checked core scales them by
// size_px / units_per_em (flipping y for canvas's y-down user space), places them
// at the pen, maps them through the CTM, and flattens -- nothing device- or
// call-specific crosses.
enum cnvs_glyph_verb : uint8_t {
    CNVS_GLYPH_MOVE,   // 1 point
    CNVS_GLYPH_LINE,   // 1 point
    CNVS_GLYPH_QUAD,   // 2 points: control, end
    CNVS_GLYPH_CUBIC,  // 3 points: control 1, control 2, end
    CNVS_GLYPH_CLOSE,  // no points
};

// Fetch one glyph's canonical curves.  The checked caller owns the buffers and the
// boundary fills within their caps (the cnvs_glyph_draw hand-off, twice over); the
// true counts come back in *nverbs/*npts and may exceed the caps, in which case the
// caller grows its buffers and calls again.  *units_per_em is the font's design-grid
// resolution, the denominator of the px scale.  A blank or color (emoji) glyph has
// no outline: both counts come back 0.  Boundary helper.
void cnvs_glyph_curves(void *__single font, uint16_t glyph,
                       enum cnvs_glyph_verb *__counted_by(vcap) verb, int vcap,
                       cnvs_vec2 *__counted_by(pcap) pt, int pcap,
                       int *__single nverbs, int *__single npts,
                       float *__single units_per_em);

// ---------------------------------------------------------------------------
// The params -> derived-data lookup: a per-canvas memo of boundary results,
// consulted BEFORE Core Text is called, populated live from what the boundary
// hands back.  Two maps:
//
//   - shaped lines, keyed by (font family, size_px bits, paragraph direction,
//     letterSpacing bits, wordSpacing bits, weight, style, kerning, rendering,
//     lang bytes, text bytes) -> the
//     struct cnvs_shaped.  The family is IN the key (the REQUESTED name, not the
//     resolved one): two families of the same bytes/size are two distinct lines.
//     Direction is IN the key because the same bytes shape
//     differently under ltr and rtl paragraphs (run order, neutral
//     resolution); leaving it out would alias the two.  letterSpacing and
//     wordSpacing are IN the key because they are baked into the line's
//     advances (cnvs_shaped_apply_spacing) before it is interned, so two
//     spacings of the same bytes are two distinct lines.  weight and style are
//     IN the key because they pick a different (real or synthesized) face, so
//     bold and regular of the same bytes are two distinct lines.  kerning,
//     rendering, and lang are IN the key because they are shaping inputs that
//     change the runs' advances (kerning), ligature formation (rendering), and
//     glyph selection (lang), so two toggle settings of the same bytes are two
//     distinct lines.  The cache OWNS
//     its entries (each run's retained CTFontRef stays alive with them); call
//     sites borrow.  Fixed CNVS_SHAPING_CACHE_N slots, evicted
//     least-recently-used: LRU keeps the measure-then-draw pair and a frame's
//     repeated labels hot, and a 64-entry linear scan is cheaper than a
//     more complex structure here.
//   - glyph data, keyed by (font name, weight, style, glyph id) -> the glyph's
//     canonical
//     form: font-unit verbs/points + units_per_em from cnvs_glyph_curves for
//     an outline glyph, or the fixed-size premultiplied capture from one
//     cnvs_glyph_draw for a color (emoji) glyph.  Canonical bytes are size-
//     and transform-independent, so one entry serves every draw.  Font
//     identity is the interned (font NAME, weight, style) triple (stable across
//     processes -- what the serialized form of this lookup needs), not the
//     CTFontRef pointer.  weight/style join the identity because a SYNTHESIZED
//     bold/italic reports the regular face's NAME -- keying by name alone would
//     return the regular outline for synth-bold; a real bold/italic face already
//     has a distinct name and so separates on name regardless.
//     Open-addressed, fixed CNVS_GLYPH_TABLE_N slots; inserts simply stop at
//     CNVS_GLYPH_CACHE_N entries (no eviction: canonical bytes never go
//     stale, and a scene with >1k distinct glyphs just degrades the rest to
//     boundary calls).  Blank glyphs cache as empty entries, so a space
//     costs one boundary call ever.
//
// The cache is transparent: a hit and a miss render identically, and any
// cache-side allocation failure degrades that lookup to a plain boundary call
// rather than failing the op.  See docs/text-boundary.md.

enum {
    CNVS_SHAPING_CACHE_N = 64,    // shaped-line slots (LRU eviction)
    CNVS_GLYPH_CACHE_N = 1024,  // max cached glyphs (inserts stop, no eviction)
    CNVS_GLYPH_TABLE_N = 2048,  // open-addressing slots: 2x entries, power of two
    CNVS_FONT_INTERN_N = 16,    // distinct font names (primary + fallbacks)
    CNVS_CAPTURE_EM = 160,      // color-glyph capture size: the em in capture px
                                // (160 = AppleColorEmoji's largest bitmap strike)
};

// One derived mip level of a color glyph's capture: the 2x2 box-halving of the
// level above (odd dimensions ceil-halve with the edge row/column replicated),
// premultiplied RGBA8 like the capture itself.  Derived data only -- rebuilt
// from the capture on demand, never serialized.
typedef struct {
    uint8_t *__counted_by(len) px;  // owned premultiplied RGBA8, w*h*4 bytes
    int len;
    int w, h;
} cnvs_mip;

// One cached shaped line.  Empty when s == NULL.
struct cnvs_shaping_slot {
    char *__counted_by(len) text;  // owned key copy of the source bytes
    int len;
    char *__counted_by(fam_len) fam;  // owned key copy of the requested family
    int fam_len;                      // (the REQUESTED name, not the resolved one)
    uint32_t size_bits;  // size_px's float bits: exact-bits keying, no epsilon
    uint32_t ls_bits;    // letterSpacing's float bits: keyed exact-bits like size
    uint32_t ws_bits;    // wordSpacing's float bits: keyed exact-bits like size
    int weight;          // CSS 100..900, the key's weight part
    bool italic;         // fontStyle italic, the key's style part
    bool rtl;            // paragraph direction, the key's third part
    int kerning;         // canvas_font_kerning, the key's kerning part
    int rendering;       // canvas_text_rendering, the key's rendering part
    char *__counted_by(lang_len) lang;  // owned key copy of the BCP-47 lang tag
    int lang_len;                       // (0 = no language)
    uint64_t last_use;   // tick at last use, the LRU ordering
    struct cnvs_shaped *__single s;  // owned; freed on eviction/clear
    bool emitted;  // already serialized into the active recording (cnvs_record.c)
};

// One cached glyph.  An outline glyph holds canonical curves; a color (emoji)
// glyph holds its canonical CAPTURE instead -- one premultiplied RGBA8 render
// of the glyph at CNVS_CAPTURE_EM px to the em, rasterized by the boundary
// once per (font, glyph) ever and sampled (never re-rasterized) at draw time.
// A blank glyph of either kind caches as upem == 0 / capture_w == 0 with no data --
// "known to be blank" is itself a boundary result worth keeping.
struct cnvs_glyph_slot {
    enum cnvs_glyph_verb *__counted_by(nverbs) verb;  // owned canonical curves
    cnvs_vec2 *__counted_by(npts) pt;
    int nverbs, npts;
    float upem;    // units_per_em; 0 for a glyph with no outline (color glyphs too)
    float ink_x0, ink_y0, ink_x1, ink_y1;  // tight ink box, y up, baseline-
                   // relative (x0,y0 bottom-left).  FONT UNITS for an outline
                   // glyph (scale by size_px/upem); CAPTURE PX for a color
                   // glyph (scale by size_px/CNVS_CAPTURE_EM).  Size-independent
                   // either way, so one entry serves measureText at every size;
                   // all zero for a blank glyph.
    // The canonical capture (color glyphs only; NULL/0 otherwise).  capture_w x
    // capture_h premultiplied RGBA8, top row first, covering the glyph-space rect
    // [ink_x0, ink_x0 + capture_w] x [ink_y0, ink_y0 + capture_h] in capture px -- the
    // ink box's bottom-left pinned to the buffer's bottom-left corner, the way
    // cnvs_glyph_draw + cnvs_glyph_bounds place a live render.
    uint8_t *__counted_by(capture_size) capture;  // owned; the canonical bytes
    int capture_size;       // capture_w * capture_h * 4
    int capture_w, capture_h;  // capture dims (CNVS_CAPTURE_EM square when fetched live)
    cnvs_mip *__counted_by(nmips) mip;  // derived pyramid, largest first, down
    int nmips;                          // to 1x1; built lazily by cnvs_glyph_mip
    uint32_t key;  // font_id << 16 | glyph id
    bool used;
    bool emitted;  // already serialized into the active recording
};

// An interned font (the sized model: length + bytes, no NUL games), keyed by
// (name, weight, style) -- not name alone -- so a SYNTHESIZED bold/italic, which
// reports the regular face's NAME, gets its own id (and so its own glyph slots)
// instead of aliasing the regular face's outlines.  A real bold/italic face has
// a distinct name and would separate on name alone; the weight/style key covers
// the synthesized case uniformly.  Carries the font's vertical metrics
// normalized to size 1.0 -- ascent/descent are linear in size, so one record
// (and one serialized `font` block) serves every font size; the consumer
// multiplies by size_px.
struct cnvs_font_name {
    char *__counted_by(len) name;
    int len;
    int weight;         // CSS 100..900 this id was interned for, part of the key
    bool italic;        // fontStyle italic, part of the key
    float asc1, desc1;  // ascent/descent at size 1.0, positive magnitudes from
    bool has_vm;        // the baseline; valid only once has_vm is set
    bool emitted;       // already serialized into the active recording
};

struct cnvs_text_cache {
    struct cnvs_shaping_slot shaping[CNVS_SHAPING_CACHE_N];
    uint64_t tick;  // monotone use counter feeding the shape slots' last_use
    struct cnvs_glyph_slot *__counted_by(glyph_cap) glyph;  // lazily built on first miss
    int glyph_cap;                                   // 0 until that build succeeds
    int glyph_count;
    struct cnvs_font_name font[CNVS_FONT_INTERN_N];  // interned names; index = font id
    int nfonts;
    // Stats: pure instrumentation (tests + measurement), no behavioural role.
    int shaping_hits, shaping_misses;
    int glyph_hits, glyph_misses;
};

void cnvs_text_cache_init(struct cnvs_text_cache *__single c);   // an empty cache
void cnvs_text_cache_reset(struct cnvs_text_cache *__single c);  // free entries -> empty;
                                                          // also the destructor (the
                                                          // struct owns no spine)

// The shape lookup: the cached line for (name, size_px, rtl, ls, ws, text),
// shaping through the boundary and baking letterSpacing/wordSpacing into the
// advances (cnvs_shaped_apply_spacing) on a miss before caching.  The result is
// BORROWED -- the cache owns it; it stays valid until the next lookup can evict
// it (call sites do one lookup per op and never nest, so a borrow never crosses
// an insert).  NULL only when the boundary itself fails or the key copy can't be
// allocated -- the same degradation as the uncached path.  `name` is the
// requested family and `weight`/`italic` the requested style: they join the
// boundary call AND the cache key (two families, or two weights/styles, of the
// same bytes cache separately).  `c` must be non-NULL (ownership needs
// somewhere to live).
struct cnvs_shaped const *__single cnvs_text_cache_shaping(struct cnvs_text_cache *__single c,
        char const *__counted_by(name_len) name, int name_len, float size_px,
        bool rtl, float ls, float ws, int weight, bool italic,
        int kerning, int rendering, char const *__counted_by(lang_len) lang, int lang_len,
        char const *__counted_by(len) text, int len);

// Intern `font`'s name under (name, weight, style) (one boundary name fetch per
// run, not per glyph) and return its id for glyph keys; -1 when there is no
// cache, the intern table is full, or any allocation fails -- the glyph walk
// then takes plain boundary calls.  `weight`/`italic` are the requested style
// the run was shaped under: they join the intern key so a synthesized bold/italic
// (same resolved name as regular) gets a distinct id.  A fresh intern also
// records the font's vmetrics (cnvs_run_vmetrics).
int cnvs_text_cache_font(struct cnvs_text_cache *__single c, void *__single font,
                         int weight, bool italic);

// Intern a font by (name bytes, weight, style), no boundary call: the replay
// path, and the canvas's own metrics intern.  Same -1 contract as
// cnvs_text_cache_font.
int cnvs_text_cache_intern(struct cnvs_text_cache *__single c,
                           char const *__counted_by(len) name, int len,
                           int weight, bool italic);

// The per-name vertical metrics record (asc1/desc1 in struct cnvs_font_name): set
// keeps the first value it is given (live and replayed values agree, so first
// wins keeps one canonical pair); get returns false until one is set.
void cnvs_text_cache_set_vmetrics(struct cnvs_text_cache *__single c, int fid,
                                  float asc1, float desc1);
bool cnvs_text_cache_get_vmetrics(struct cnvs_text_cache *__single c, int fid,
                                  float *__single asc1, float *__single desc1);

// The glyph lookup behind the outline and metrics walks: the cached canonical
// curves + ink bounds for (fid, glyph).  A miss fetches BOTH from the boundary
// (one curves fetch + one bounds fetch per (font, glyph) ever), normalizes the
// ink box to font units (size_px is the size `font` is built at, the px scale's
// numerator), and inserts.  NULL when the cache can't serve -- no cache, fid <
// 0, no `font` handle to fetch with (a replay-built run whose glyph block was
// missing), a full table, or a failed allocation -- and the caller degrades to
// a plain boundary call (or, with no handle, to a blank glyph).
struct cnvs_glyph_slot *__single cnvs_text_cache_glyph(struct cnvs_text_cache *__single c,
        int fid, void *__single font, uint16_t glyph, float size_px);

// Insert one glyph's canonical data under (fid, glyph) without a boundary
// handle: the replay path.  Takes ownership of the heap arrays `verb` and `pt`
// whether or not it inserts; an existing entry wins (replaying onto a canvas
// that already drew live), and a full table or failed build just leaves the
// entry out -- the cache's usual best-effort degradation, not an error.
void cnvs_text_cache_put_glyph(struct cnvs_text_cache *__single c, int fid,
        uint16_t glyph, enum cnvs_glyph_verb *__counted_by(nverbs) verb, int nverbs,
        cnvs_vec2 *__counted_by(npts) pt, int npts, float upem,
        float ink_x0, float ink_y0, float ink_x1, float ink_y1);

// The color-glyph lookup behind the emoji draw and metrics walks: the cached
// canonical capture for (fid, glyph).  A miss rasterizes through the boundary
// ONCE -- a sized handle at CNVS_CAPTURE_EM px (cnvs_font_resized), the ink box
// (cnvs_glyph_bounds, in capture px), and one cnvs_glyph_draw into the square
// CNVS_CAPTURE_EM buffer -- and inserts; every later draw at every size and
// transform samples those bytes.  A glyph with empty ink caches as capture_w == 0
// (known blank, no capture).  NULL when the cache can't serve -- no cache,
// fid < 0, no `font` handle to rasterize with (a replay-built run whose bitmap
// block was missing), a full table, or a failed allocation -- and the caller
// degrades to a per-draw boundary render (or, with no handle, to a blank
// advance).
struct cnvs_glyph_slot *__single cnvs_text_cache_color(struct cnvs_text_cache *__single c,
        int fid, void *__single font, uint16_t glyph);

// Insert one color glyph's capture under (fid, glyph) without a boundary
// handle: the replay path (the `bitmap` block).  Takes ownership of `px`
// whether or not it inserts; `len` must be w*h*4.  `ink_*` is the capture-px
// ink box (the placement rect's bottom-left is (ink_x0, ink_y0)).  An existing
// entry wins, and a full table or failed build just leaves the entry out --
// the cache's usual best-effort degradation, not an error.
void cnvs_text_cache_put_capture(struct cnvs_text_cache *__single c, int fid,
        uint16_t glyph, uint8_t *__counted_by(len) px, int len, int w, int h,
        float ink_x0, float ink_y0, float ink_x1, float ink_y1);

// One 2x2 box-halving step over premultiplied RGBA8: dst (dw x dh) is the
// ceil-halved src (sw x sh), each dst pixel the rounded mean of its 2x2 src
// block, with the edge row/column replicated when a dimension is odd.  All
// four channels share the rounding, so the premul invariant (r,g,b <= a) is
// preserved exactly.  Requires dw == (sw+1)/2 and dh == (sh+1)/2 (no-op
// otherwise).  Exported for the pyramid tests; cnvs_glyph_mip drives it.
void cnvs_mip_halve(uint8_t const *__counted_by(sw * sh * 4) src, int sw, int sh,
                    uint8_t *__counted_by(dw * dh * 4) dst, int dw, int dh);

// Borrow the pyramid level whose resolution best matches `footprint` (the
// glyph quad's longer edge in device px): the SMALLEST level still >=
// footprint in both dimensions, so bilinear sampling within the level never
// downscales by more than 2x.  Builds the slot's pyramid lazily on first use
// (a failed level allocation keeps the prefix built so far and degrades the
// selection to the coarsest available level -- worst case the capture itself,
// which always exists).  Returns a zero view (px NULL) only when the slot has
// no capture.
cnvs_mip cnvs_glyph_mip(struct cnvs_glyph_slot *__single slot, float footprint);

// The trilinear flavour of cnvs_glyph_mip: borrow the pyramid level PAIR
// around `footprint` -- `fine` covering it within a 2x downscale, `coarse`
// the next level down -- and return the blend toward `coarse`, 0 at fine's
// own size rising to 1 at coarse's.  The pair is
// found by doubling and the blend by exact float arithmetic (draw_image_quad's
// SAMP_TRILINEAR rule -- no log2f, so replay cannot drift across libms).  A
// magnifying or degenerate footprint, the pyramid floor, and a build that
// degraded all collapse the same way: coarse == fine and the blend is 0, so a
// caller can always lerp blindly.  Zero views (px NULL) only when the slot has
// no capture.
float cnvs_glyph_mip_pair(struct cnvs_glyph_slot *__single slot, float footprint,
                          cnvs_mip *__single fine, cnvs_mip *__single coarse);

// Insert a rebuilt shaped line for (name, size_px, rtl, ls, ws, weight, style,
// text): the replay path.  The advances arrive already spacing-baked (the
// recorded `run` lines carry them), so this only keys and inserts -- it does NOT
// re-apply spacing.  `name` is the requested family and `weight`/`italic` the
// requested style, the key's font parts.  Takes ownership of `s` always (freeing
// it when it can't insert); copies the key bytes; replaces an existing entry for
// the same key.
void cnvs_text_cache_put_shaping(struct cnvs_text_cache *__single c,
        char const *__counted_by(name_len) name, int name_len, float size_px,
        bool rtl, float ls, float ws, int weight, bool italic,
        int kerning, int rendering, char const *__counted_by(lang_len) lang, int lang_len,
        char const *__counted_by(len) text, int len,
        struct cnvs_shaped *__single s);

// The recorder's view of a cached line: the slot holding (name, size_px, rtl,
// ls, ws, weight, style, text), or NULL when it isn't cached.  `name` is the
// requested family and `weight`/`italic` the requested style, the key's font
// parts.  A peek -- no stats, no LRU bump, no shaping.
struct cnvs_shaping_slot *__single cnvs_text_cache_shaping_slot(struct cnvs_text_cache *__single c,
        char const *__counted_by(name_len) name, int name_len, float size_px,
        bool rtl, float ls, float ws, int weight, bool italic,
        int kerning, int rendering, char const *__counted_by(lang_len) lang, int lang_len,
        char const *__counted_by(len) text, int len);

// Clear every `emitted` mark (font names, glyphs, shaped lines): a new
// recording starts with nothing serialized yet.
void cnvs_text_cache_unmark(struct cnvs_text_cache *__single c);

// The per-canvas cache handle, for tests and stats (implemented in canvas.c).
// Deliberately not in the public canvas.h: tests include internal headers.
struct canvas;
struct cnvs_text_cache *__single cnvs_canvas_text_cache(struct canvas *__single cv);

// Callback for the color (emoji) glyphs cnvs_shaped_outline meets: they have no
// outline and must be drawn, not traced (the capture path above), so the walk
// hands them to the caller at their pen position in user space.  `fid` is the
// run's interned font-name id (the capture cache key; -1 when the name couldn't
// intern) and `font` its boundary handle (NULL for a replay-built run -- the
// capture cache draws without one).  `ctx` is the caller's own pointer passed
// back verbatim -- checked C on both sides, so the void* hop needs no forge.
// NULL skips color glyphs (they still contribute their advance).
typedef void (*cnvs_color_glyph_fn)(void *__single ctx, int fid,
                                    void *__single font, uint16_t glyph,
                                    float pen_x, float baseline_y);

// Outline a shaped line at pen origin (ox,oy) in user space into `out` (device space,
// mapped by to_device, curves flattened at `tol` px).  Layout -- the pen advance --
// runs checked in the core; each glyph's canonical curves come from `cache` when it
// holds them and from the boundary otherwise (populating the cache; NULL `cache`
// means plain boundary calls throughout).  Returns the advance width.  Color
// (emoji) runs have no outline path: their glyphs go to `color` (or are skipped when
// it is NULL), and contribute only their advance to the layout.
float cnvs_shaped_outline(struct cnvs_text_cache *__single cache,
                          struct cnvs_shaped const *__single s, float ox, float oy,
                          cnvs_mat to_device, float tol, struct cnvs_path *__single out,
                          cnvs_color_glyph_fn color, void *__single ctx);

// Draw one glyph of the opaque `font` into an RGBA8 premultiplied buffer at (x,y) in
// bitmap space (origin bottom-left, y up).  The rendering path for color glyphs
// (emoji), which have no outline.  The checked core owns the __counted_by(w*h*4)
// buffer and hands it over; the boundary wraps it in a CGBitmapContext and draws
// within (w,h) -- a pixel buffer crossing checked->boundary, mirror of the glyph run.
void cnvs_glyph_draw(void *__single font, uint16_t glyph, float x, float y,
                     uint8_t *__counted_by(w * h * 4) px, int w, int h);

// A sized copy of an opaque run-font handle: the same face at `size_px` -- the
// capture-size handle cnvs_text_cache_color rasterizes with.  Retained; release
// with cnvs_font_release.  NULL on failure.  Boundary helper: an opaque handle
// in, an opaque handle out, no bounds to carry.
void *__single cnvs_font_resized(void *__single font, float size_px);
void cnvs_font_release(void *__single font);

// A run font's vertical metrics normalized to size 1.0 (positive magnitudes
// from the baseline; multiply by size_px).  Boundary helper: feeds the interned
// name's vmetrics record so one fetch -- and one serialized `font` block --
// serves every size.
void cnvs_run_vmetrics(void const *__single font, float *__single asc1,
                       float *__single desc1);

// One glyph's tight ink bounding box in font px, baseline-relative, y up (x0,y0 =
// bottom-left, x1,y1 = top-right).  All zero for a blank glyph.  Boundary helper:
// sizes and places the buffer cnvs_glyph_draw fills.
void cnvs_glyph_bounds(void *__single font, uint16_t glyph,
                       float *__single x0, float *__single y0,
                       float *__single x1, float *__single y1);

// A primary font handle: a system typeface at a size/weight/style, for the cheap
// font-wide metrics (ascent/descent) and as the reference font for measureText's
// font-wide box and baselines.  `weight`/`italic` build it through the same
// trait-applying descriptor as cnvs_shape_text.  NULL on failure.  Like
// cnvs_shape_text, the name crosses the boundary counted -- (bytes, len), no NUL
// contract.
struct cnvs_font;
struct cnvs_font *__single cnvs_font(char const *__counted_by(name_len) name,
                                     int name_len, float size_px,
                                     int weight, bool italic);
void cnvs_font_free(struct cnvs_font *__single f);

// Font vertical metrics in user px: ascent and descent, both positive magnitudes
// from the baseline.  Cheap (no glyph walk) -- for textBaseline positioning.
void cnvs_font_vmetrics(struct cnvs_font *__single f, float *__single ascent,
                        float *__single descent);

// Full text metrics, all in user px, baseline-relative and laid out from a pen
// origin at x = 0 (the Canvas measureText() defaults: textAlign start / left,
// textBaseline alphabetic).  Sign conventions match TextMetrics: *_left/_ascent
// are positive going left/up, *_right/_descent positive going right/down.
typedef struct {
    float width;                  // advance width
    float actual_left, actual_right;     // actual glyph-ink bounding box (this text)
    float actual_ascent, actual_descent;
    float font_ascent, font_descent;     // font-wide ascent/descent
    float em_ascent, em_descent;         // em square split by the ascent/descent ratio
    float alphabetic_baseline;           // 0 (the reference baseline)
    float hanging_baseline, ideographic_baseline;
} cnvs_text_metrics;

// Full TextMetrics for a shaped line, fallback-aware and checked-core.  Width
// sums the run advances; the actual (ink) box unions each glyph's tight box
// from the cache entry (populating it through the boundary on a miss, exactly
// like the draw walks: font-unit curves for outline glyphs, the capture-px ink
// box for color glyphs) scaled to size_px -- so a warm or replayed canvas
// measures without a boundary call, and emoji measure the same whether the
// metrics came from a live capture or a replayed bitmap block.  When the cache
// can't serve a glyph, a run that still has its font handle falls back to a
// live cnvs_glyph_bounds; a handle-less run's glyph contributes no ink.  The
// font-wide metrics (ascent/descent/em/baselines) derive from ascent_px/
// descent_px -- the primary font's vertical metrics at size_px, which canvas.c
// reads through its cached vmetrics record -- and size_px.  s may be NULL
// (font-wide metrics only).
void cnvs_shaped_metrics(struct cnvs_text_cache *__single cache,
                         struct cnvs_shaped const *__single s, float size_px,
                         float ascent_px, float descent_px,
                         cnvs_text_metrics *__single out);
