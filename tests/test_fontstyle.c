// fontWeight / fontStyle as canvas state (font-family project, phase F2).
// Checks:
//   - bold (700) vs regular (400) of the same text/family differ in advance
//     and/or rendered ink (the weight really threads through shaping);
//   - italic vs normal differ in rendered ink;
//   - the SYNTHESIS aliasing guard: a family with no real bold/italic face --
//     synth differs from regular AND does not alias it in the glyph cache (draw
//     regular then synth of the same glyph and confirm the pixels differ);
//   - weight clamping (50 -> 100, 1000 -> 900) and a garbage style ignored;
//   - reset() restores 400 / NORMAL;
//   - an in-memory record -> replay round trip with a non-default weight+style
//     reproduces the surface byte for byte.

#include "canvas2d.h"
#include "canvas2d_text.h"  // the cache handle: interned faces + stats

#include "test_util.h"

#include <math.h>
#include <ptrcheck.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define W 160
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

// Count non-transparent bytes: a coarse "how much ink" proxy.  Two renders that
// differ here painted different pixels.
static int ink_bytes(uint8_t const *__counted_by(n) px, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) {
        if (px[i] != 0) {
            c++;
        }
    }
    return c;
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

// Bold differs from regular: a real bold face lays the text out at different
// advances and/or paints different ink.
static void test_bold_vs_regular(void) {
    struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas2d_set_font_family(cv, "Helvetica");
    canvas2d_set_font_size(cv, 32.0f);
    char const *__null_terminated text = "Weighty";

    canvas2d_set_font_weight(cv, 400);
    float const w_reg = canvas2d_measure_text(cv, text);
    uint8_t *__counted_by(NPX) reg = malloc(NPX);
    uint8_t *__counted_by(NPX) bold = malloc(NPX);
    CHECK(reg != NULL && bold != NULL);
    if (reg && bold) {
        render_text(cv, text, reg);
        canvas2d_set_font_weight(cv, 700);
        float const w_bold = canvas2d_measure_text(cv, text);
        render_text(cv, text, bold);
        // Different advances and/or different ink: at least one must hold.
        CHECK(differ(w_reg, w_bold) || memcmp(reg, bold, NPX) != 0);
        // Bold is heavier: more ink than regular (Helvetica-Bold's stems).
        CHECK(ink_bytes(bold, NPX) > ink_bytes(reg, NPX));
        free(reg);
        free(bold);
    }
    canvas2d_free(cv);
}

// Italic differs from normal: a real oblique face paints different (slanted) ink.
static void test_italic_vs_normal(void) {
    struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas2d_set_font_family(cv, "Helvetica");
    canvas2d_set_font_size(cv, 32.0f);
    char const *__null_terminated text = "Slanted";

    uint8_t *__counted_by(NPX) up = malloc(NPX);
    uint8_t *__counted_by(NPX) it = malloc(NPX);
    CHECK(up != NULL && it != NULL);
    if (up && it) {
        canvas2d_set_font_style(cv, CANVAS2D_FONT_STYLE_NORMAL);
        render_text(cv, text, up);
        canvas2d_set_font_style(cv, CANVAS2D_FONT_STYLE_ITALIC);
        render_text(cv, text, it);
        CHECK(memcmp(up, it, NPX) != 0);  // the slant moved pixels
        free(up);
        free(it);
    }
    canvas2d_free(cv);
}

// The aliasing guard.  A family with no real italic face (Papyrus) gets a
// SYNTHESIZED oblique whose resolved font NAME matches the regular face's.  If
// the glyph cache keyed by name alone, the synth-italic draw would return the
// regular outline and the two renders would be byte-identical.  They must NOT be:
// the cache identity includes weight/style.  Drawn regular FIRST, so the regular
// glyph is cached; the italic draw must not pull that cached regular outline.
static void test_synth_no_alias(void) {
    struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas2d_set_font_family(cv, "Papyrus");
    canvas2d_set_font_size(cv, 30.0f);
    char const *__null_terminated text = "Papyrus";

    uint8_t *__counted_by(NPX) reg = malloc(NPX);
    uint8_t *__counted_by(NPX) it = malloc(NPX);
    CHECK(reg != NULL && it != NULL);
    if (reg && it) {
        canvas2d_set_font_style(cv, CANVAS2D_FONT_STYLE_NORMAL);
        render_text(cv, text, reg);       // regular cached first
        CHECK(drew_anything(reg, NPX));
        canvas2d_set_font_style(cv, CANVAS2D_FONT_STYLE_ITALIC);
        render_text(cv, text, it);        // synth italic: must not alias regular
        CHECK(drew_anything(it, NPX));
        CHECK(memcmp(reg, it, NPX) != 0);  // the synthesized slant is visible

        // The cache interned two distinct faces for the same family/name: the
        // regular and the synthesized italic occupy separate ids.
        struct canvas2d_text_cache *__single c = canvas2d_canvas_text_cache(cv);
        bool saw_upright = false, saw_italic = false;
        for (int i = 0; i < c->nfonts; i++) {
            if (c->font[i].italic) {
                saw_italic = true;
            } else {
                saw_upright = true;
            }
        }
        CHECK(saw_upright && saw_italic);
        free(reg);
        free(it);
    }
    canvas2d_free(cv);
}

