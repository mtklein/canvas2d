// fontVariantCaps / fontStretch as canvas state (font-family project, phase
// F3b).  fontVariantCaps feeds Core Text's small-cap features (smcp / c2sc), so
// it substitutes small-cap glyphs within the resolved face; fontStretch feeds
// the width trait, so it resolves a real narrower/wider face of the family.
// Checks:
//   - fontVariantCaps SMALL_CAPS vs NORMAL on an smcp face (Baskerville):
//     rendered pixels differ (lowercase -> small caps); a no-op on a face
//     without the feature (Menlo): identical;
//   - fontStretch CONDENSED vs NORMAL on a width family (Futura, with a real
//     Futura-CondensedMedium face): the measured width differs (the condensed
//     face is narrower); a no-op on a family with no width face (Menlo):
//     identical;
//   - an out-of-range variantCaps/stretch enum is ignored (keep current);
//   - reset() restores NORMAL / NORMAL;
//   - an in-memory record -> replay round trip with non-default
//     variantCaps + stretch reproduces the surface byte for byte.
//
// Han-unification-style font dependence does not apply here: the effects come
// from OpenType features (smcp) and real width faces that ship with macOS, the
// gallery's record machine.

#include "canvas.h"

#include "test_util.h"

#include <math.h>
#include <ptrcheck.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define W 320
#define H 64
#define NPX (W * H * 4)

// -Wfloat-equal: spell exact equality as a zero absolute difference, "differ" as
// a sub-pixel-or-greater difference.
static bool exact(float a, float b) {
    return fabsf(a - b) <= 0.0f;
}
static bool differ(float a, float b) {
    return fabsf(a - b) > 0.25f;
}

static bool drew_anything(uint8_t const *__counted_by(n) px, int n) {
    for (int i = 0; i < n; i++) {
        if (px[i] != 0) {
            return true;
        }
    }
    return false;
}

// Draw `text` once under the current state into a fresh read-back buffer.
static void render_text(struct canvas *__single cv, char const *__null_terminated text,
                        uint8_t *__counted_by(NPX) out) {
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.0f, 0.0f, 0.0f, 0.0f);
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_text(cv, text, 4.0f, 44.0f);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, out, NPX);
}

// fontVariantCaps SMALL_CAPS substitutes small-cap glyphs on an smcp face, so the
// rendered pixels differ from NORMAL.  ALL_SMALL_CAPS (smcp + c2sc) is a peer
// that at minimum keeps the small-cap substitution; Baskerville ships smcp.
static void test_variant_caps_substitutes(void) {
    struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas_set_font_family(cv, "Baskerville");
    canvas_set_font_size(cv, 34.0f);
    char const *__null_terminated text = "Chapter";

    uint8_t *__counted_by(NPX) normal = malloc(NPX);
    uint8_t *__counted_by(NPX) small = malloc(NPX);
    CHECK(normal != NULL && small != NULL);
    if (normal && small) {
        canvas_set_font_variant_caps(cv, CANVAS_FONT_VARIANT_CAPS_NORMAL);
        render_text(cv, text, normal);
        canvas_set_font_variant_caps(cv, CANVAS_FONT_VARIANT_CAPS_SMALL_CAPS);
        render_text(cv, text, small);
        CHECK(drew_anything(normal, NPX));
        CHECK(memcmp(normal, small, NPX) != 0);  // lowercase -> small caps
        free(normal);
        free(small);
    }
    canvas_free(cv);
}

// On a face without smcp (Menlo, a monospace with no small-cap feature),
// SMALL_CAPS and ALL_SMALL_CAPS are no-ops: the surface and the width match
// NORMAL exactly.
static void test_variant_caps_noop(void) {
    struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas_set_font_family(cv, "Menlo");
    canvas_set_font_size(cv, 34.0f);
    char const *__null_terminated text = "Chapter";

    uint8_t *__counted_by(NPX) normal = malloc(NPX);
    uint8_t *__counted_by(NPX) small = malloc(NPX);
    CHECK(normal != NULL && small != NULL);
    if (normal && small) {
        canvas_set_font_variant_caps(cv, CANVAS_FONT_VARIANT_CAPS_NORMAL);
        render_text(cv, text, normal);
        float const w_normal = canvas_measure_text(cv, text);
        canvas_set_font_variant_caps(cv, CANVAS_FONT_VARIANT_CAPS_SMALL_CAPS);
        render_text(cv, text, small);
        CHECK(drew_anything(normal, NPX));
        CHECK(memcmp(normal, small, NPX) == 0);  // no smcp feature: identical
        CHECK(exact(canvas_measure_text(cv, text), w_normal));
        canvas_set_font_variant_caps(cv, CANVAS_FONT_VARIANT_CAPS_ALL_SMALL_CAPS);
        CHECK(exact(canvas_measure_text(cv, text), w_normal));
        free(normal);
        free(small);
    }
    canvas_free(cv);
}

// fontStretch CONDENSED resolves a real narrower face (Futura-CondensedMedium),
// so the measured width differs from -- and is less than -- NORMAL.
static void test_stretch_condenses(void) {
    struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas_set_font_family(cv, "Futura");
    canvas_set_font_size(cv, 32.0f);
    char const *__null_terminated text = "Magazine";

    canvas_set_font_stretch(cv, CANVAS_FONT_STRETCH_NORMAL);
    float const w_normal = canvas_measure_text(cv, text);
    canvas_set_font_stretch(cv, CANVAS_FONT_STRETCH_CONDENSED);
    float const w_cond = canvas_measure_text(cv, text);
    CHECK(differ(w_normal, w_cond));  // a real width face resolved
    CHECK(w_cond < w_normal);         // condensed is narrower
    canvas_free(cv);
}

