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
typedef struct {
    uint16_t *__counted_by(count) glyph;    // glyph ids
    float *__counted_by(count) xadv;         // x advance per glyph, user px
    int32_t *__counted_by(count) cluster;    // source UTF-16 index per glyph (logical)
    int count;
    bool rtl;
    void *__single font;   // opaque CTFontRef for this run (font fallback), retained
} cnvs_glyph_run;

typedef struct {
    cnvs_glyph_run *__counted_by(nruns) run;
    int nruns;
    int text_len;   // source UTF-16 length, the bound for cluster indices
    float size_px;  // font size the line was shaped at, user px (every run's font,
                    // fallback included, is at this size; the numerator of the
                    // canonical-curve px scale)
} cnvs_shaped;

// Shape UTF-8 `text` with font `name` at `size_px`.  Runs come back in visual order.
// NULL on failure.  Implemented in the unsafe boundary TU.
cnvs_shaped *__single cnvs_shape(char const *__null_terminated name, float size_px,
                                 char const *__null_terminated text);
void cnvs_shaped_free(cnvs_shaped *__single s);

// Checked-core consumers.
float cnvs_shaped_width(cnvs_shaped const *__single s);                // sum of advances
int cnvs_shaped_index_at_x(cnvs_shaped const *__single s, float x);    // hit-test -> UTF-16 index, or -1

// A visual x range [x0,x1) in user px from the line start (a selection highlight rect).
typedef struct {
    float x0, x1;
} cnvs_xspan;

// Caret: visual x for a logical UTF-16 index (the leading visual edge of that glyph).
float cnvs_shaped_x_at_index(cnvs_shaped const *__single s, int index);

