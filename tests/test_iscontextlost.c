#include "canvas2d.h"
#include "test_util.h"

int main(void) {
    struct canvas2d_context *__single cv = canvas2d(8, 8, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return TEST_REPORT();
    }

    // The context is never lost: true on a fresh canvas, after drawing, and
    // after a reset.
    CHECK(!canvas2d_is_context_lost(cv));
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, 8.0f, 8.0f);
    CHECK(!canvas2d_is_context_lost(cv));
    canvas2d_reset(cv);
    CHECK(!canvas2d_is_context_lost(cv));

    canvas2d_free(cv);
    return TEST_REPORT();
}
