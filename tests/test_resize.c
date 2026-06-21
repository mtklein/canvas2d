#include "canvas2d.h"
#include "test_pixels.h"
#include "test_util.h"

#include <math.h>
#include <string.h>

static bool is_identity(canvas2d_matrix m) {
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
    struct canvas2d_context *__single cv = canvas2d(8, 8, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    // Paint the 8x8 canvas, then grow to 16x16: the bitmap is reallocated and
    // cleared.  Pre-fill the readback buffer with a sentinel so an unchanged
    // (still-8x8) canvas would leave the far corner unwritten and fail the check.
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, 8.0f, 8.0f);
    CHECK(canvas2d_resize(cv, 16, 16));
    memset(px, 0xAA, (size_t)cap);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, 16 * 16 * 4);
    CHECK(px_near(pixel_at(px, cap, 16, 0, 0), 0, 0, 0, 0, 0));     // cleared
    CHECK(px_near(pixel_at(px, cap, 16, 15, 15), 0, 0, 0, 0, 0));   // and addressable

    // The enlarged canvas is drawable to its new far corner.
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, 16.0f, 16.0f);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, 16 * 16 * 4);
    CHECK(px_near(pixel_at(px, cap, 16, 15, 15), 0, 255, 0, 255, 1));

    // Resize also resets the drawing state: a transform set beforehand is gone.
    canvas2d_translate(cv, 5.0f, 5.0f);
    CHECK(canvas2d_resize(cv, 4, 4));
    CHECK(is_identity(canvas2d_get_transform(cv)));
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, 4 * 4 * 4);
    CHECK(px_near(pixel_at(px, cap, 4, 3, 3), 0, 0, 0, 0, 0));  // cleared at new size

    // Invalid dimensions fail and leave the canvas untouched.
    CHECK(canvas2d_resize(cv, 8, 8));
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, 8.0f, 8.0f);
    CHECK(!canvas2d_resize(cv, 0, 5));
    CHECK(!canvas2d_resize(cv, -1, 3));
    CHECK(!canvas2d_resize(cv, 20000, 1));  // beyond the max dimension
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, 8 * 8 * 4);
    CHECK(px_near(pixel_at(px, cap, 8, 7, 7), 0, 0, 255, 255, 1));  // still 8x8 blue

    canvas2d_free(cv);
    free(px);
    return TEST_REPORT();
}
