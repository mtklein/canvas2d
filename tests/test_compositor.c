#include "compositor.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>

// Premultiplied blend tile from a straight colour (channels in [0,1]).
static cnvs_premul *__counted_by(w * h) make_tile16(int w, int h,
                                                    float r, float g, float b, float a) {
    int n = w * h;
    cnvs_premul *t = malloc((size_t)n * sizeof *t);
    if (!t) {
        return NULL;
    }
    cnvs_premul p = cnvs_premultiply(cnvs_unpremul_of(r, g, b, a));
    for (int i = 0; i < n; i++) {
        t[i] = p;
    }
    return t;
}

// Read the premultiplied target back as straight RGBA8 (mirrors the canvas-side
// conversion), so the pixel checks can use straight values.
static void read8(compositor *__single c, int w, int h,
                  uint8_t *__counted_by(w * h * 4) out) {
    int const n = w * h;
    cnvs_premul *__counted_by(n) buf = malloc((size_t)n * sizeof *buf);
    if (!buf) {
        return;
    }
    compositor_read(c, buf, n);
    for (int i = 0; i < n; i++) {
        cnvs_unpremul s = cnvs_unpremultiply(buf[i]);
        out[i * 4]     = (uint8_t)((float)s.r * 255.0f + 0.5f);
        out[i * 4 + 1] = (uint8_t)((float)s.g * 255.0f + 0.5f);
        out[i * 4 + 2] = (uint8_t)((float)s.b * 255.0f + 0.5f);
        out[i * 4 + 3] = (uint8_t)((float)s.a * 255.0f + 0.5f);
    }
    free(buf);
}

// clearRect == destination-out of a unit-alpha tile.
static void clear_all(compositor *__single c, int w, int h) {
    cnvs_premul *t = make_tile16(w, h, 0.0f, 0.0f, 0.0f, 1.0f);
    if (t) {
        compositor_blend(c, 0, 0, w, h, t, COMPOSITOR_DST_OUT);
        free(t);
    }
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
    read8(c, w, h, px);
    CHECK(px_near(pixel_at(px, len, w, 0, 0), 0, 0, 0, 0, 0));

    // Blend an opaque red tile over a 4x4 region.
    cnvs_premul *red = make_tile16(4, 4, 1.0f, 0.0f, 0.0f, 1.0f);
    if (red) {
        compositor_blend(c, 2, 2, 4, 4, red, COMPOSITOR_SRC_OVER);
        read8(c, w, h, px);
        CHECK(px_near(pixel_at(px, len, w, 3, 3), 255, 0, 0, 255, 1));  // painted
        CHECK(px_near(pixel_at(px, len, w, 0, 0), 0, 0, 0, 0, 0));      // outside
        free(red);
    }

    // COPY overwrites the destination -- putImageData's compositing operator.
    cnvs_premul *blue = make_tile16(2, 2, 0.0f, 0.0f, 1.0f, 1.0f);
    if (blue) {
        compositor_blend(c, 3, 3, 2, 2, blue, COMPOSITOR_COPY);
        read8(c, w, h, px);
        CHECK(px_near(pixel_at(px, len, w, 3, 3), 0, 0, 255, 255, 1));  // overwritten
        CHECK(px_near(pixel_at(px, len, w, 2, 2), 255, 0, 0, 255, 1));  // still red
        free(blue);
    }

    // DESTINATION_OUT of a unit-alpha tile erases toward transparent (clearRect).
    clear_all(c, w, h);
    read8(c, w, h, px);
    CHECK(px_near(pixel_at(px, len, w, 3, 3), 0, 0, 0, 0, 0));

    // Translucent over transparent must read back STRAIGHT: a half-alpha red tile
    // is stored premultiplied (0.5,0,0,0.5) but un-premultiplies to (255,0,0,128).
    cnvs_premul *half_red = make_tile16(w, h, 1.0f, 0.0f, 0.0f, 0.5f);
    if (half_red) {
        compositor_blend(c, 0, 0, w, h, half_red, COMPOSITOR_SRC_OVER);
        read8(c, w, h, px);
        CHECK(px_near(pixel_at(px, len, w, 8, 8), 255, 0, 0, 128, 3));
        free(half_red);
    }
    clear_all(c, w, h);

    // Clip mask: left half open (255), right half closed (0).
    uint8_t *mask = malloc((size_t)(w * h));
    if (mask) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                mask[y * w + x] = x < 8 ? 255 : 0;
            }
        }
        compositor_set_clip(c, mask, w * h);
        cnvs_premul *green = make_tile16(w, h, 0.0f, 1.0f, 0.0f, 1.0f);
        if (green) {
            compositor_blend(c, 0, 0, w, h, green, COMPOSITOR_SRC_OVER);
            read8(c, w, h, px);
            CHECK(px_near(pixel_at(px, len, w, 4, 8), 0, 255, 0, 255, 1));  // clip open
            CHECK(px_near(pixel_at(px, len, w, 12, 8), 0, 0, 0, 0, 1));     // clip closed
            free(green);
        }
        free(mask);
    }

    // Re-open the clip and check source-over compositing of a half-alpha tile.
    compositor_set_clip(c, NULL, 0);
    clear_all(c, w, h);
    cnvs_premul *opaque_red = make_tile16(w, h, 1.0f, 0.0f, 0.0f, 1.0f);
    cnvs_premul *half_green = make_tile16(w, h, 0.0f, 1.0f, 0.0f, 0.5f);
    if (opaque_red && half_green) {
        compositor_blend(c, 0, 0, w, h, opaque_red, COMPOSITOR_SRC_OVER);
        compositor_blend(c, 0, 0, w, h, half_green, COMPOSITOR_SRC_OVER);
        read8(c, w, h, px);
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
