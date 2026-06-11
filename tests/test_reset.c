#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>

static bool is_identity(canvas_matrix m) {
    float const tol = 1e-5f;
    return fabsf(m.a - 1) < tol && fabsf(m.b) < tol && fabsf(m.c) < tol &&
           fabsf(m.d - 1) < tol && fabsf(m.e) < tol && fabsf(m.f) < tol;
}

int main(void) {
    int const w = 16;
    int const h = 16;
    int const len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }
    struct canvas *__single cv = canvas_create(w, h);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    // Dirty everything: paint the bitmap, move the transform, change paint and
    // alpha, set a dash, push a state, and clip to a tiny corner.
    canvas_set_fill_rgba(cv, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_translate(cv, 5.0f, 5.0f);
    canvas_set_global_alpha(cv, 0.5f);
    canvas_set_line_width(cv, 9.0f);
    float const dash[2] = { 3.0f, 3.0f };
    canvas_set_line_dash(cv, dash, 2);
    canvas_save(cv);
    canvas_set_fill_rgba(cv, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas_begin_path(cv);
    canvas_rect(cv, 0.0f, 0.0f, 2.0f, 2.0f);
    canvas_clip(cv);

    canvas_reset(cv);

    // The bitmap is cleared to transparent black.
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 8, 8), 0, 0, 0, 0, 0));
    CHECK(px_near(pixel_at(px, len, w, 0, 0), 0, 0, 0, 0, 0));

    // State is back to defaults: identity transform and an empty dash.
    CHECK(is_identity(canvas_get_transform(cv)));
    CHECK(canvas_get_line_dash(cv, NULL, 0) == 0);

    // Painting with the (reset) default fill proves fill colour reverted to
    // opaque black, global alpha to 1, the transform to identity, and the clip to
    // open: the whole canvas -- including the old clip's excluded area -- paints
    // opaque black.
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, w, 8, 8), 0, 0, 0, 255, 1));
    CHECK(px_near(pixel_at(px, len, w, 15, 15), 0, 0, 0, 255, 1));

    // The save/restore stack was emptied: this restore is a no-op, not a pop of
    // the pre-reset saved state.
    canvas_restore(cv);
    CHECK(is_identity(canvas_get_transform(cv)));

    canvas_destroy(cv);
    free(px);
    return TEST_REPORT();
}
