// fontFamily as canvas state (font-family project, phase F1).  Five checks:
//   - two families give different measureText widths (and/or shaped runs) for
//     the same text -- the family really threads through shaping;
//   - a bogus family does not crash, renders ink, and the cache holds a resolved
//     font name (the Core Text cascade fired -- the Libian model);
//   - set_font_family then reset returns to the default "Libian TC";
//   - a NULL / empty name is ignored (the current family is kept);
//   - an in-memory (file under build/) record -> replay round trip with a
//     non-default family reproduces the surface byte for byte (follows
//     tests/test_record.c).

#include "canvas2d.h"
#include "canvas2d_text.h"  // the cache handle: resolved font names + stats

#include "test_util.h"

#include <math.h>
#include <ptrcheck.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define W 96
#define H 48
#define NPX (W * H * 4)

// The suite forbids a bare == on floats (-Wfloat-equal): spell exact equality as
// a zero absolute difference, and "differ" as a difference above a perceptible
// (sub-pixel) threshold.
static bool exact(float a, float b) {
    return fabsf(a - b) <= 0.0f;
}
static bool differ(float a, float b) {
    return fabsf(a - b) > 0.25f;
}

// Any non-transparent byte: the draw produced ink.
static bool drew_anything(uint8_t const *__counted_by(n) px, int n) {
    for (int i = 0; i < n; i++) {
        if (px[i] != 0) {
            return true;
        }
    }
    return false;
}

// Two distinct families measure (or shape) the same text differently.
static void test_distinct_families(void) {
    struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas2d_set_font_size(cv, 24.0f);

    char const *__null_terminated text = "Typeface";
    canvas2d_set_font_family(cv, "Helvetica");
    float const w_helv = canvas2d_measure_text(cv, text);
    canvas2d_set_font_family(cv, "Georgia");
    float const w_geo = canvas2d_measure_text(cv, text);
    canvas2d_set_font_family(cv, "Menlo");  // monospace: a third distinct metric
    float const w_mono = canvas2d_measure_text(cv, text);

    CHECK(w_helv > 0.0f && w_geo > 0.0f && w_mono > 0.0f);
    // The three proportional/monospace faces lay the same string out at
    // different advances; at least one pair must differ (no aliasing).
    CHECK(differ(w_helv, w_geo) || differ(w_helv, w_mono) || differ(w_geo, w_mono));

    // The shaping cache kept the families in distinct slots: measuring the first
    // again is a hit, not a re-shape that would mean they aliased.
    struct canvas2d_text_cache *__single c = canvas2d_canvas_text_cache(cv);
    int const miss_before = c->shaping_misses;
    canvas2d_set_font_family(cv, "Helvetica");
    (void)canvas2d_measure_text(cv, text);  // already shaped above: a hit
    CHECK(c->shaping_misses == miss_before);

    canvas2d_free(cv);
}

// A bogus family does not crash, renders ink, and resolves to a real font (the
// cache interns the resolved name, never the bogus request).
static void test_bogus_family(void) {
    struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
    // sizeof-1 keeps the literal an array (a bounded, indexable buffer), so the
    // name reuse below stays in the indexable world.
    static char const bogus[] = "NoSuchFamily ZZQ 12345";
    int const blen = (int)sizeof bogus - 1;
    canvas2d_set_font_family_n(cv, bogus, blen);
    canvas2d_set_font_size(cv, 28.0f);

    float const w = canvas2d_measure_text(cv, "Hi");
    CHECK(w > 0.0f);  // the fallback cascade produced a measurable line

    canvas2d_fill_text(cv, "Hi", 4.0f, 36.0f);

    uint8_t *__counted_by(NPX) px = malloc(NPX);
    CHECK(px != NULL);
    if (px) {
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, NPX);
        CHECK(drew_anything(px, NPX));  // something was painted
        free(px);
    }

    // The interned font names are the RESOLVED ones; the cache holds at least
    // one and none of them is the bogus request.
    struct canvas2d_text_cache *__single c = canvas2d_canvas_text_cache(cv);
    CHECK(c->nfonts > 0);
    for (int i = 0; i < c->nfonts; i++) {
        bool const same = c->font[i].len == blen &&
                          memcmp(c->font[i].name, bogus, (size_t)blen) == 0;
        CHECK(!same);  // the bogus name never becomes a resolved run's name
    }

    canvas2d_free(cv);
}

// reset() restores the default family; a non-default family before reset gives a
// different width than after.
static void test_reset_restores_default(void) {
    struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas2d_set_font_size(cv, 24.0f);
    char const *__null_terminated text = "Typeface";

    float const w_default = canvas2d_measure_text(cv, text);  // Libian TC
    canvas2d_set_font_family(cv, "Menlo");
    float const w_menlo = canvas2d_measure_text(cv, text);
    CHECK(differ(w_default, w_menlo));  // the family changed the measurement

    canvas2d_reset(cv);
    canvas2d_set_font_size(cv, 24.0f);  // reset also cleared the size
    float const w_after = canvas2d_measure_text(cv, text);
    CHECK(exact(w_after, w_default));  // back on the default family

    canvas2d_free(cv);
}

// NULL and "" are ignored: the family stays whatever it was.
static void test_null_empty_ignored(void) {
    struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas2d_set_font_size(cv, 24.0f);
    char const *__null_terminated text = "Typeface";

    canvas2d_set_font_family(cv, "Menlo");
    float const w_menlo = canvas2d_measure_text(cv, text);

    canvas2d_set_font_family(cv, NULL);  // ignored
    CHECK(exact(canvas2d_measure_text(cv, text), w_menlo));
    canvas2d_set_font_family(cv, "");    // ignored
    CHECK(exact(canvas2d_measure_text(cv, text), w_menlo));

    canvas2d_free(cv);
}

// A text scene drawn under a non-default family.
static void draw_scene(struct canvas2d_context *__single cv) {
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.95f, 0.95f, 0.9f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.1f, 0.1f, 0.4f, 1.0f);
    canvas2d_set_font_family(cv, "Georgia");
    canvas2d_set_font_size(cv, 20.0f);
    canvas2d_fill_text(cv, "Hi 字", 4.0f, 32.0f);
}

// Record a non-default-family scene to a file, then replay it onto a fresh
// canvas: the surfaces match byte for byte (the recorded blocks carry the
// resolved glyphs; the set_font_family op and the shaping family token reproduce
// the cache key).
static void test_record_replay_roundtrip(void) {
    char const *__null_terminated path = "build/test_fontfamily_rt.canvas";

    uint8_t recorded_px[NPX];
    {
        struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas2d_record_to(cv, path));
        draw_scene(cv);
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, recorded_px, (int)sizeof recorded_px);
        canvas2d_free(cv);  // flush + close
    }
    CHECK(drew_anything(recorded_px, NPX));

    {
        struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas2d_replay_from(cv, path));
        uint8_t replayed_px[NPX];
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, replayed_px, (int)sizeof replayed_px);
        CHECK(memcmp(recorded_px, replayed_px, sizeof recorded_px) == 0);
        canvas2d_free(cv);
    }
}

int main(void) {
    test_distinct_families();
    test_bogus_family();
    test_reset_restores_default();
    test_null_empty_ignored();
    test_record_replay_roundtrip();
    return TEST_REPORT();
}
