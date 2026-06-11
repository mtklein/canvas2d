#include "canvas.h"
#include "cnvs_text.h"
#include "test_util.h"

#include <stdlib.h>
#include <string.h>

// Color emoji through the public text API, and the canonical-capture machinery
// behind it (cnvs_text_cache_color / cnvs_glyph_mip in src/cnvs_text.h):
//   - fill_text falls back to a color font and composites the glyph in colour;
//   - one capture per (font, glyph): the second draw reuses the same bytes,
//     no new boundary fetch;
//   - the mip pyramid is exact 2x2 box halving (hand-checked, odd dims, the
//     premul invariant preserved at every level);
//   - mip selection picks the smallest level >= the device footprint;
//   - drawing at exactly the capture size reproduces the capture's bytes
//     (within compositing rounding), and at half size the level-1 mip's.

// The emoji capture slot the canvas holds after drawing one emoji: scan the
// glyph table for the (single) entry carrying a capture.
static struct cnvs_glyph_slot *__single find_capture(struct canvas *__single cv) {
    struct cnvs_text_cache *__single c = cnvs_canvas_text_cache(cv);
    for (int i = 0; i < c->glyph_cap; i++) {
        if (c->glyph[i].used && c->glyph[i].capture_w > 0) {
            return &c->glyph[i];
        }
    }
    return NULL;
}

