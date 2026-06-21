// The PNG encoder's guard rails and happy path -- branches the gallery gate never
// reaches because it only ever encodes valid canvas sizes.  canvas2d_png_encode
// returns NULL (outlen 0) on non-positive dimensions and on dimensions past the
// 16384 per-axis cap.  (The internal int-overflow guard, which fires only around
// 16384x16384, needs a ~2 GB pixel buffer to even call under -fbounds-safety, so
// it stays belt-and-suspenders rather than a unit test.)  Plus: a valid small
// encode has the PNG signature and a non-zero length, and is deterministic.
#include "canvas2d_png.h"
#include "test_util.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void rejects_bad_dimensions(void) {
    // One buffer big enough to satisfy __counted_by(width*height*4) for the
    // largest dimensions we pass; content is irrelevant (the guards trip first).
    int const cap = 16385 * 4;
    uint16_t *__counted_by(cap) buf = calloc((size_t)cap, sizeof *buf);
    CHECK(buf != NULL);
    if (!buf) {
        return;
    }
    int len;
    len = -1; CHECK(canvas2d_png_encode(buf, 0, 1, &len) == NULL && len == 0);
    len = -1; CHECK(canvas2d_png_encode(buf, 1, 0, &len) == NULL && len == 0);
    len = -1; CHECK(canvas2d_png_encode(buf, 16385, 1, &len) == NULL && len == 0);
    len = -1; CHECK(canvas2d_png_encode(buf, 1, 16385, &len) == NULL && len == 0);
    free(buf);
}

static void encodes_valid_png(void) {
    uint16_t px[2 * 2 * 4] = {0};
    int len = 0;
    uint8_t *a = canvas2d_png_encode(px, 2, 2, &len);
    CHECK(a != NULL);
    CHECK(len > 8);
    if (a && len >= 8) {
        static uint8_t const sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        CHECK(memcmp(a, sig, 8) == 0);
    }
    // Deterministic: identical pixels -> identical bytes (the gate's premise).
    int len2 = 0;
    uint8_t *b = canvas2d_png_encode(px, 2, 2, &len2);
    CHECK(b != NULL && len2 == len);
    if (a && b && len == len2) {
        CHECK(memcmp(a, b, (size_t)len) == 0);
    }
    free(a);
    free(b);
}

int main(void) {
    rejects_bad_dimensions();
    encodes_valid_png();
    return TEST_REPORT();
}
