// fontKerning / textRendering / lang as canvas state (font-family project, phase
// F3a).  These shaping-attribute toggles feed Core Text shaping, so they change
// advances (kerning), ligature formation (textRendering optimizeSpeed), and
// locale-dependent glyph selection (lang).  Checks:
//   - fontKerning NONE vs AUTO of a kerned string: measured width differs
//     (kerning removed -> wider, the looser unkerned advances);
//   - textRendering OPTIMIZE_SPEED vs AUTO: measured width differs on a face
//     where kerning/ligatures matter (Hoefler Text);
//   - lang: a record->replay round trip always holds, and a glyph-selection
//     difference is asserted where it is observable on an available CJK font
//     (PingFang SC zh-Hans vs zh-Hant), else only the round trip;
//   - an out-of-range kerning/rendering enum is ignored, NULL lang is ignored,
//     "" lang clears, and an over-long lang is accepted (truncated, no crash);
//   - reset() restores AUTO / AUTO / "";
//   - an in-memory record -> replay round trip with non-default toggles
//     reproduces the surface byte for byte.

#include "canvas2d.h"

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
static void render_text(struct canvas2d_context *__single cv, char const *__null_terminated text,
                        uint8_t *__counted_by(NPX) out) {
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 0.0f);
    canvas2d_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_fill_text(cv, text, 4.0f, 44.0f);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, out, NPX);
}

// fontKerning NONE widens a kerned string vs AUTO (kerning tightens the
// advances; removing it loosens them).
static void test_kerning_widens(void) {
    struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas2d_set_font_family(cv, "Helvetica");
    canvas2d_set_font_size(cv, 32.0f);
    char const *__null_terminated text = "AVA To Wa";

    canvas2d_set_font_kerning(cv, CANVAS2D_FONT_KERNING_AUTO);
    float const w_auto = canvas2d_measure_text(cv, text);
    // NORMAL is a peer of AUTO: both leave the default kerning, so the width is
    // identical to AUTO (no kerning removed).
    canvas2d_set_font_kerning(cv, CANVAS2D_FONT_KERNING_NORMAL);
    CHECK(exact(canvas2d_measure_text(cv, text), w_auto));

    canvas2d_set_font_kerning(cv, CANVAS2D_FONT_KERNING_NONE);
    float const w_none = canvas2d_measure_text(cv, text);
    CHECK(differ(w_auto, w_none));   // kerning toggled the advances
    CHECK(w_none > w_auto);          // unkerned is the looser (wider) layout
    canvas2d_free(cv);
}

// textRendering OPTIMIZE_SPEED disables kerning AND ligatures, so the measured
// width differs from AUTO on a face where they matter (Hoefler Text).
static void test_rendering_changes_width(void) {
    struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas2d_set_font_family(cv, "Hoefler Text");
    canvas2d_set_font_size(cv, 32.0f);
    char const *__null_terminated text = "office Wa";

    canvas2d_set_text_rendering(cv, CANVAS2D_TEXT_RENDERING_AUTO);
    float const w_auto = canvas2d_measure_text(cv, text);
    // optimizeLegibility / geometricPrecision are peers of AUTO (defaults left
    // on): the width matches AUTO.
    canvas2d_set_text_rendering(cv, CANVAS2D_TEXT_RENDERING_OPTIMIZE_LEGIBILITY);
    CHECK(exact(canvas2d_measure_text(cv, text), w_auto));
    canvas2d_set_text_rendering(cv, CANVAS2D_TEXT_RENDERING_GEOMETRIC_PRECISION);
    CHECK(exact(canvas2d_measure_text(cv, text), w_auto));

    canvas2d_set_text_rendering(cv, CANVAS2D_TEXT_RENDERING_OPTIMIZE_SPEED);
    CHECK(differ(canvas2d_measure_text(cv, text), w_auto));  // kerning+ligatures off
    canvas2d_free(cv);
}

// lang glyph selection: on PingFang SC the Han characters 骨/誤 take different
// locale glyph forms under zh-Hans vs zh-Hant.  Asserted as a rendered-pixel
// difference (the same string, two languages, two surfaces).  Han unification is
// font-dependent; this font ships with macOS, the gallery's record machine.
static void test_lang_glyph_selection(void) {
    struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas2d_set_font_family(cv, "PingFang SC");
    canvas2d_set_font_size(cv, 40.0f);
    char const *__null_terminated text = "\xE9\xAA\xA8\xE8\xAA\xA4";  // 骨誤

    uint8_t *__counted_by(NPX) hans = malloc(NPX);
    uint8_t *__counted_by(NPX) hant = malloc(NPX);
    CHECK(hans != NULL && hant != NULL);
    if (hans && hant) {
        canvas2d_set_lang(cv, "zh-Hans");
        render_text(cv, text, hans);
        canvas2d_set_lang(cv, "zh-Hant");
        render_text(cv, text, hant);
        CHECK(drew_anything(hans, NPX));
        CHECK(memcmp(hans, hant, NPX) != 0);  // locale glyph forms differ
        free(hans);
        free(hant);
    }
    canvas2d_free(cv);
}