// Weight clamps to [100, 900]; a garbage style enum is ignored.
static void test_clamp_and_ignore(void) {
    struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas2d_set_font_family(cv, "Helvetica");
    canvas2d_set_font_size(cv, 32.0f);
    char const *__null_terminated text = "Clamp";

    // 50 clamps to 100, which measures like an explicit 100.
    canvas2d_set_font_weight(cv, 50);
    float const w_lo_clamped = canvas2d_measure_text(cv, text);
    canvas2d_set_font_weight(cv, 100);
    CHECK(exact(canvas2d_measure_text(cv, text), w_lo_clamped));

    // 1000 clamps to 900, which measures like an explicit 900.
    canvas2d_set_font_weight(cv, 1000);
    float const w_hi_clamped = canvas2d_measure_text(cv, text);
    canvas2d_set_font_weight(cv, 900);
    CHECK(exact(canvas2d_measure_text(cv, text), w_hi_clamped));

    // A garbage style value is ignored: the style stays italic.
    canvas2d_set_font_style(cv, CANVAS2D_FONT_STYLE_ITALIC);
    uint8_t *__counted_by(NPX) before = malloc(NPX);
    uint8_t *__counted_by(NPX) after = malloc(NPX);
    CHECK(before != NULL && after != NULL);
    if (before && after) {
        canvas2d_set_font_weight(cv, 400);
        render_text(cv, text, before);
        canvas2d_set_font_style(cv, (enum canvas2d_font_style)999);  // garbage: ignored
        render_text(cv, text, after);
        CHECK(memcmp(before, after, NPX) == 0);  // still italic, unchanged
        free(before);
        free(after);
    }
    canvas2d_free(cv);
}

// reset() restores 400 / NORMAL.
static void test_reset_restores_default(void) {
    struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas2d_set_font_family(cv, "Helvetica");
    canvas2d_set_font_size(cv, 32.0f);
    char const *__null_terminated text = "Default";

    float const w_default = canvas2d_measure_text(cv, text);  // 400, normal
    canvas2d_set_font_weight(cv, 900);
    canvas2d_set_font_style(cv, CANVAS2D_FONT_STYLE_ITALIC);
    CHECK(differ(canvas2d_measure_text(cv, text), w_default));

    canvas2d_reset(cv);
    canvas2d_set_font_family(cv, "Helvetica");  // reset cleared family + size too
    canvas2d_set_font_size(cv, 32.0f);
    CHECK(exact(canvas2d_measure_text(cv, text), w_default));  // back to 400/normal

    canvas2d_free(cv);
}

// A non-default weight+style scene for the round trip.
static void draw_scene(struct canvas2d_context *__single cv) {
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.95f, 0.95f, 0.9f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.1f, 0.1f, 0.4f, 1.0f);
    canvas2d_set_font_family(cv, "Helvetica");
    canvas2d_set_font_weight(cv, 700);
    canvas2d_set_font_style(cv, CANVAS2D_FONT_STYLE_ITALIC);
    canvas2d_set_font_size(cv, 28.0f);
    canvas2d_fill_text(cv, "Bold It", 6.0f, 44.0f);
}

// Record a bold-italic scene, then replay it onto a fresh canvas: the surfaces
// match byte for byte (the recorded blocks carry the resolved glyphs; the
// set_font_weight/set_font_style ops and the shaping/font weight-style tokens
// reproduce the cache identity).
static void test_record_replay_roundtrip(void) {
    char const *__null_terminated path = "build/test_fontstyle_rt.canvas";

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
    test_bold_vs_regular();
    test_italic_vs_normal();
    test_synth_no_alias();
    test_clamp_and_ignore();
    test_reset_restores_default();
    test_record_replay_roundtrip();
    return TEST_REPORT();
}
