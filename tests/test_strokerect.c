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
    canvas *__single cv = canvas_create(w, h);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    // A red rectangle outline, width 4, from (16,16) to (48,48): the border is
    // painted (half-width 2 either side of each edge), the interior is not.
    canvas_set_stroke_rgba(cv, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_set_line_width(cv, 4.0f);
    canvas_stroke_rect(cv, 16.0f, 16.0f, 32.0f, 32.0f);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 16, 32), 255, 0, 0, 255, 1));  // left edge
    CHECK(px_near(pixel_at(px, len, w, 32, 16), 255, 0, 0, 255, 1));  // top edge
    CHECK(px_near(pixel_at(px, len, w, 48, 32), 255, 0, 0, 255, 1));  // right edge
    CHECK(px_near(pixel_at(px, len, w, 16, 16), 255, 0, 0, 255, 1));  // corner join
    CHECK(px_near(pixel_at(px, len, w, 32, 32), 0, 0, 0, 0, 1));      // hollow centre
    CHECK(px_near(pixel_at(px, len, w, 4, 4), 0, 0, 0, 0, 1));        // outside

    // The current transform applies: under translate(10,10), the rect lands at
    // device (10,10)-(30,30).
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_save(cv);
    canvas_translate(cv, 10.0f, 10.0f);
    canvas_set_stroke_rgba(cv, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_stroke_rect(cv, 0.0f, 0.0f, 20.0f, 20.0f);
    canvas_restore(cv);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 10, 20), 0, 0, 255, 255, 1));  // left edge
    CHECK(px_near(pixel_at(px, len, w, 20, 20), 0, 0, 0, 0, 1));      // hollow centre

    // A zero-height rect is a horizontal hairline (caps, not a closed box).
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_stroke_rgba(cv, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas_set_line_width(cv, 4.0f);
    canvas_stroke_rect(cv, 10.0f, 30.0f, 40.0f, 0.0f);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 30, 30), 0, 255, 0, 255, 1));  // on the line
    CHECK(px_near(pixel_at(px, len, w, 30, 20), 0, 0, 0, 0, 1));      // off the line

    // strokeRect must not disturb the current path: build a fillable rect in the
    // path, strokeRect elsewhere, then fill() and confirm the path is intact.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_line_width(cv, 4.0f);
    canvas_begin_path(cv);
    canvas_rect(cv, 8.0f, 8.0f, 16.0f, 16.0f);
    canvas_set_stroke_rgba(cv, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_stroke_rect(cv, 40.0f, 40.0f, 16.0f, 16.0f);
    canvas_set_fill_rgba(cv, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_fill(cv);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 16, 16), 0, 0, 255, 255, 1));  // path filled
    CHECK(px_near(pixel_at(px, len, w, 40, 48), 255, 0, 0, 255, 1));  // strokeRect edge
    CHECK(px_near(pixel_at(px, len, w, 48, 48), 0, 0, 0, 0, 1));      // strokeRect hollow

    // Non-finite arguments paint nothing.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_stroke_rgba(cv, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_stroke_rect(cv, 0.0f, 0.0f, (float)INFINITY, 10.0f);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 0, 0), 0, 0, 0, 0, 1));

    canvas_destroy(cv);
    free(px);
    return TEST_REPORT();
}
