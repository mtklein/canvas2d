// Dimension validation: canvas caps dimensions, and the image entry
// points reject caller-supplied (w,h)/(sw,sh) whose RGBA8 byte size would
// overflow `int`.  Before the fix, `w * h * 4` overflowed signed int in these
// guards, defeating them; under the debug variant's UBSan this whole test
// aborts if any guard regresses (the overflow is fatal there).  See
// docs/decisions/security-review.md, Finding 1.

#include "test_util.h"

#include "canvas.h"

#include <ptrcheck.h>
#include <string.h>

// w * h such that w*h*4 overflows a 32-bit int (23171^2 = 536,887,241 > INT_MAX/4).
#define OVF 23171

int main(void) {
    // canvas bounds: rejects non-positive and oversized, accepts in-range.
    CHECK(canvas(0, 10, CANVAS_CS_SRGB) == NULL);
    CHECK(canvas(10, -1, CANVAS_CS_SRGB) == NULL);
    CHECK(canvas(16385, 1, CANVAS_CS_SRGB) == NULL);
    CHECK(canvas(1, 16385, CANVAS_CS_SRGB) == NULL);
    CHECK(canvas(OVF, OVF, CANVAS_CS_SRGB) == NULL);

    struct canvas *__single cv = canvas(16384, 1, CANVAS_CS_SRGB);  // boundary value is accepted
    CHECK(cv != NULL);
    canvas_free(cv);

    cv = canvas(8, 8, CANVAS_CS_SRGB);
    CHECK(cv != NULL);

    // get_image_data: an overflowing region must be rejected *before* the memset
    // that clears `out`, so the buffer is left untouched.
    uint8_t buf[256];
    memset(buf, 0xAB, sizeof buf);
    canvas_get_image_data(cv, CANVAS_CS_SRGB, 0, 0, OVF, OVF, buf, (int)sizeof buf);
    CHECK(buf[0] == 0xAB && buf[255] == 0xAB);

    // A genuinely-too-small buffer for a sane region is still rejected.
    canvas_get_image_data(cv, CANVAS_CS_SRGB, 0, 0, 8, 8, buf, 4);
    CHECK(buf[0] == 0xAB);

    // put_image_data with overflowing source dims must return without tripping
    // the size arithmetic -- here (w,h) are plain-int params distinct from the
    // honest buffer length, so the internal rgba8_dims_ok guard is what rejects
    // it (reaching the next line at all under UBSan is the assertion).
    // draw_image shares that guard but its buffer count *is* sw*sh*4, so a lying
    // buffer would trip the call-boundary check first; it is covered transitively.
    uint8_t src[4] = { 1, 2, 3, 4 };
    canvas_put_image_data(cv, CANVAS_CS_SRGB, src, (int)sizeof src, OVF, OVF, 0, 0);
    CHECK(cv != NULL);

    canvas_free(cv);
    return TEST_REPORT();
}
