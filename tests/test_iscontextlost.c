#include "canvas.h"
#include "test_util.h"

int main(void) {
    struct canvas *__single cv = canvas(8, 8);
    CHECK(cv != NULL);
    if (!cv) {
        return TEST_REPORT();
    }

    // The context is never lost: true on a fresh canvas, after drawing, and
    // after a reset.
    CHECK(!canvas_is_context_lost(cv));
    canvas_set_fill_rgba(cv, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, 8.0f, 8.0f);
    CHECK(!canvas_is_context_lost(cv));
    canvas_reset(cv);
    CHECK(!canvas_is_context_lost(cv));

    canvas_free(cv);
    return TEST_REPORT();
}
