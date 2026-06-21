// Exercises the allocation-heavy, ownership-transfer paths so leaks/UAF surface:
// save/restore (deep-copies the clip mask), clip() (allocs+frees masks), the
// gradient row buffers, get/put image data, draw_image, the font cache (create/destroy
// on size change), and many create/destroy cycles.
//
// Dual use:
//   - a normal test (run in every variant) -- under the debug variant's ASan it
//     checks use-after-free / use-after-scope / use-after-return on these paths;
//   - the target of `ninja leakcheck`, which runs the release build under the
//     macOS `leaks` tool (LeakSanitizer is broken on Apple-Silicon macOS, so
//     `leaks` is how we detect leaks here).

#include "test_util.h"

#include "canvas2d.h"
#include "canvas2d_path2d.h"
#include "canvas2d_replay.h"

#include <ptrcheck.h>
#include <stdint.h>

static void draw_some(struct canvas2d_context *__single cv, int seed) {
    canvas2d_save(cv);
    canvas2d_translate(cv, (float)(seed % 7), (float)(seed % 5));
    canvas2d_rotate(cv, 0.1f * (float)seed);

    // Clip to a path (allocates a full-canvas mask; save() above snapshotted the
    // previous one, restore() below frees this and brings the old one back).
    canvas2d_begin_path(cv);
    canvas2d_rect(cv, 4.0f, 4.0f, 40.0f, 30.0f);
    canvas2d_clip(cv, CANVAS2D_NONZERO);

    // Gradient fill (grows the parameter/colour row buffers), then a solid
    // fill and a stroke.
    canvas2d_set_fill_linear_gradient(cv, CANVAS2D_CS_SRGB, CANVAS2D_ALPHA_UNPREMUL, 0.0f, 0.0f, 48.0f, 0.0f);
    canvas2d_add_fill_color_stop(cv, CANVAS2D_CS_SRGB, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_add_fill_color_stop(cv, CANVAS2D_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas2d_begin_path(cv);
    canvas2d_arc(cv, 24.0f, 24.0f, 18.0f, 0.0f, 6.2831853f, false);
    canvas2d_fill(cv, CANVAS2D_NONZERO);

    canvas2d_set_stroke_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_set_line_width(cv, 2.0f);
    canvas2d_begin_path(cv);
    canvas2d_move_to(cv, 2.0f, 2.0f);
    canvas2d_bezier_curve_to(cv, 20.0f, 0.0f, 30.0f, 40.0f, 46.0f, 20.0f);
    canvas2d_stroke(cv);

    // Nested save/restore to stress the clip-mask stack ownership.
    canvas2d_save(cv);
    canvas2d_begin_path(cv);
    canvas2d_rect(cv, 10.0f, 10.0f, 10.0f, 10.0f);
    canvas2d_clip(cv, CANVAS2D_NONZERO);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, 48.0f, 48.0f);
    canvas2d_restore(cv);

    canvas2d_restore(cv);
}

int main(void) {
    for (int i = 0; i < 16; i++) {
        struct canvas2d_context *__single cv = canvas2d(48, 48, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            continue;
        }
        draw_some(cv, i);

        // Image data round-trip (get allocates, blit, free; put builds a tile).
        uint8_t buf[48 * 48 * 4];
        canvas2d_get_image_data(cv, CANVAS2D_CS_SRGB, 0, 0, 48, 48, buf, (int)sizeof buf);
        canvas2d_put_image_data(cv, CANVAS2D_CS_SRGB, buf, (int)sizeof buf, 24, 24, 4, 4);

        // drawImage source (its own bbox tile + premultiplied sampling path).
        uint8_t src[16 * 16 * 4];
        for (int k = 0; k < (int)sizeof src; k++) {
            src[k] = (uint8_t)(k * 7);
        }
        canvas2d_draw_bitmap_scaled(cv, CANVAS2D_CS_SRGB, src, 16, 16, 2.0f, 2.0f, 40.0f, 40.0f);

        // Font cache: a size change frees the old face and builds a new one.
        canvas2d_set_font_size(cv, 12.0f + (float)i);
        (void)canvas2d_measure_text(cv, "Ag");

        // Emoji: the canonical capture and its lazily-built mip pyramid are
        // cache-owned heap buffers, freed with the canvas -- draw twice so the
        // hit path (no second allocation) runs under the leak gate too.
        canvas2d_fill_text(cv, "\xF0\x9F\x99\x82", 4.0f, 40.0f);  // 🙂
        canvas2d_fill_text(cv, "\xF0\x9F\x99\x82", 20.0f, 40.0f);

        canvas2d_free(cv);
    }

    // Record/replay ownership: the recorder's content-dedupe copies (one per
    // distinct image and path block) free when the recording closes; a
    // replayed program's image blocks are ADOPTED by the canvas (patterns
    // borrow them past replay) and free at destroy; the parser's rebuilt
    // Path2D objects free when replay returns -- including via the
    // truncated-block drop paths, which a failing replay exercises.
    for (int i = 0; i < 4; i++) {
        char const *__null_terminated path = "build/test_leak.canvas";
        struct canvas2d_context *__single cv = canvas2d(48, 48, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            continue;
        }
        CHECK(canvas2d_record_to(cv, path));
        uint8_t src[8 * 8 * 4];
        for (int k = 0; k < (int)sizeof src; k++) {
            src[k] = (uint8_t)(k * 3);
        }
        canvas2d_draw_bitmap(cv, CANVAS2D_CS_SRGB, src, 8, 8, 2.0f, 2.0f);
        canvas2d_set_fill_pattern(cv, CANVAS2D_CS_SRGB, src, 8, 8, CANVAS2D_REPEAT);
        canvas2d_fill_rect(cv, 0.0f, 0.0f, 24.0f, 24.0f);
        struct canvas2d_path2d *__single p2 = canvas2d_path2d();
        if (p2) {
            canvas2d_path2d_rect(p2, 4.0f, 4.0f, 20.0f, 20.0f);
            canvas2d_fill_path(cv, p2, CANVAS2D_NONZERO);
            canvas2d_stroke_path(cv, p2);  // dedupe hit: no second copy
            canvas2d_path2d_free(p2);
        }
        canvas2d_fill_text(cv, "Ag", 4.0f, 40.0f);
        canvas2d_free(cv);  // closes the recording, freeing its copies

        struct canvas2d_context *__single rv = canvas2d(48, 48, CANVAS2D_CS_SRGB);
        CHECK(rv != NULL);
        if (!rv) {
            continue;
        }
        CHECK(canvas2d_replay_from(rv, path));
        // Truncated blocks: the pending path and pending image state must
        // free on the failure path too.
        static char const trunc_path[] = "path 0 2\nm 1 2\n";
        CHECK(!canvas2d_replay_text(rv, trunc_path, sizeof trunc_path - 1));
        static char const trunc_image[] = "image 0 1 1 12 1\n";
        CHECK(!canvas2d_replay_text(rv, trunc_image, sizeof trunc_image - 1));
        canvas2d_free(rv);  // frees the adopted image blocks
    }
    return TEST_REPORT();
}
