#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <math.h>
#include <string.h>

static bool is_identity(canvas_matrix m) {
    float const tol = 1e-5f;
    return fabsf(m.a - 1) < tol && fabsf(m.b) < tol && fabsf(m.c) < tol &&
           fabsf(m.d - 1) < tol && fabsf(m.e) < tol && fabsf(m.f) < tol;
}

int main(void) {
    int const cap = 16 * 16 * 4;
    uint8_t *__counted_by(cap) px = malloc((size_t)cap);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }
    struct canvas *__single cv = canvas(8, 8);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    // Paint the 8x8 canvas, then grow to 16x16: the bitmap is reallocated and
    // cleared.  Pre-fill the readback buffer with a sentinel so an unchanged
    // (still-8x8) canvas would leave the far corner unwritten and fail the check.
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, 8.0f, 8.0f);
    CHECK(canvas_resize(cv, 16, 16));
    memset(px, 0xAA, (size_t)cap);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, 16 * 16 * 4);
    CHECK(px_near(pixel_at(px, cap, 16, 0, 0), 0, 0, 0, 0, 0));     // cleared
    CHECK(px_near(pixel_at(px, cap, 16, 15, 15), 0, 0, 0, 0, 0));   // and addressable

    // The enlarged canvas is drawable to its new far corner.
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, 16.0f, 16.0f);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, 16 * 16 * 4);
    CHECK(px_near(pixel_at(px, cap, 16, 15, 15), 0, 255, 0, 255, 1));

    // Resize also resets the drawing state: a transform set beforehand is gone.
    canvas_translate(cv, 5.0f, 5.0f);
    CHECK(canvas_resize(cv, 4, 4));
    CHECK(is_identity(canvas_get_transform(cv)));
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, 4 * 4 * 4);
    CHECK(px_near(pixel_at(px, cap, 4, 3, 3), 0, 0, 0, 0, 0));  // cleared at new size

    // Invalid dimensions fail and leave the canvas untouched.
    CHECK(canvas_resize(cv, 8, 8));
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, 8.0f, 8.0f);
    CHECK(!canvas_resize(cv, 0, 5));
    CHECK(!canvas_resize(cv, -1, 3));
    CHECK(!canvas_resize(cv, 20000, 1));  // beyond the max dimension
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, 8 * 8 * 4);
    CHECK(px_near(pixel_at(px, cap, 8, 7, 7), 0, 0, 255, 255, 1));  // still 8x8 blue

    canvas_free(cv);
    free(px);
    return TEST_REPORT();
}