// Selection: visual x-spans covering the logical range [lo,hi).  A bidi range maps to
// non-contiguous visual positions and so splits into multiple spans; writes up to
// `max`, returns the count.  Pure index logic: the cluster map drives it and every
// access is bounds-checked, no forge -- this is where the checked side earns its keep.
int cnvs_shaped_selection(cnvs_shaped const *__single s, int lo, int hi,
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
typedef enum : uint8_t {
    CNVS_GLYPH_MOVE,   // 1 point
    CNVS_GLYPH_LINE,   // 1 point
    CNVS_GLYPH_QUAD,   // 2 points: control, end
    CNVS_GLYPH_CUBIC,  // 3 points: control 1, control 2, end
    CNVS_GLYPH_CLOSE,  // no points
} cnvs_glyph_verb;

// Fetch one glyph's canonical curves.  The checked caller owns the buffers and the
// boundary fills within their caps (the cnvs_glyph_draw hand-off, twice over); the
// true counts come back in *nverbs/*npts and may exceed the caps, in which case the
// caller grows its buffers and calls again.  *units_per_em is the font's design-grid
// resolution, the denominator of the px scale.  A blank or color (emoji) glyph has
// no outline: both counts come back 0.  Boundary helper.
void cnvs_glyph_curves(void *__single font, uint16_t glyph,
                       cnvs_glyph_verb *__counted_by(vcap) verb, int vcap,
                       cnvs_vec2 *__counted_by(pcap) pt, int pcap,
                       int *__single nverbs, int *__single npts,
                       float *__single units_per_em);

// ---------------------------------------------------------------------------
// The params -> derived-data lookup: a per-canvas memo of boundary results,
// consulted BEFORE Core Text is called, populated live from what the boundary
// hands back.  Two maps:
//
//   - shaped lines, keyed by (size_px bits, text bytes) -> the cnvs_shaped.
//     The cache OWNS its entries (each run's retained CTFontRef stays alive
//     with them); call sites borrow.  Fixed CNVS_SHAPE_CACHE_N slots, evicted
//     least-recently-used: LRU keeps the measure-then-draw pair and a frame's
//     repeated labels hot, and a 64-entry linear scan is cheaper than any
//     structure clever enough to beat it.
//   - glyph curves, keyed by (font name, glyph id) -> the canonical font-unit
//     verbs/points + units_per_em from cnvs_glyph_curves.  Canonical bytes are
//     size- and transform-independent, so one entry serves every draw.  Font
//     identity is the interned font NAME (stable across processes -- what the
//     upcoming serialized form of this lookup needs), not the CTFontRef
//     pointer.  Open-addressed, fixed CNVS_GLYPH_TABLE_N slots; inserts simply
//     stop at CNVS_GLYPH_CACHE_N entries (no eviction: a glyph's curves never
//     go stale, and a scene with >1k distinct glyphs just degrades the rest
//     to boundary calls).  Blank glyphs cache as empty entries, so a space
//     costs one boundary call ever.
//
// The cache is transparent: a hit and a miss render identically, and any
// cache-side allocation failure degrades that lookup to a plain boundary call
// rather than failing the op.  See docs/text-boundary.md.

enum {
    CNVS_SHAPE_CACHE_N = 64,    // shaped-line slots (LRU eviction)
    CNVS_GLYPH_CACHE_N = 1024,  // max cached glyphs (inserts stop, no eviction)
    CNVS_GLYPH_TABLE_N = 2048,  // open-addressing slots: 2x entries, power of two
    CNVS_FONT_INTERN_N = 16,    // distinct font names (primary + fallbacks)
};

// One cached shaped line.  Empty when s == NULL.
typedef struct {
    char *__counted_by(len) text;  // owned key copy of the source bytes
    int len;
    uint32_t size_bits;  // size_px's float bits: exact-bits keying, no epsilon
    uint64_t stamp;      // last-use tick, the LRU ordering
    cnvs_shaped *__single s;  // owned; freed on eviction/clear
} cnvs_shape_slot;

// One cached glyph outline.  A blank or color glyph caches as upem == 0 with no
// curves -- "known to have no outline" is itself a boundary result worth keeping.
typedef struct {
    cnvs_glyph_verb *__counted_by(nverbs) verb;  // owned canonical curves
    cnvs_vec2 *__counted_by(npts) pt;
    int nverbs, npts;
    float upem;    // units_per_em; 0 for a glyph with no outline
    uint32_t key;  // font_id << 16 | glyph id
    bool used;
} cnvs_glyph_slot;

// An interned font name (the sized model: length + bytes, no NUL games).
typedef struct {
    char *__counted_by(len) name;
    int len;
} cnvs_font_name;

typedef struct {
    cnvs_shape_slot shape[CNVS_SHAPE_CACHE_N];
    uint64_t tick;  // monotone use counter feeding the shape slots' LRU stamps
    cnvs_glyph_slot *__counted_by(glyph_cap) glyph;  // lazily built on first miss
    int glyph_cap;                                   // 0 until that build succeeds
    int glyph_count;
    cnvs_font_name font[CNVS_FONT_INTERN_N];  // interned names; index = font id
    int nfonts;
    // Stats: pure instrumentation (tests + measurement), no behavioural role.
    int shape_hits, shape_misses;
    int glyph_hits, glyph_misses;
} cnvs_text_cache;

void cnvs_text_cache_init(cnvs_text_cache *__single c);   // an empty cache
void cnvs_text_cache_clear(cnvs_text_cache *__single c);  // free entries -> empty;
                                                          // also the destructor (the
                                                          // struct owns no spine)

// The shape lookup: the cached line for (size_px, text), shaping through the
// boundary and caching on a miss.  The result is BORROWED -- the cache owns it;
// it stays valid until the next lookup can evict it (call sites do one lookup
// per op and never nest, so a borrow never crosses an insert).  NULL only when
// the boundary itself fails or the key copy can't be allocated -- the same
// degradation as the uncached path.  `name` joins the boundary call but NOT the
// key: the project pins one family ("Libian TC"); a font-family feature must
// add it to the key.  `c` must be non-NULL (ownership needs somewhere to live).
cnvs_shaped const *__single cnvs_text_cache_shape(cnvs_text_cache *__single c,
        char const *__null_terminated name, float size_px,
        char const *__counted_by(len) text, int len);

// Intern `font`'s name (one boundary name fetch per run, not per glyph) and
// return its id for glyph keys; -1 when there is no cache, the intern table is
// full, or any allocation fails -- the glyph walk then takes plain boundary
// calls.
int cnvs_text_cache_font(cnvs_text_cache *__single c, void *__single font);

// The per-canvas cache handle, for tests and stats (implemented in canvas.c).
// Deliberately not in the public canvas.h: tests include internal headers.
struct canvas;
cnvs_text_cache *__single cnvs_canvas_text_cache(struct canvas *__single cv);

// Callback for the color (emoji) glyphs cnvs_shaped_outline meets: they have no
// outline and must be drawn, not traced (see cnvs_glyph_draw), so the walk hands
// them to the caller at their pen position in user space.  `ctx` is the caller's
// own pointer passed back verbatim -- checked C on both sides, so the void* hop
// needs no forge.  NULL skips color glyphs (they still contribute their advance).
typedef void (*cnvs_color_glyph_fn)(void *__single ctx, void *__single font,
                                    uint16_t glyph, float pen_x, float baseline_y);

// Outline a shaped line at pen origin (ox,oy) in user space into `out` (device space,
// mapped by to_device, curves flattened at `tol` px).  Layout -- the pen advance --
// runs checked in the core; each glyph's canonical curves come from `cache` when it
// holds them and from the boundary otherwise (populating the cache; NULL `cache`
// means plain boundary calls throughout).  Returns the advance width.  Color
// (emoji) runs have no outline path: their glyphs go to `color` (or are skipped when
// it is NULL), and contribute only their advance to the layout.
float cnvs_shaped_outline(cnvs_text_cache *__single cache,
                          cnvs_shaped const *__single s, float ox, float oy,
                          cnvs_mat to_device, float tol, cnvs_path *__single out,
                          cnvs_color_glyph_fn color, void *__single ctx);

// Outline one glyph at pen origin (ox,oy) in user space into `out` (device space,
// mapped by to_device, curves flattened at `tol` px).  Checked core: fetches the
// glyph's canonical font-unit curves from the boundary, scales them to user px at
// `size_px`, and runs the transform and the adaptive flattening every other path
// takes -- in checked C, not in the shim.
void cnvs_glyph_outline(void *__single font, uint16_t glyph, float size_px,
                        float ox, float oy, cnvs_mat to_device, float tol,
                        cnvs_path *__single out);

// Draw one glyph of the opaque `font` into an RGBA8 premultiplied buffer at (x,y) in
// bitmap space (origin bottom-left, y up).  The rendering path for color glyphs
// (emoji), which have no outline.  The checked core owns the __counted_by(w*h*4)
// buffer and hands it over; the boundary wraps it in a CGBitmapContext and draws
// within (w,h) -- a pixel buffer crossing checked->boundary, mirror of the glyph run.
void cnvs_glyph_draw(void *__single font, uint16_t glyph, float x, float y,
                     uint8_t *__counted_by(w * h * 4) px, int w, int h);

// Whether `font` is a color (bitmap-glyph) font, e.g. the AppleColorEmoji a mixed
// string falls back to.  Its glyphs have no outline and must be drawn with
// cnvs_glyph_draw rather than cnvs_glyph_outline.  Boundary helper.
bool cnvs_run_is_color(void const *__single font);

// One glyph's tight ink bounding box in font px, baseline-relative, y up (x0,y0 =
// bottom-left, x1,y1 = top-right).  All zero for a blank glyph.  Boundary helper:
// sizes and places the buffer cnvs_glyph_draw fills.
void cnvs_glyph_bounds(void *__single font, uint16_t glyph,
                       float *__single x0, float *__single y0,
                       float *__single x1, float *__single y1);

// A primary font handle: a system typeface at a size, for the cheap font-wide
// metrics (ascent/descent) and as the reference font for measureText's font-wide
// box and baselines.  NULL on failure.
typedef struct cnvs_font cnvs_font;
cnvs_font *__single cnvs_font_create(char const *__null_terminated name, float size_px);
void cnvs_font_destroy(cnvs_font *__single f);

// Font vertical metrics in user px: ascent and descent, both positive magnitudes
// from the baseline.  Cheap (no glyph walk) -- for textBaseline positioning.
void cnvs_font_vmetrics(cnvs_font *__single f, float *__single ascent,
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

// Full TextMetrics for a shaped line, fallback-aware: width and the actual (ink)
// box come from the shaped runs (each glyph measured in its own fallback font); the
// font-wide metrics (ascent/descent/em/baselines) come from `primary`.  Boundary
// helper -- it reads the run glyphs the shaper handed back.
void cnvs_shaped_metrics(cnvs_shaped const *__single s, cnvs_font *__single primary,
                         cnvs_text_metrics *__single out);
