#include "canvas.h"
#include "test_util.h"

#include <stdlib.h>

// Color emoji through the public text API: fill_text must fall back to a color
// font and composite the glyph as a *colour* bitmap, not draw a grayscale .notdef
// box.  (The boundary itself is covered by test_shape; this guards the wiring into
// canvas_fill_text.)
int main(void) {
    int const w = 80, h = 80, len = w * h * 4, n = w * h;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }

    canvas *__single cv = canvas_create(w, h);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
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
        int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        if (mx - mn > 40) {
            colored++;  // a chromatic pixel: not grayscale coverage / a black box
        }
    }
    CHECK(ink > 200);      // the emoji rendered
    CHECK(colored > 50);   // and in colour

    // Plain ASCII still renders ink (the outline path coexists with color glyphs).
    canvas_set_fill_rgba(cv, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_rgba(cv, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_text(cv, "Hi", 8.0f, 60.0f);
    canvas_read_rgba(cv, px, len);
    long ascii_ink = 0;
    for (int i = 0; i < n; i++) {
        if (px[i * 4] < 128) {
            ascii_ink++;
        }
    }
    CHECK(ascii_ink > 50);

    canvas_destroy(cv);
    free(px);
    return TEST_REPORT();
}