static void check_renders_in_color(void) {
    int const w = 80, h = 80, len = w * h * 4, n = w * h;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return;
    }

    struct canvas *__single cv = canvas(w, h);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return;
    }

    // White ground; a black fill colour (ignored for a color glyph).
    canvas_set_fill_rgba(cv, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_rgba(cv, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas_set_font_size(cv, 56.0f);
    canvas_fill_text(cv, "\xF0\x9F\x8C\x88", 12.0f, 64.0f);  // 🌈 U+1F308
    canvas_read_rgba(cv, px, len);

    long ink = 0, colored = 0;
    for (int i = 0; i < n; i++) {
        int r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2];
        if (!(r == 255 && g == 255 && b == 255)) {
            ink++;  // drew something over the white ground
        }
        int const mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        int const mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        if (mx - mn > 40) {
            colored++;  // a chromatic pixel: not grayscale coverage / a black box
        }
    }
    CHECK(ink > 200);      // the emoji rendered
    CHECK(colored > 50);   // and in colour

    // One capture, fetched once: the draw above cost one boundary
    // rasterization; a second draw (any size) reuses the same bytes.
    struct cnvs_glyph_slot *__single slot = find_capture(cv);
    CHECK(slot != NULL);
    if (slot) {
        CHECK(slot->capture_w == CNVS_CAPTURE_EM && slot->capture_h == CNVS_CAPTURE_EM);
        CHECK(slot->capture_size == CNVS_CAPTURE_EM * CNVS_CAPTURE_EM * 4);
        uint8_t *snap = malloc((size_t)slot->capture_size);
        CHECK(snap != NULL);
        if (snap) {
            memcpy(snap, slot->capture, (size_t)slot->capture_size);
            uint8_t const *before = slot->capture;
            struct cnvs_text_cache *__single c = cnvs_canvas_text_cache(cv);
            int const miss = c->glyph_misses;
            canvas_set_font_size(cv, 23.0f);
            canvas_fill_text(cv, "\xF0\x9F\x8C\x88", 12.0f, 40.0f);
            CHECK(c->glyph_misses == miss);    // no new boundary fetch
            CHECK(slot->capture == before);    // same buffer...
            CHECK(memcmp(snap, slot->capture, (size_t)slot->capture_size) == 0);  // ...same bytes
            free(snap);
        }
    }

    // Plain ASCII still renders ink (the outline path coexists with color glyphs).
    canvas_set_fill_rgba(cv, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_rgba(cv, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas_set_font_size(cv, 56.0f);
    canvas_fill_text(cv, "Hi", 8.0f, 60.0f);
    canvas_read_rgba(cv, px, len);
    long ascii_ink = 0;
    for (int i = 0; i < n; i++) {
        if (px[i * 4] < 128) {
            ascii_ink++;
        }
    }
    CHECK(ascii_ink > 50);

    canvas_free(cv);
    free(px);
}

// Hand-checkable box halving: a 4x4 whose 2x2 block sums are known, halved to
// 2x2 and then 1x1; every channel runs the same math, so the test drives one.
static void check_halve_4x4(void) {
    uint8_t src[4 * 4 * 4];
    for (int i = 0; i < 16; i++) {  // pixel value 16*i in all four channels
        for (int k = 0; k < 4; k++) {
            src[i * 4 + k] = (uint8_t)(16 * i);
        }
    }
    uint8_t mid[2 * 2 * 4];
    cnvs_mip_halve(src, 4, 4, mid, 2, 2);
    // block sums: (0+16+64+80)=160, (32+48+96+112)=288, (128+144+192+208)=672,
    // (160+176+224+240)=800; rounded means 40, 72, 168, 200.
    CHECK(mid[0 * 4] == 40 && mid[1 * 4] == 72 && mid[2 * 4] == 168 &&
          mid[3 * 4] == 200);
    uint8_t one[1 * 1 * 4];
    cnvs_mip_halve(mid, 2, 2, one, 1, 1);
    CHECK(one[0] == 120);  // (40+72+168+200+2)>>2

    // Rounding: a lone 1 in a 2x2 block floors (3/4 -> 0); a lone 2 rounds up.
    uint8_t r2[2 * 2 * 4] = { 0 };
    uint8_t r1[1 * 1 * 4];
    r2[0] = 1;
    cnvs_mip_halve(r2, 2, 2, r1, 1, 1);
    CHECK(r1[0] == 0);
    r2[0] = 2;
    cnvs_mip_halve(r2, 2, 2, r1, 1, 1);
    CHECK(r1[0] == 1);
}

// Odd dimensions ceil-halve with the edge row/column replicated: 5x3 -> 3x2,
// where the right column and bottom row of the destination read the clamped
// source edge.
static void check_halve_odd(void) {
    uint8_t src[5 * 3 * 4];
    memset(src, 0, sizeof src);
    for (int k = 0; k < 4; k++) {
        src[(2 * 5 + 4) * 4 + k] = 200;  // bottom-right source pixel
    }
    uint8_t dst[3 * 2 * 4];
    memset(dst, 0xAA, sizeof dst);
    cnvs_mip_halve(src, 5, 3, dst, 3, 2);
    // dst(2,1) clamps both axes onto src(4,2): all four samples are 200.
    CHECK(dst[(1 * 3 + 2) * 4] == 200);
    // dst(1,1) samples src(2..3, 2): all zero.
    CHECK(dst[(1 * 3 + 1) * 4] == 0);
    // a constant image stays constant through replicated edges
    memset(src, 100, sizeof src);
    cnvs_mip_halve(src, 5, 3, dst, 3, 2);
    for (int i = 0; i < (int)sizeof dst; i++) {
        CHECK(dst[i] == 100);
    }
    // mismatched destination dims are a no-op, not a partial write
    memset(dst, 0x55, sizeof dst);
    cnvs_mip_halve(src, 5, 3, dst, 2, 2);
    CHECK(dst[0] == 0x55);
}

// xorshift32 for a deterministic pseudo-random premul image (the test_record
// pattern: run in 64-bit and masked so the deliberate wrap stays out of
// -fsanitize=integer's lane).
static uint32_t xorshift32(uint32_t *__single s) {
    uint64_t x = *s;
    x = (x ^ (x << 13)) & 0xFFFFFFFFu;
    x = x ^ (x >> 17);
    x = (x ^ (x << 5)) & 0xFFFFFFFFu;
    *s = (uint32_t)x;
    return (uint32_t)x;
}

// The premul invariant r,g,b <= a survives every halving level exactly: all
// four channels share one rounding, so summed r <= summed a stays <= through
// the shift.  Swept over an odd-dimension pyramid (7x5 -> 4x3 -> 2x2 -> 1x1).
static void check_halve_premul(void) {
    enum { SW = 7, SH = 5 };
    uint8_t a[SW * SH * 4], b[SW * SH * 4];
    uint32_t seed = 0xBADC0DE;
    for (int i = 0; i < SW * SH; i++) {
        uint32_t const r = xorshift32(&seed);
        uint8_t const alpha = (uint8_t)(r & 0xFF);
        a[i * 4 + 0] = (uint8_t)(((r >> 8) & 0xFF) % (alpha + 1u));
        a[i * 4 + 1] = (uint8_t)(((r >> 16) & 0xFF) % (alpha + 1u));
        a[i * 4 + 2] = (uint8_t)(((r >> 24) & 0xFF) % (alpha + 1u));
        a[i * 4 + 3] = alpha;
    }
    int w = SW, h = SH;
    uint8_t *cur = a, *nxt = b;
    while (w > 1 || h > 1) {
        int const dw = (w + 1) / 2, dh = (h + 1) / 2;
        cnvs_mip_halve(cur, w, h, nxt, dw, dh);
        for (int i = 0; i < dw * dh; i++) {
            CHECK(nxt[i * 4 + 0] <= nxt[i * 4 + 3]);
            CHECK(nxt[i * 4 + 1] <= nxt[i * 4 + 3]);
            CHECK(nxt[i * 4 + 2] <= nxt[i * 4 + 3]);
        }
        uint8_t *t = cur;
        cur = nxt;
        nxt = t;
        w = dw;
        h = dh;
    }
}

// Mip selection on a real capture: the smallest level >= the footprint in
// both dimensions, the capture above the top level, 1x1 at the bottom.
static void check_mip_select(void) {
    struct canvas *__single cv = canvas(64, 64);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas_set_font_size(cv, 32.0f);
    canvas_fill_text(cv, "\xF0\x9F\x8D\x95", 4.0f, 48.0f);  // 🍕
    struct cnvs_glyph_slot *__single slot = find_capture(cv);
    CHECK(slot != NULL);
    if (slot) {
        // 160 halves 8 times: 80 40 20 10 5 3 2 1.
        CHECK(slot->nmips == 8);
        if (slot->nmips == 8) {
            CHECK(slot->mip[5].w == 3 && slot->mip[5].h == 3);  // ceil(5/2)
            CHECK(slot->mip[7].w == 1 && slot->mip[7].h == 1);
        }
        struct { float f; int want; } const cases[] = {
            { 1000.0f, 160 },  // bigger than the capture: the capture
            { 160.0f, 160 },
            { 81.0f, 160 },    // smallest level >= 81
            { 80.0f, 80 },
            { 41.0f, 80 },
            { 40.0f, 40 },
            { 4.0f, 5 },
            { 1.0f, 1 },
            { 0.0f, 1 },       // degenerate footprints take the smallest
            { -3.0f, 1 },
        };
        for (int i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) {
            cnvs_mip const m = cnvs_glyph_mip(slot, cases[i].f);
            CHECK(m.px != NULL);
            CHECK(m.w == cases[i].want);
            CHECK(m.len == m.w * m.h * 4);
        }
    }
    canvas_free(cv);
}

// Draw an emoji at exactly the capture's size over white and read back: every
// interior pixel must match the capture composited over white within a few
// LSB (float->_Float16 tile quantization and the readback rounding).  Then the
// same at half size against the level-1 mip -- the level the footprint rule
// selects.  `against` is the level to compare with; (ox, oy) places the
// glyph's buffer at integer device coords so sampling lands on pixel centres.
static void check_draw_matches_level(struct canvas *__single cv, float size_px,
                                     cnvs_mip lvl, int ox, int oy) {
    int const w = 192, len = 192 * 192 * 4;
    static uint8_t px[192 * 192 * 4];
    canvas_set_fill_rgba(cv, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, 192.0f, 192.0f);
    canvas_set_font_size(cv, size_px);
    // The capture covers glyph-space y in [ink_y0, ink_y0 + capture_h] = [-20, 140]
    // at em 160 for AppleColorEmoji, so the buffer's top edge sits 140*k above
    // the baseline: baseline = oy + 140*k puts the top edge at device y = oy.
    float const k = size_px / (float)CNVS_CAPTURE_EM;
    canvas_fill_text(cv, "\xF0\x9F\x8D\x95", (float)ox, (float)oy + 140.0f * k);
    canvas_read_rgba(cv, px, len);
    int bad = 0;
    for (int y = 1; y + 1 < lvl.h; y++) {  // interior: skip the coverage edge
        for (int x = 1; x + 1 < lvl.w; x++) {
            for (int c = 0; c < 4; c++) {
                int const s = lvl.px[(y * lvl.w + x) * 4 + c];
                int const a = lvl.px[(y * lvl.w + x) * 4 + 3];
                // premul over opaque white, then the unpremultiplying readback
                // (alpha reads back 255): rgb = s + (255 - a), alpha = 255.
                int want = c == 3 ? 255 : s + (255 - a);
                if (want > 255) {
                    want = 255;
                }
                int const got = px[((oy + y) * w + ox + x) * 4 + c];
                int const d = got > want ? got - want : want - got;
                if (d > 3) {
                    bad++;
                }
            }
        }
    }
    CHECK(bad == 0);
}

static void check_draw_equivalence(void) {
    struct canvas *__single cv = canvas(192, 192);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas_set_font_size(cv, 40.0f);
    canvas_fill_text(cv, "\xF0\x9F\x8D\x95", 4.0f, 48.0f);  // populate the capture
    struct cnvs_glyph_slot *__single slot = find_capture(cv);
    CHECK(slot != NULL);
    if (slot) {
        // At 160px device size the dest quad is the capture, texel for texel.
        cnvs_mip const cap = cnvs_glyph_mip(slot, 160.0f);
        CHECK(cap.w == 160);
        check_draw_matches_level(cv, 160.0f, cap, 16, 16);
        // At 80px the footprint rule lands on the level-1 mip, ditto.
        cnvs_mip const half = cnvs_glyph_mip(slot, 80.0f);
        CHECK(half.w == 80);
        check_draw_matches_level(cv, 80.0f, half, 16, 16);
    }
    canvas_free(cv);
}

int main(void) {
    check_renders_in_color();
    check_halve_4x4();
    check_halve_odd();
    check_halve_premul();
    check_mip_select();
    check_draw_equivalence();
    return TEST_REPORT();
}
