#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>

int main(void) {
    int const w = 8;
    int const h = 8;
    int const len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }

    // Whole canvas opaque red, then clearRect a sub-region.
    struct canvas *__single cv = canvas(w, h);
    CHECK(cv != NULL);
    if (cv) {
        canvas_set_fill_rgba(cv, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
        canvas_read_rgba(cv, px, len);
        CHECK(px_near(pixel_at(px, len, w, 4, 4), 255, 0, 0, 255, 1));
        CHECK(px_near(pixel_at(px, len, w, 0, 0), 255, 0, 0, 255, 1));

        canvas_clear_rect(cv, 2.0f, 2.0f, 4.0f, 4.0f);
        canvas_read_rgba(cv, px, len);
        CHECK(px_near(pixel_at(px, len, w, 4, 4), 0, 0, 0, 0, 1));
        CHECK(px_near(pixel_at(px, len, w, 0, 0), 255, 0, 0, 255, 1));
        canvas_free(cv);
    }

    // 50% blue over red -> ~(128, 0, 128, 255).
    struct canvas *__single cb = canvas(w, h);
    CHECK(cb != NULL);
    if (cb) {
        canvas_set_fill_rgba(cb, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_fill_rect(cb, 0.0f, 0.0f, (float)w, (float)h);
        canvas_set_fill_rgba(cb, 0.0f, 0.0f, 1.0f, 1.0f);
        canvas_set_global_alpha(cb, 0.5f);
        canvas_fill_rect(cb, 0.0f, 0.0f, (float)w, (float)h);
        canvas_read_rgba(cb, px, len);
        CHECK(px_near(pixel_at(px, len, w, 4, 4), 128, 0, 128, 255, 3));
        canvas_free(cb);
    }
    free(px);
    return TEST_REPORT();
}
