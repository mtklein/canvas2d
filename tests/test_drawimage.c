#include "canvas2d.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>

int main(void) {
    int const w = 8;
    int const h = 8;
    int const len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }

    // 2x2 source: red, green / blue, yellow.
    uint8_t src[16] = {
        255, 0, 0, 255,   0, 255, 0, 255,
        0, 0, 255, 255,   255, 255, 0, 255,
    };

    struct canvas2d_context *__single cv = canvas2d(w, h, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    // 1:1 draw at (1,1) reproduces the source texels exactly (bilinear is
    // identity at integer scale and aligned pixel centres).
    canvas2d_draw_bitmap(cv, CANVAS2D_CS_SRGB, src, 2, 2, 1.0f, 1.0f);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, w, 1, 1), 255, 0, 0, 255, 2));      // red
    CHECK(px_near(pixel_at(px, len, w, 2, 1), 0, 255, 0, 255, 2));      // green
    CHECK(px_near(pixel_at(px, len, w, 1, 2), 0, 0, 255, 255, 2));      // blue
    CHECK(px_near(pixel_at(px, len, w, 2, 2), 255, 255, 0, 255, 2));    // yellow
    CHECK(px_near(pixel_at(px, len, w, 5, 5), 0, 0, 0, 0, 1));          // untouched

    // Scale 2x2 -> 8x8.  Corners clamp to the source corners; a top-edge midpoint
    // is a horizontal red<->green bilinear blend.
    canvas2d_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_draw_bitmap_scaled(cv, CANVAS2D_CS_SRGB, src, 2, 2, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, w, 0, 0), 255, 0, 0, 255, 4));      // red corner
    CHECK(px_near(pixel_at(px, len, w, 7, 0), 0, 255, 0, 255, 4));      // green corner
    CHECK(px_near(pixel_at(px, len, w, 0, 7), 0, 0, 255, 255, 4));      // blue corner
    CHECK(px_near(pixel_at(px, len, w, 7, 7), 255, 255, 0, 255, 4));    // yellow corner
    struct rgba edge = pixel_at(px, len, w, 4, 0);
    CHECK(edge.r > 30 && edge.g > 30 && edge.b < 20);                   // red+green mix

    // Source subrect: right column only (green/yellow) -> centre has no red/blue.
    canvas2d_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_draw_bitmap_subrect(cv, CANVAS2D_CS_SRGB, src, 2, 2, 1.0f, 0.0f, 1.0f, 2.0f,
                              0.0f, 0.0f, (float)w, (float)h);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    struct rgba mid = pixel_at(px, len, w, 4, 4);
    CHECK(mid.g > 200 && mid.b < 30);                                  // green/yellow

    // global alpha composites the image source-over: 50% opaque blue over red.
    canvas2d_clear_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    uint8_t blue[16] = {
        0, 0, 255, 255,  0, 0, 255, 255,
        0, 0, 255, 255,  0, 0, 255, 255,
    };
    canvas2d_set_global_alpha(cv, 0.5f);
    canvas2d_draw_bitmap_scaled(cv, CANVAS2D_CS_SRGB, blue, 2, 2, 0.0f, 0.0f, (float)w, (float)h);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
    CHECK(px_near(pixel_at(px, len, w, 4, 4), 128, 0, 128, 255, 4));    // half blue over red

    // A huge source rect maps device pixels to source coordinates that
    // saturate canvas2d_f2i to INT_MAX; the bilinear sampler's second tap (x0+1)
    // must not overflow past it (fuzz_replay found the signed wrap; the
    // sanitized debug run of this case is the regression).  Just must not
    // trap -- the draw itself samples a clamped edge texel.
    canvas2d_set_global_alpha(cv, 1.0f);
    canvas2d_draw_bitmap_subrect(cv, CANVAS2D_CS_SRGB, src, 2, 2, 0.0f, 0.0f, 1.0f, 1e30f,
                              0.0f, 3.0f, 3.0f, 1.0f);

    canvas2d_free(cv);
    free(px);
    return TEST_REPORT();
}
