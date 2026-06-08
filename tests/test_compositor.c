#include "compositor.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>
#include <string.h>

// RGBA16F blend tile (channels in [0,1]).
static _Float16 *__counted_by(w * h * 4) make_tile16(int w, int h,
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

// RGBA8 replace tile (channels in 0..255).
static uint8_t *__counted_by(w * h * 4) make_tile8(int w, int h,
                                                   int r, int g, int b, int a) {
    int len = w * h * 4;
    uint8_t *t = malloc((size_t)len);
    if (!t) {
        return NULL;
    }
    for (int i = 0; i < w * h; i++) {
        t[i * 4] = (uint8_t)r;
        t[i * 4 + 1] = (uint8_t)g;
        t[i * 4 + 2] = (uint8_t)b;
        t[i * 4 + 3] = (uint8_t)a;
    }
    return t;
}

int main(void) {
    int const w = 16, h = 16, len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    compositor *__single c = compositor_create(w, h);
    CHECK(px != NULL);
    CHECK(c != NULL);
    if (!px || !c) {
        return TEST_REPORT();
    }

    // Fresh target is transparent.
    compositor_read_rgba(c, px, len);
    CHECK(px_near(pixel_at(px, len, w, 0, 0), 0, 0, 0, 0, 0));

    // Blend an opaque red tile over a 4x4 region.
    _Float16 *red = make_tile16(4, 4, 1.0f, 0.0f, 0.0f, 1.0f);
    if (red) {
        compositor_blend(c, 2, 2, 4, 4, red);
        compositor_read_rgba(c, px, len);
        CHECK(px_near(pixel_at(px, len, w, 3, 3), 255, 0, 0, 255, 1));  // painted
        CHECK(px_near(pixel_at(px, len, w, 0, 0), 0, 0, 0, 0, 0));      // outside
        free(red);
    }

    // replace overwrites (no blend) -- putImageData semantics.
    uint8_t *blue = make_tile8(2, 2, 0, 0, 255, 255);
    if (blue) {
        compositor_replace(c, 3, 3, 2, 2, blue);
        compositor_read_rgba(c, px, len);
        CHECK(px_near(pixel_at(px, len, w, 3, 3), 0, 0, 255, 255, 1));  // overwritten
        CHECK(px_near(pixel_at(px, len, w, 2, 2), 255, 0, 0, 255, 1));  // still red
        free(blue);
    }

    // clear erases toward transparent.
    compositor_clear(c, 0, 0, w, h);
    compositor_read_rgba(c, px, len);
    CHECK(px_near(pixel_at(px, len, w, 3, 3), 0, 0, 0, 0, 0));

    // Clip mask: left half open (255), right half closed (0).
    uint8_t *mask = malloc((size_t)(w * h));
    if (mask) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                mask[y * w + x] = x < 8 ? 255 : 0;
            }
        }
        compositor_set_clip(c, mask, w * h);
        _Float16 *green = make_tile16(w, h, 0.0f, 1.0f, 0.0f, 1.0f);
        if (green) {
            compositor_blend(c, 0, 0, w, h, green);
            compositor_read_rgba(c, px, len);
            CHECK(px_near(pixel_at(px, len, w, 4, 8), 0, 255, 0, 255, 1));  // clip open
            CHECK(px_near(pixel_at(px, len, w, 12, 8), 0, 0, 0, 0, 1));     // clip closed
            free(green);
        }
        free(mask);
    }

    // Re-open the clip and check source-over compositing of a half-alpha tile.
    compositor_set_clip(c, NULL, 0);
    compositor_clear(c, 0, 0, w, h);
    _Float16 *opaque_red = make_tile16(w, h, 1.0f, 0.0f, 0.0f, 1.0f);
    _Float16 *half_green = make_tile16(w, h, 0.0f, 1.0f, 0.0f, 0.5f);
    if (opaque_red && half_green) {
        compositor_blend(c, 0, 0, w, h, opaque_red);
        compositor_blend(c, 0, 0, w, h, half_green);
        compositor_read_rgba(c, px, len);
        struct px4 m = pixel_at(px, len, w, 8, 8);
        CHECK(m.r > 110 && m.r < 145);  // ~half red survives
        CHECK(m.g > 110 && m.g < 145);  // ~half green over
        CHECK(m.b < 8);
    }
    free(opaque_red);
    free(half_green);

    compositor_destroy(c);
    free(px);
    return TEST_REPORT();
}
