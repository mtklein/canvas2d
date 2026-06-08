#include "compositor.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>

// RGBA16F tile (linear, channels in [0,1]); every op now speaks linear 16F.
static _Float16 *__counted_by(w * h * 4) make_tile(int w, int h,
                                                   float r, float g, float b, float a) {
    int len = w * h * 4;
    _Float16 *t = malloc((size_t)len * sizeof(_Float16));
    if (!t) {
        return NULL;
    }
    for (int i = 0; i < w * h; i++) {
        t[i * 4] = (_Float16)r;
        t[i * 4 + 1] = (_Float16)g;
        t[i * 4 + 2] = (_Float16)b;
        t[i * 4 + 3] = (_Float16)a;
    }
    return t;
}

static bool near16(_Float16 const *__counted_by(len) px, int len, int w,
                   int x, int y, float r, float g, float b, float a, float tol) {
    (void)len;
    int i = (y * w + x) * 4;
    return fabsf((float)px[i] - r) <= tol && fabsf((float)px[i + 1] - g) <= tol &&
           fabsf((float)px[i + 2] - b) <= tol && fabsf((float)px[i + 3] - a) <= tol;
}

int main(void) {
    int const w = 16, h = 16, n = w * h * 4;
    _Float16 *__counted_by(n) px = malloc((size_t)n * sizeof(_Float16));
    compositor *__single c = compositor_create(w, h);
    CHECK(px != NULL);
    CHECK(c != NULL);
    if (!px || !c) {
        return TEST_REPORT();
    }

    // Fresh target is transparent.
    compositor_read_f16(c, px, n);
    CHECK(near16(px, n, w, 0, 0, 0, 0, 0, 0, 0.001f));

    // Blend an opaque red tile over a 4x4 region.
    _Float16 *red = make_tile(4, 4, 1.0f, 0.0f, 0.0f, 1.0f);
    if (red) {
        compositor_blend(c, 2, 2, 4, 4, red);
        compositor_read_f16(c, px, n);
        CHECK(near16(px, n, w, 3, 3, 1.0f, 0.0f, 0.0f, 1.0f, 0.01f));  // painted
        CHECK(near16(px, n, w, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.001f)); // outside
        free(red);
    }

    // replace overwrites (no blend) -- putImageData semantics.
    _Float16 *blue = make_tile(2, 2, 0.0f, 0.0f, 1.0f, 1.0f);
    if (blue) {
        compositor_replace(c, 3, 3, 2, 2, blue);
        compositor_read_f16(c, px, n);
        CHECK(near16(px, n, w, 3, 3, 0.0f, 0.0f, 1.0f, 1.0f, 0.01f));  // overwritten
        CHECK(near16(px, n, w, 2, 2, 1.0f, 0.0f, 0.0f, 1.0f, 0.01f));  // still red
        free(blue);
    }

    // clear erases toward transparent.
    compositor_clear(c, 0, 0, w, h);
    compositor_read_f16(c, px, n);
    CHECK(near16(px, n, w, 3, 3, 0.0f, 0.0f, 0.0f, 0.0f, 0.001f));

    // Clip mask: left half open (255), right half closed (0).
    uint8_t *mask = malloc((size_t)(w * h));
    if (mask) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                mask[y * w + x] = x < 8 ? 255 : 0;
            }
        }
        compositor_set_clip(c, mask, w * h);
        _Float16 *green = make_tile(w, h, 0.0f, 1.0f, 0.0f, 1.0f);
        if (green) {
            compositor_blend(c, 0, 0, w, h, green);
            compositor_read_f16(c, px, n);
            CHECK(near16(px, n, w, 4, 8, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f));   // clip open
            CHECK(near16(px, n, w, 12, 8, 0.0f, 0.0f, 0.0f, 0.0f, 0.01f));  // clip closed
            free(green);
        }
        free(mask);
    }

    // Re-open the clip and check linear source-over of a half-alpha tile:
    // 0.5*green over opaque red = (0.5, 0.5, 0, 1) in linear light.
    compositor_set_clip(c, NULL, 0);
    compositor_clear(c, 0, 0, w, h);
    _Float16 *opaque_red = make_tile(w, h, 1.0f, 0.0f, 0.0f, 1.0f);
    _Float16 *half_green = make_tile(w, h, 0.0f, 1.0f, 0.0f, 0.5f);
    if (opaque_red && half_green) {
        compositor_blend(c, 0, 0, w, h, opaque_red);
        compositor_blend(c, 0, 0, w, h, half_green);
        compositor_read_f16(c, px, n);
        CHECK(near16(px, n, w, 8, 8, 0.5f, 0.5f, 0.0f, 1.0f, 0.02f));
    }
    free(opaque_red);
    free(half_green);

    compositor_destroy(c);
    free(px);
    return TEST_REPORT();
}
