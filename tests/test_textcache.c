// The params -> derived-data text lookup (struct cnvs_text_cache in src/cnvs_text.h):
// a per-canvas memo of Core Text boundary results, checked before the boundary
// is called.  Pinned here: transparency (a warm cache renders byte-identical to
// a cold one), the measure-then-draw hit pattern, key correctness (size bits +
// paragraph direction + text bytes), LRU eviction at the slot bound, the
// glyph-curve map's one fetch per (font, glyph), font-name interning across
// fallback, and reset() clearing back to cold.  Stats come via the internal
// cnvs_canvas_text_cache handle -- they are instrumentation, not public API.

#include "canvas.h"
#include "cnvs_text.h"
#include "test_util.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum { W = 96, H = 48, LEN = W * H * 4 };

// A small text scene mixing outline glyphs (with a repeat and a space) and a
// color-emoji fallback run.
static void draw_scene(struct canvas *__single cv) {
    canvas_set_font_size(cv, 18.0f);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.10f, 0.20f, 0.30f, 1.0f);
    canvas_fill_text(cv, "Waffle fan", 4.0f, 22.0f);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.80f, 0.30f, 0.20f, 1.0f);
    canvas_fill_text(cv, "A\xF0\x9F\x98\x80g", 4.0f, 44.0f);  // A 😀 g
}

// Transparency: render the scene cold on one canvas and warm (drawn, wiped,
// drawn again) on another -- the readback bytes must match exactly, and the
// stats must prove the second draw never went back to the boundary.
static void check_transparent(void) {
    struct canvas *__single cold = canvas(W, H);
    struct canvas *__single warm = canvas(W, H);
    CHECK(cold != NULL && warm != NULL);
    if (!cold || !warm) {
        canvas_free(cold);
        canvas_free(warm);
        return;
    }
    draw_scene(cold);

    draw_scene(warm);  // populate the caches
    canvas_clear_rect(warm, 0.0f, 0.0f, (float)W, (float)H);
    struct cnvs_text_cache *__single c = cnvs_canvas_text_cache(warm);
    int smiss = c->shaping_misses, gmiss = c->glyph_misses;
    draw_scene(warm);  // every lookup must hit
    CHECK(c->shaping_misses == smiss);
    CHECK(c->glyph_misses == gmiss);
    CHECK(c->shaping_hits >= 2);
    CHECK(c->glyph_hits > 0);

    uint8_t a[LEN], b[LEN];
    canvas_get_image_data(cold, CANVAS_CS_SRGB, 0, 0, W, H, a, LEN);
    canvas_get_image_data(warm, CANVAS_CS_SRGB, 0, 0, W, H, b, LEN);
    CHECK(memcmp(a, b, LEN) == 0);

    canvas_free(cold);
    canvas_free(warm);
}

// The measure-then-draw pattern real callers use, plus key correctness: a
// different size or different bytes is a different key; the originals stay hot.
static void check_keys(void) {
    struct canvas *__single cv = canvas(W, H);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    struct cnvs_text_cache *__single c = cnvs_canvas_text_cache(cv);
    canvas_set_font_size(cv, 20.0f);

    canvas_fill_text(cv, "kerning", 4.0f, 30.0f);
    CHECK(c->shaping_misses == 1 && c->shaping_hits == 0);
    CHECK(canvas_measure_text(cv, "kerning") > 0.0f);  // measure after draw: hit
    CHECK(c->shaping_misses == 1 && c->shaping_hits == 1);
    (void)canvas_measure_text_full(cv, "kerning");     // the full metrics too
    CHECK(c->shaping_misses == 1 && c->shaping_hits == 2);

    canvas_set_font_size(cv, 21.0f);                   // same bytes, other size
    (void)canvas_measure_text(cv, "kerning");
    CHECK(c->shaping_misses == 2 && c->shaping_hits == 2);

    canvas_set_font_size(cv, 20.0f);                   // same size, other bytes
    (void)canvas_measure_text(cv, "kerninG");
    CHECK(c->shaping_misses == 3 && c->shaping_hits == 2);

    (void)canvas_measure_text(cv, "kerning");          // the original is still hot
    CHECK(c->shaping_misses == 3 && c->shaping_hits == 3);

    // Same size, same bytes, other paragraph direction: a different key (the
    // same text shapes differently under ltr and rtl, so aliasing the two
    // would replay one as the other).  Both stay hot side by side.
    canvas_set_direction(cv, CANVAS_DIRECTION_RTL);
    (void)canvas_measure_text(cv, "kerning");
    CHECK(c->shaping_misses == 4 && c->shaping_hits == 3);
    (void)canvas_measure_text(cv, "kerning");          // the rtl line is hot...
    CHECK(c->shaping_misses == 4 && c->shaping_hits == 4);
    canvas_set_direction(cv, CANVAS_DIRECTION_LTR);
    (void)canvas_measure_text(cv, "kerning");          // ...and the ltr one kept
    CHECK(c->shaping_misses == 4 && c->shaping_hits == 5);

    canvas_free(cv);
}

