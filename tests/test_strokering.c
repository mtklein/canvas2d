#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>

// Stroking a full-circle arc (closed) must not leave a notch at the seam (theta=0,
// the +x point): a full-circle arc's last vertex nearly coincides with its first,
// and a microscopic closing segment used to corrupt the seam join.  The coverage
// at the seam should match any other point on the ring.
int main(void) {
    int const W = 160, len = W * W * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }
    struct canvas *__single cv = canvas(W, W);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    float const cx = 80.0f, cy = 80.0f, r = 46.0f, half_width = 7.5f;  // ring radius +/- half_width
    canvas_set_stroke_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_set_line_width(cv, 2.0f * half_width);
    canvas_begin_path(cv);
    canvas_arc(cv, cx, cy, r, 0.0f, 2.0f * (float)M_PI, false);
    canvas_close_path(cv);
    canvas_stroke(cv);
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);

    // A point on the ring centreline at several angles, including the seam at
    // theta=0; all should be fully covered (the notch left the seam partial).
    float const ang[4] = { 0.0f, (float)M_PI * 0.5f, (float)M_PI,
                           (float)M_PI * 1.5f };
    for (int i = 0; i < 4; i++) {
        int const x = (int)(cx + r * cosf(ang[i]));
        int y = (int)(cy + r * sinf(ang[i]));
        struct rgba p = pixel_at(px, len, W, x, y);
        CHECK(p.a > 250);  // ring centreline is solid everywhere, seam included
    }

    // And specifically the outer half of the ring at the seam (x just inside the
    // outer radius, on the y=80 centreline) -- this is exactly where the notch ate
    // the coverage back.
    CHECK(pixel_at(px, len, W, (int)(cx + r + half_width - 2.0f), 80).a > 250);

    canvas_free(cv);
    free(px);
    return TEST_REPORT();
}