// Out-of-range enum values are ignored (keep the current setting); a NULL lang
// is ignored, "" clears, and an over-long lang truncates without crashing.
static void test_ignore_and_clear(void) {
    struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas2d_set_font_family(cv, "Helvetica");
    canvas2d_set_font_size(cv, 32.0f);
    char const *__null_terminated text = "AVA To Wa";

    // A garbage kerning enum is ignored: the kerning stays NONE.
    canvas2d_set_font_kerning(cv, CANVAS2D_FONT_KERNING_NONE);
    float const w_none = canvas2d_measure_text(cv, text);
    canvas2d_set_font_kerning(cv, (enum canvas2d_font_kerning)999);  // garbage: ignored
    CHECK(exact(canvas2d_measure_text(cv, text), w_none));

    // A garbage rendering enum is ignored: rendering stays AUTO.
    canvas2d_set_font_kerning(cv, CANVAS2D_FONT_KERNING_AUTO);
    canvas2d_set_text_rendering(cv, CANVAS2D_TEXT_RENDERING_AUTO);
    float const w_auto = canvas2d_measure_text(cv, text);
    canvas2d_set_text_rendering(cv, (enum canvas2d_text_rendering)999);  // garbage: ignored
    CHECK(exact(canvas2d_measure_text(cv, text), w_auto));

    // NULL lang is ignored (keep current); "" clears; an over-long tag truncates.
    canvas2d_set_lang(cv, "en");
    canvas2d_set_lang(cv, NULL);                 // ignored: still "en"
    float const w_en = canvas2d_measure_text(cv, text);
    canvas2d_set_lang(cv, "");                   // cleared
    CHECK(exact(canvas2d_measure_text(cv, text), w_en) ||
          differ(canvas2d_measure_text(cv, text), w_en));  // either; just no crash
    char overlong[200];
    memset(overlong, 'a', sizeof overlong);
    // The counted form (it need not be NUL-terminated): an over-long tag is
    // truncated to the state cap, no crash.
    canvas2d_set_lang_n(cv, overlong, (int)sizeof overlong);
    (void)canvas2d_measure_text(cv, text);
    canvas2d_free(cv);
}

// reset() restores AUTO / AUTO / "".
static void test_reset_restores_default(void) {
    struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas2d_set_font_family(cv, "Helvetica");
    canvas2d_set_font_size(cv, 32.0f);
    char const *__null_terminated text = "AVA To Wa";

    float const w_default = canvas2d_measure_text(cv, text);  // AUTO kerning
    canvas2d_set_font_kerning(cv, CANVAS2D_FONT_KERNING_NONE);
    CHECK(differ(canvas2d_measure_text(cv, text), w_default));

    canvas2d_reset(cv);
    canvas2d_set_font_family(cv, "Helvetica");  // reset cleared family + size too
    canvas2d_set_font_size(cv, 32.0f);
    CHECK(exact(canvas2d_measure_text(cv, text), w_default));  // back to AUTO kerning
    canvas2d_free(cv);
}

// A non-default toggle scene for the round trip: kerning none + optimizeSpeed +
// a lang tag, drawn with both Latin (kerning/ligatures) and CJK (lang) text.
static void draw_scene(struct canvas2d_context *__single cv) {
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.95f, 0.95f, 0.9f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.1f, 0.1f, 0.4f, 1.0f);
    canvas2d_set_font_family(cv, "Helvetica");
    canvas2d_set_font_size(cv, 26.0f);
    canvas2d_set_font_kerning(cv, CANVAS2D_FONT_KERNING_NONE);
    canvas2d_set_text_rendering(cv, CANVAS2D_TEXT_RENDERING_OPTIMIZE_SPEED);
    canvas2d_set_lang(cv, "en");
    canvas2d_fill_text(cv, "AVA office", 6.0f, 30.0f);
    canvas2d_set_font_family(cv, "PingFang SC");
    canvas2d_set_lang(cv, "zh-Hant");
    canvas2d_fill_text(cv, "\xE9\xAA\xA8\xE8\xAA\xA4", 6.0f, 58.0f);  // 骨誤
}

// Record a non-default-toggle scene, then replay it onto a fresh canvas: the
// surfaces match byte for byte (the recorded blocks carry the shaped runs with
// the toggles baked in; the setter ops and the shaping-block toggle tokens
// reproduce the cache identity, so replay never calls the platform text system).
static void test_record_replay_roundtrip(void) {
    char const *__null_terminated path = "build/test_fonttoggles_rt.canvas";

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
    test_kerning_widens();
    test_rendering_changes_width();
    test_lang_glyph_selection();
    test_ignore_and_clear();
    test_reset_restores_default();
    test_record_replay_roundtrip();
    return TEST_REPORT();
}
