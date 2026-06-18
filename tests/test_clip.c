#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>

int main(void) {
    int const w = 64;
    int const h = 64;
    int const len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }
    struct canvas *__single cv = canvas(w, h);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    // Rectangular clip: only the clipped window paints.
    canvas_save(cv);
    canvas_begin_path(cv);
    canvas_rect(cv, 16.0f, 16.0f, 32.0f, 32.0f);
    canvas_clip(cv, CANVAS_NONZERO);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 255, 0, 0, 255, 1));  // inside clip
    CHECK(px_near(pixel_at(px, len, w, 8, 8), 0, 0, 0, 0, 1));        // outside clip
    CHECK(px_near(pixel_at(px, len, w, 56, 56), 0, 0, 0, 0, 1));      // outside clip

    // restore() lifts the clip: the whole canvas paints again.
    canvas_restore(cv);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 8, 8), 0, 255, 0, 255, 1));    // now paintable
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 0, 255, 0, 255, 1));

    // Two clips intersect: paint only where both windows overlap.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_save(cv);
    canvas_begin_path(cv);
    canvas_rect(cv, 8.0f, 8.0f, 40.0f, 40.0f);  // x,y in [8,48)
    canvas_clip(cv, CANVAS_NONZERO);
    canvas_begin_path(cv);
    canvas_rect(cv, 24.0f, 0.0f, 40.0f, 64.0f);  // x in [24,64)
    canvas_clip(cv, CANVAS_NONZERO);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 0, 0, 255, 255, 1));  // in both
    CHECK(px_near(pixel_at(px, len, w, 12, 32), 0, 0, 0, 0, 1));      // first only (x<24)
    CHECK(px_near(pixel_at(px, len, w, 52, 32), 0, 0, 0, 0, 1));      // second only (x>=48)
    canvas_restore(cv);

    // A non-rectangular (circular) clip masks to the disc.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_save(cv);
    canvas_begin_path(cv);
    canvas_arc(cv, 32.0f, 32.0f, 16.0f, 0.0f, 2.0f * (float)M_PI, false);
    canvas_clip(cv, CANVAS_NONZERO);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 1.0f, 0.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 255, 255, 0, 255, 1));  // disc centre
    CHECK(px_near(pixel_at(px, len, w, 32, 10), 0, 0, 0, 0, 1));        // outside disc
    canvas_restore(cv);

    // After restoring out of every clip, painting is unmasked once more.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 4, 4), 255, 0, 255, 255, 1));    // corner paints

    canvas_free(cv);
    free(px);
    return TEST_REPORT();
}
