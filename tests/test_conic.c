#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>

// A conic at (cx,cy): red @0, green @0.25, blue @0.5, white @0.75, red @1.
static void setup_conic(struct canvas *__single cv, float start, float cx, float cy) {
    canvas_set_fill_conic_gradient(cv, start, cx, cy);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.00f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.25f, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.50f, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.75f, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 1.00f, 1.0f, 0.0f, 0.0f, 1.0f);
}

static bool dom_red(struct rgba p)   { return p.r > 180 && p.g < 90 && p.b < 90; }
static bool dom_green(struct rgba p) { return p.g > 180 && p.r < 90 && p.b < 90; }
static bool dom_blue(struct rgba p)  { return p.b > 180 && p.r < 90 && p.g < 90; }
static bool dom_white(struct rgba p) { return p.r > 180 && p.g > 180 && p.b > 180; }

int main(void) {
    int const W = 32, len = W * W * 4;
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

    // No transform: t=0 (red) at +x, sweeping clockwise (canvas y is down) through
    // green (down), blue (left), white (up).
    canvas_reset_transform(cv);
    setup_conic(cv, 0.0f, 16.0f, 16.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_read_rgba(cv, px, len);
    CHECK(dom_red(pixel_at(px, len, W, 28, 16)));    // right
    CHECK(dom_green(pixel_at(px, len, W, 16, 28)));  // down
    CHECK(dom_blue(pixel_at(px, len, W, 4, 16)));    // left
    CHECK(dom_white(pixel_at(px, len, W, 16, 4)));   // up

    // The start angle rotates with the transform: a +pi/2 rotation (set via a
    // rotation matrix mapping the origin to the centre) carries red from +x round
    // to +y.  The gradient is baked into device space, so we reset the transform
    // before filling to cover the whole canvas.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_set_transform(cv, 0.0f, 1.0f, -1.0f, 0.0f, 16.0f, 16.0f);  // rotate +pi/2
    setup_conic(cv, 0.0f, 0.0f, 0.0f);   // centre -> device (16,16), angle += pi/2
    canvas_reset_transform(cv);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_read_rgba(cv, px, len);
    CHECK(dom_red(pixel_at(px, len, W, 16, 28)));    // red moved to "down"
    CHECK(dom_white(pixel_at(px, len, W, 28, 16)));  // "right" is now white

    canvas_free(cv);
    free(px);
    return TEST_REPORT();
}
