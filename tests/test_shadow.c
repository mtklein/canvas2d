#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>

#define W 48

int main(void) {
    int const len = W * W * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }
    struct canvas *__single cv = canvas_create(W, W);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    // Sharp (no blur) offset shadow: a red rect [8,24) casts a blue shadow at
    // +12,+12 -> [20,36).  The shadow sits under the shape.
    canvas_set_fill_rgba(cv, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_set_shadow_color_rgba(cv, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_set_shadow_offset_x(cv, 12.0f);
    canvas_set_shadow_offset_y(cv, 12.0f);
    canvas_set_shadow_blur(cv, 0.0f);
    canvas_fill_rect(cv, 8.0f, 8.0f, 16.0f, 16.0f);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, W, 12, 12), 255, 0, 0, 255, 2));  // shape only
    CHECK(px_near(pixel_at(px, len, W, 30, 30), 0, 0, 255, 255, 2));  // shadow only
    CHECK(px_near(pixel_at(px, len, W, 22, 22), 255, 0, 0, 255, 2));  // shape over shadow
    CHECK(px_near(pixel_at(px, len, W, 45, 45), 0, 0, 0, 0, 2));      // neither

    // Blur spreads the shadow beyond the shape: with zero offset a sharp shadow
    // would hide entirely under the shape, but a blurred one leaks past the edges.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_set_shadow_offset_x(cv, 0.0f);
    canvas_set_shadow_offset_y(cv, 0.0f);
    canvas_set_shadow_blur(cv, 16.0f);
    canvas_fill_rect(cv, 16.0f, 16.0f, 16.0f, 16.0f);  // shape [16,32)
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, W, 24, 24), 255, 0, 0, 255, 2));  // shape on top
    struct px4 leak = pixel_at(px, len, W, 36, 24);                   // 4px past the edge
    CHECK(leak.a > 20);          // blurred shadow leaked out
    CHECK(leak.b > leak.r);      // and it's the blue shadow, not red
    // The soft falloff must reach well past one box radius (blur 16 -> r 8): a
    // pixel 10px past the edge still carries shadow.  (The bug that clipped the
    // mask region to r left this at zero -- a hard rectangular cut-off.)
    CHECK(pixel_at(px, len, W, 42, 24).a > 5);

    // A transparent shadow colour casts no shadow even with an offset set.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_set_shadow_blur(cv, 0.0f);
    canvas_set_shadow_offset_x(cv, 12.0f);
    canvas_set_shadow_offset_y(cv, 12.0f);
    canvas_set_shadow_color_rgba(cv, 0.0f, 0.0f, 1.0f, 0.0f);  // transparent
    canvas_fill_rect(cv, 8.0f, 8.0f, 16.0f, 16.0f);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, W, 30, 30), 0, 0, 0, 0, 2));      // no shadow
    CHECK(px_near(pixel_at(px, len, W, 12, 12), 255, 0, 0, 255, 2));  // shape still there

    // The shadow colour's alpha carries through (a 50% blue shadow).
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_set_shadow_color_rgba(cv, 0.0f, 0.0f, 1.0f, 0.5f);
    canvas_fill_rect(cv, 8.0f, 8.0f, 16.0f, 16.0f);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, W, 30, 30), 0, 0, 255, 128, 4));  // 50% blue

    // Strokes cast shadows too.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_set_shadow_color_rgba(cv, 0.0f, 1.0f, 0.0f, 1.0f);  // green shadow
    canvas_set_shadow_offset_x(cv, 12.0f);
    canvas_set_shadow_offset_y(cv, 0.0f);
    canvas_set_stroke_rgba(cv, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_set_line_width(cv, 8.0f);
    canvas_begin_path(cv);
    canvas_move_to(cv, 8.0f, 24.0f);
    canvas_line_to(cv, 24.0f, 24.0f);  // stroke x[8,24], y[20,28]
    canvas_stroke(cv);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, W, 32, 24), 0, 255, 0, 255, 2));  // shadow only
    CHECK(px_near(pixel_at(px, len, W, 12, 24), 255, 0, 0, 255, 2));  // stroke only

    // drawImage casts a shadow too (from the destination-quad coverage): a 2x2
    // opaque red image scaled to [8,24) casts a blue shadow at +12,+12 -> [20,36).
    uint8_t img[16] = { 255, 0, 0, 255, 255, 0, 0, 255,
                        255, 0, 0, 255, 255, 0, 0, 255 };
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_set_shadow_color_rgba(cv, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_set_shadow_offset_x(cv, 12.0f);
    canvas_set_shadow_offset_y(cv, 12.0f);
    canvas_draw_image_scaled(cv, img, 2, 2, 8.0f, 8.0f, 16.0f, 16.0f);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, W, 12, 12), 255, 0, 0, 255, 2));  // image only
    CHECK(px_near(pixel_at(px, len, W, 30, 30), 0, 0, 255, 255, 2));  // shadow only
    CHECK(px_near(pixel_at(px, len, W, 45, 45), 0, 0, 0, 0, 2));      // neither

    canvas_destroy(cv);
    free(px);
    return TEST_REPORT();
}