// Eviction: one entry past the bound pushes out the least-recently-used line.
// The evicted string re-shapes (a miss) to the same metrics, the freshest one
// still hits, and a post-churn draw matches a cold canvas pixel for pixel.
// The churn keys are runtime-built bytes, so they go through the cache's own
// counted seam (the same lookup canvas_measure_text lands on, same stats, same
// LRU stamping) -- the __null_terminated public entry would demand an unsafe
// indexable->NUL bridge for a buffer the type system can't see terminated.
static char const k_family[] = "Libian TC";  // the canvas's pinned family;
                                             // joins the boundary call, not the key

static void check_eviction(void) {
    struct canvas *__single churn = canvas(W, H);
    struct canvas *__single fresh = canvas(W, H);
    CHECK(churn != NULL && fresh != NULL);
    if (!churn || !fresh) {
        canvas_free(churn);
        canvas_free(fresh);
        return;
    }
    struct cnvs_text_cache *__single c = cnvs_canvas_text_cache(churn);
    canvas_set_font_size(churn, 16.0f);

    float const w0 = canvas_measure_text(churn, "s0");
    char last[3] = { 0 };
    for (int i = 1; i <= CNVS_SHAPING_CACHE_N; i++) {  // fill every slot, plus one
        last[0] = 's';                               // distinct two-letter keys
        last[1] = (char)('a' + i / 26);
        last[2] = (char)('a' + i % 26);
        (void)cnvs_text_cache_shaping(c, k_family, (int)sizeof k_family - 1,
                                    16.0f, false, last, (int)sizeof last);
    }
    CHECK(c->shaping_misses == CNVS_SHAPING_CACHE_N + 1);

    int const hits = c->shaping_hits;
    // Evicted: re-shaped from the boundary, to the bit-identical width.
    CHECK(fabsf(canvas_measure_text(churn, "s0") - w0) <= 0.0f);
    CHECK(c->shaping_misses == CNVS_SHAPING_CACHE_N + 2);
    CHECK(c->shaping_hits == hits);
    (void)cnvs_text_cache_shaping(c, k_family, (int)sizeof k_family - 1,
                                16.0f, false, last, (int)sizeof last);
    CHECK(c->shaping_hits == hits + 1);  // the newest entry survived

    canvas_set_fill_rgba(churn, CANVAS_CS_SRGB, 0.2f, 0.2f, 0.7f, 1.0f);
    canvas_fill_text(churn, "s0", 4.0f, 30.0f);
    canvas_set_font_size(fresh, 16.0f);
    canvas_set_fill_rgba(fresh, CANVAS_CS_SRGB, 0.2f, 0.2f, 0.7f, 1.0f);
    canvas_fill_text(fresh, "s0", 4.0f, 30.0f);
    uint8_t a[LEN], b[LEN];
    canvas_get_image_data(churn, CANVAS_CS_SRGB, 0, 0, W, H, a, LEN);
    canvas_get_image_data(fresh, CANVAS_CS_SRGB, 0, 0, W, H, b, LEN);
    CHECK(memcmp(a, b, LEN) == 0);

    canvas_free(churn);
    canvas_free(fresh);
}