// On a family with no width face (Menlo), every stretch keyword resolves the
// same face: the width matches NORMAL exactly (no width is synthesized).
static void test_stretch_noop(void) {
    struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas_set_font_family(cv, "Menlo");
    canvas_set_font_size(cv, 32.0f);
    char const *__null_terminated text = "Magazine";

    canvas_set_font_stretch(cv, CANVAS_FONT_STRETCH_NORMAL);
    float const w_normal = canvas_measure_text(cv, text);
    canvas_set_font_stretch(cv, CANVAS_FONT_STRETCH_CONDENSED);
    CHECK(exact(canvas_measure_text(cv, text), w_normal));     // no width face
    canvas_set_font_stretch(cv, CANVAS_FONT_STRETCH_ULTRA_EXPANDED);
    CHECK(exact(canvas_measure_text(cv, text), w_normal));
    canvas_free(cv);
}

// Out-of-range enum values are ignored (keep the current setting).
static void test_ignore_out_of_range(void) {
    struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas_set_font_family(cv, "Futura");
    canvas_set_font_size(cv, 32.0f);
    char const *__null_terminated text = "Magazine";

    // A garbage variantCaps enum is ignored: variantCaps stays SMALL_CAPS.
    canvas_set_font_variant_caps(cv, CANVAS_FONT_VARIANT_CAPS_SMALL_CAPS);
    float const w_sc = canvas_measure_text(cv, text);
    canvas_set_font_variant_caps(cv, (enum canvas_font_variant_caps)999);  // ignored
    CHECK(exact(canvas_measure_text(cv, text), w_sc));

    // A garbage stretch enum is ignored: stretch stays CONDENSED.
    canvas_set_font_variant_caps(cv, CANVAS_FONT_VARIANT_CAPS_NORMAL);
    canvas_set_font_stretch(cv, CANVAS_FONT_STRETCH_CONDENSED);
    float const w_cond = canvas_measure_text(cv, text);
    canvas_set_font_stretch(cv, (enum canvas_font_stretch)999);  // ignored
    CHECK(exact(canvas_measure_text(cv, text), w_cond));
    canvas_free(cv);
}

// reset() restores NORMAL / NORMAL.
static void test_reset_restores_default(void) {
    struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas_set_font_family(cv, "Futura");
    canvas_set_font_size(cv, 32.0f);
    char const *__null_terminated text = "Magazine";

    float const w_default = canvas_measure_text(cv, text);  // NORMAL width
    canvas_set_font_stretch(cv, CANVAS_FONT_STRETCH_CONDENSED);
    CHECK(differ(canvas_measure_text(cv, text), w_default));

    canvas_reset(cv);
    canvas_set_font_family(cv, "Futura");  // reset cleared family + size too
    canvas_set_font_size(cv, 32.0f);
    CHECK(exact(canvas_measure_text(cv, text), w_default));  // back to NORMAL width
    canvas_free(cv);
}

// A non-default scene for the round trip: small caps on an smcp face and a
// condensed width face, both drawn so the recorded blocks carry the substituted
// and width-face glyphs.
static void draw_scene(struct canvas *__single cv) {
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.95f, 0.95f, 0.9f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.1f, 0.1f, 0.4f, 1.0f);
    canvas_set_font_family(cv, "Baskerville");
    canvas_set_font_size(cv, 26.0f);
    canvas_set_font_variant_caps(cv, CANVAS_FONT_VARIANT_CAPS_SMALL_CAPS);
    canvas_fill_text(cv, "Chapter", 6.0f, 30.0f);
    canvas_set_font_family(cv, "Futura");
    canvas_set_font_variant_caps(cv, CANVAS_FONT_VARIANT_CAPS_NORMAL);
    canvas_set_font_stretch(cv, CANVAS_FONT_STRETCH_CONDENSED);
    canvas_fill_text(cv, "Magazine", 6.0f, 58.0f);
}

// Record a non-default scene, then replay it onto a fresh canvas: the surfaces
// match byte for byte (the recorded blocks carry the shaped runs with the
// substituted small-cap and width-face glyphs baked in; the setter ops and the
// shaping-block variantCaps/stretch tokens reproduce the cache identity, so
// replay never calls the platform text system).
static void test_record_replay_roundtrip(void) {
    char const *__null_terminated path = "build/test_fontvariants_rt.canvas";

    uint8_t recorded_px[NPX];
    {
        struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas_record_to(cv, path));
        draw_scene(cv);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, recorded_px, (int)sizeof recorded_px);
        canvas_free(cv);  // flush + close
    }
    CHECK(drew_anything(recorded_px, NPX));

    {
        struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas_replay_from(cv, path));
        uint8_t replayed_px[NPX];
        canvas_read_rgba(cv, CANVAS_CS_SRGB, replayed_px, (int)sizeof replayed_px);
        CHECK(memcmp(recorded_px, replayed_px, sizeof recorded_px) == 0);
        canvas_free(cv);
    }
}

int main(void) {
    test_variant_caps_substitutes();
    test_variant_caps_noop();
    test_stretch_condenses();
    test_stretch_noop();
    test_ignore_out_of_range();
    test_reset_restores_default();
    test_record_replay_roundtrip();
    return TEST_REPORT();
}
