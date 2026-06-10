// Exercises the allocation-heavy, ownership-transfer paths so leaks/UAF surface:
// save/restore (deep-copies the clip mask), clip() (allocs+frees masks), the
// gradient ramp, get/put image data, draw_image, the font cache (create/destroy
// on size change), and many create/destroy cycles.
//
// Dual use:
//   - a normal test (run in every variant) -- under the debug variant's ASan it
//     checks use-after-free / use-after-scope / use-after-return on these paths;
//   - the target of `ninja leakcheck`, which runs the release build under the
//     macOS `leaks` tool (LeakSanitizer is broken on Apple-Silicon macOS, so
//     `leaks` is how we detect leaks here).

#include "test_util.h"

#include "canvas.h"

#include <ptrcheck.h>
#include <stdint.h>

static void draw_some(canvas *__single cv, int seed) {
    canvas_save(cv);
    canvas_translate(cv, (float)(seed % 7), (float)(seed % 5));
    canvas_rotate(cv, 0.1f * (float)seed);

    // Clip to a path (allocates a full-canvas mask; save() above snapshotted the
    // previous one, restore() below frees this and brings the old one back).
    canvas_begin_path(cv);
    canvas_rect(cv, 4.0f, 4.0f, 40.0f, 30.0f);
    canvas_clip(cv);

    // Gradient fill (builds the colour ramp), then a solid fill and a stroke.
    canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, 48.0f, 0.0f);
    canvas_add_fill_color_stop(cv, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_add_fill_color_stop(cv, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_begin_path(cv);
    canvas_arc(cv, 24.0f, 24.0f, 18.0f, 0.0f, 6.2831853f, false);
    canvas_fill(cv);

    canvas_set_stroke_rgba(cv, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas_set_line_width(cv, 2.0f);
    canvas_begin_path(cv);
    canvas_move_to(cv, 2.0f, 2.0f);
    canvas_bezier_curve_to(cv, 20.0f, 0.0f, 30.0f, 40.0f, 46.0f, 20.0f);
    canvas_stroke(cv);

    // Nested save/restore to stress the clip-mask stack ownership.
    canvas_save(cv);
    canvas_begin_path(cv);
    canvas_rect(cv, 10.0f, 10.0f, 10.0f, 10.0f);
    canvas_clip(cv);
    canvas_fill_rect(cv, 0.0f, 0.0f, 48.0f, 48.0f);
    canvas_restore(cv);

    canvas_restore(cv);
}

int main(void) {
    for (int i = 0; i < 16; i++) {
        canvas *__single cv = canvas_create(48, 48);
        CHECK(cv != NULL);
        if (!cv) {
            continue;
        }
        draw_some(cv, i);

        // Image data round-trip (get allocates, blit, free; put builds a tile).
        uint8_t buf[48 * 48 * 4];
        canvas_get_image_data(cv, 0, 0, 48, 48, buf, (int)sizeof buf);
        canvas_put_image_data(cv, buf, (int)sizeof buf, 24, 24, 4, 4);

        // drawImage source (its own bbox tile + premultiplied sampling path).
        uint8_t src[16 * 16 * 4];
        for (int k = 0; k < (int)sizeof src; k++) {
            src[k] = (uint8_t)(k * 7);
        }
        canvas_draw_image_scaled(cv, src, 16, 16, 2.0f, 2.0f, 40.0f, 40.0f);

        // Font cache: a size change frees the old face and builds a new one.
        canvas_set_font_size(cv, 12.0f + (float)i);
        (void)canvas_measure_text(cv, "Ag");

        // Emoji: the canonical capture and its lazily-built mip pyramid are
        // cache-owned heap buffers, freed with the canvas -- draw twice so the
        // hit path (no second allocation) runs under the leak gate too.
        canvas_fill_text(cv, "\xF0\x9F\x99\x82", 4.0f, 40.0f);  // 🙂
        canvas_fill_text(cv, "\xF0\x9F\x99\x82", 20.0f, 40.0f);

        canvas_destroy(cv);
    }
    return TEST_REPORT();
}