// The glyph-curve map: a repeated glyph is fetched once per (font, glyph), a
// blank (the space) caches as "no outline", and a warm redraw adds no misses.
static void check_glyph_once(void) {
    struct canvas *__single cv = canvas(W, H);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    struct cnvs_text_cache *__single c = cnvs_canvas_text_cache(cv);
    canvas_set_font_size(cv, 20.0f);

    canvas_fill_text(cv, "AA AA", 2.0f, 30.0f);  // two distinct glyphs: 'A', ' '
    CHECK(c->glyph_misses == 2);
    CHECK(c->glyph_hits == 3);
    CHECK(c->glyph_count == 2);
    CHECK(c->nfonts == 1);

    canvas_fill_text(cv, "AA AA", 2.0f, 30.0f);  // warm: no new fetches
    CHECK(c->glyph_misses == 2);
    CHECK(c->glyph_hits == 8);

    canvas_free(cv);
}

// Fallback interning: Hebrew falls back from the pinned (CJK) face to another
// outline font, so two names intern and their glyph keys live side by side --
// a second draw hits every one of them (no key collisions across fonts).
static void check_fallback_fonts(void) {
    struct canvas *__single cv = canvas(W, H);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    struct cnvs_text_cache *__single c = cnvs_canvas_text_cache(cv);
    canvas_set_font_size(cv, 20.0f);

    canvas_fill_text(cv, "A\xD7\x90", 2.0f, 30.0f);  // A + aleph
    CHECK(c->nfonts >= 2);
    CHECK(c->glyph_count == c->glyph_misses);  // every miss inserted exactly once
    int const gmiss = c->glyph_misses;
    canvas_fill_text(cv, "A\xD7\x90", 2.0f, 30.0f);
    CHECK(c->glyph_misses == gmiss);  // both fonts' glyphs hit on the redraw

    canvas_free(cv);
}

// reset(): the cache goes back to its initial (empty) state -- documented with
// the reset contract in canvas.c -- and a post-reset draw is cold but correct.
static void check_reset(void) {
    struct canvas *__single cv = canvas(W, H);
    struct canvas *__single fresh = canvas(W, H);
    CHECK(cv != NULL && fresh != NULL);
    if (!cv || !fresh) {
        canvas_free(cv);
        canvas_free(fresh);
        return;
    }
    struct cnvs_text_cache *__single c = cnvs_canvas_text_cache(cv);
    canvas_set_font_size(cv, 18.0f);
    canvas_fill_text(cv, "Reset", 4.0f, 30.0f);
    CHECK(c->shaping_misses > 0 && c->glyph_count > 0 && c->nfonts > 0);

    canvas_reset(cv);
    CHECK(c->shaping_misses == 0 && c->shaping_hits == 0);
    CHECK(c->glyph_misses == 0 && c->glyph_hits == 0);
    CHECK(c->glyph_count == 0 && c->glyph_cap == 0 && c->nfonts == 0);

    canvas_set_font_size(cv, 18.0f);  // reset dropped the 18px state too
    canvas_fill_text(cv, "Reset", 4.0f, 30.0f);
    CHECK(c->shaping_misses == 1);      // cold again

    canvas_set_font_size(fresh, 18.0f);
    canvas_fill_text(fresh, "Reset", 4.0f, 30.0f);
    uint8_t a[LEN], b[LEN];
    canvas_get_image_data(cv, CANVAS_CS_SRGB, 0, 0, W, H, a, LEN);
    canvas_get_image_data(fresh, CANVAS_CS_SRGB, 0, 0, W, H, b, LEN);
    CHECK(memcmp(a, b, LEN) == 0);

    canvas_free(cv);
    canvas_free(fresh);
}

int main(void) {
    check_transparent();
    check_keys();
    check_eviction();
    check_glyph_once();
    check_fallback_fonts();
    check_reset();
    return TEST_REPORT();
}
