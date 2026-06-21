#include "canvas2d.h"
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
    struct canvas2d_context *__single cv = canvas2d(w, h, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    // Rectangular clip: only the clipped window paints.
    canvas2d_save(cv);
    canvas2d_begin_path(cv);
    canvas2d_rect(cv, 16.0f, 16.0f, 32.0f, 32.0f);
    canvas2d_clip(cv, CANVAS2D_NONZERO);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 255, 0, 0, 255, 1));  // inside clip
    CHECK(px_near(pixel_at(px, len, w, 8, 8), 0, 0, 0, 0, 1));        // outside clip
    CHECK(px_near(pixel_at(px, len, w, 56, 56), 0, 0, 0, 0, 1));      // outside clip

    // restore() lifts the clip: the whole canvas paints again.
    canvas2d_restore(cv);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, w, 8, 8), 0, 255, 0, 255, 1));    // now paintable
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 0, 255, 0, 255, 1));

    // Two clips intersect: paint only where both windows overlap.
    canvas2d_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_save(cv);
    canvas2d_begin_path(cv);
    canvas2d_rect(cv, 8.0f, 8.0f, 40.0f, 40.0f);  // x,y in [8,48)
    canvas2d_clip(cv, CANVAS2D_NONZERO);
    canvas2d_begin_path(cv);
    canvas2d_rect(cv, 24.0f, 0.0f, 40.0f, 64.0f);  // x in [24,64)
    canvas2d_clip(cv, CANVAS2D_NONZERO);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 0, 0, 255, 255, 1));  // in both
    CHECK(px_near(pixel_at(px, len, w, 12, 32), 0, 0, 0, 0, 1));      // first only (x<24)
    CHECK(px_near(pixel_at(px, len, w, 52, 32), 0, 0, 0, 0, 1));      // second only (x>=48)
    canvas2d_restore(cv);

    // A non-rectangular (circular) clip masks to the disc.
    canvas2d_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_save(cv);
    canvas2d_begin_path(cv);
    canvas2d_arc(cv, 32.0f, 32.0f, 16.0f, 0.0f, 2.0f * (float)M_PI, false);
    canvas2d_clip(cv, CANVAS2D_NONZERO);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 1.0f, 0.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 255, 255, 0, 255, 1));  // disc centre
    CHECK(px_near(pixel_at(px, len, w, 32, 10), 0, 0, 0, 0, 1));        // outside disc
    canvas2d_restore(cv);

    // After restoring out of every clip, painting is unmasked once more.
    canvas2d_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 0.0f, 1.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, w, 4, 4), 255, 0, 255, 255, 1));    // corner paints

    canvas2d_free(cv);
    free(px);
    return TEST_REPORT();
}
