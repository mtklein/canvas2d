// Out-of-memory fault-injection gate.  The core is compiled with
// -Dmalloc=cnvs_oom_malloc (and realloc/calloc), so cnvs_oom_fail_at(k) makes the
// k-th allocation of an operation return NULL.  For each of a few canvas
// operations we first count its allocations, then re-run it once per allocation
// with that one failing.  Every allocation-failure cleanup path must degrade
// gracefully: the operation may draw nothing, but it must not crash, leak into a
// bad state, or corrupt memory -- which -fbounds-safety + ASan/UBSan enforce (a
// use-after-free or OOB in an error path aborts the test).  This reaches the
// `if (!p) return false` OOM guards that the normal suite leaves untaken.

#include "canvas.h"
#include "oom_alloc.h"
#include "test_util.h"

#include <stdint.h>

enum { W = 32, H = 32 };

typedef void (*scene_fn)(struct canvas *__single cv);

static void scene_fill(struct canvas *__single cv) {
    canvas_begin_path(cv);
    canvas_move_to(cv, 4.0f, 4.0f);
    canvas_line_to(cv, 28.0f, 6.0f);
    canvas_line_to(cv, 16.0f, 28.0f);
    canvas_close_path(cv);
    canvas_set_fill_rgba(cv, 0.8f, 0.3f, 0.2f, 1.0f);
    canvas_fill(cv);
}

static void scene_curve(struct canvas *__single cv) {
    canvas_begin_path(cv);
    canvas_move_to(cv, 2.0f, 16.0f);
    canvas_bezier_curve_to(cv, 10.0f, 0.0f, 22.0f, 32.0f, 30.0f, 16.0f);
    canvas_set_fill_rgba(cv, 0.3f, 0.7f, 0.4f, 1.0f);
    canvas_fill(cv);
}

static void scene_gradient(struct canvas *__single cv) {
    canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_add_fill_color_stop(cv, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_add_fill_color_stop(cv, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
}

static void scene_stroke(struct canvas *__single cv) {
    canvas_begin_path(cv);
    canvas_move_to(cv, 2.0f, 2.0f);
    canvas_line_to(cv, 30.0f, 8.0f);
    canvas_line_to(cv, 10.0f, 30.0f);
    canvas_set_line_width(cv, 3.0f);
    canvas_set_stroke_rgba(cv, 0.2f, 0.6f, 0.9f, 1.0f);
    canvas_stroke(cv);
}

static void scene_image(struct canvas *__single cv) {
    uint8_t buf[16 * 16 * 4];
    canvas_get_image_data(cv, 0, 0, 16, 16, buf, (int)sizeof buf);
    canvas_put_image_data(cv, buf, (int)sizeof buf, 16, 16, 4, 4);
    uint8_t src[8 * 8 * 4];
    for (int i = 0; i < (int)sizeof src; i++) {
        src[i] = (uint8_t)(i * 7);
    }
    canvas_draw_image_scaled(cv, src, 8, 8, 2.0f, 2.0f, 24.0f, 24.0f);
}

// Clipping: build a clip mask, then paint through it (exercises the canvas's
// clip-mask allocation and the clip stack).
static void scene_clip(struct canvas *__single cv) {
    canvas_save(cv);
    canvas_begin_path(cv);
    canvas_arc(cv, 16.0f, 16.0f, 12.0f, 0.0f, 6.2831853f, false);
    canvas_clip(cv);
    canvas_set_fill_rgba(cv, 0.9f, 0.6f, 0.2f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_restore(cv);
}

// Text: glyph outlines from the Core Text shim, built into a path and filled --
// the font cache + glyph-path allocation path, and the text lookup cache's own
// allocation sites (the shape-key copy, the glyph table, the per-glyph curve
// copies, the interned font name, the emoji capture buffer and its mip
// pyramid).  Each must degrade to a plain boundary call (or a skipped op),
// never a crash: the repeat exercises the hit path -- and, when an earlier
// insert was the failed allocation, the miss-then-insert retry.  A failed
// capture allocation falls back to the per-draw boundary render (whose own
// buffer may then fail too -> a blank glyph); a failed pyramid level samples
// the coarsest level that did build, worst case the capture itself.
static void scene_text(struct canvas *__single cv) {
    canvas_set_font_size(cv, 14.0f);
    canvas_set_fill_rgba(cv, 0.9f, 0.9f, 0.95f, 1.0f);
    canvas_fill_text(cv, "Ag", 2.0f, 20.0f);
    canvas_fill_text(cv, "Ag", 2.0f, 20.0f);     // warm: shape + glyph hits
    (void)canvas_measure_text(cv, "Ag");         // measure-then-draw shares the line
    canvas_fill_text(cv, "g A", 2.0f, 30.0f);    // new key; shared glyphs, a blank
    canvas_fill_text(cv, "\xF0\x9F\x99\x82", 2.0f, 30.0f);  // 🙂: capture + mips
    canvas_fill_text(cv, "\xF0\x9F\x99\x82", 12.0f, 30.0f); // warm capture hit
}

// Image pattern fill (createPattern path): a small tile sampled under the CTM.
static void scene_pattern(struct canvas *__single cv) {
    uint8_t tile[4 * 4 * 4];
    for (int i = 0; i < (int)sizeof tile; i++) {
        tile[i] = (uint8_t)(i * 11);
    }
    canvas_set_fill_pattern(cv, tile, 4, 4, CANVAS_REPEAT);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
}

// Dashed stroke: the dash pattern is copied into the canvas state, then stroking
// walks it.
static void scene_dash(struct canvas *__single cv) {
    float const dash[4] = { 5.0f, 3.0f, 2.0f, 3.0f };
    canvas_set_line_dash(cv, dash, 4);
    canvas_set_line_width(cv, 2.0f);
    canvas_set_stroke_rgba(cv, 0.8f, 0.8f, 0.3f, 1.0f);
    canvas_begin_path(cv);
    canvas_move_to(cv, 2.0f, 16.0f);
    canvas_line_to(cv, 30.0f, 16.0f);
    canvas_stroke(cv);
}

// save/restore nesting grows the state stack.
static void scene_savestack(struct canvas *__single cv) {
    for (int i = 0; i < 12; i++) {
        canvas_save(cv);
        canvas_translate(cv, 1.0f, 1.0f);
        canvas_set_global_alpha(cv, 0.9f);
    }
    for (int i = 0; i < 12; i++) {
        canvas_restore(cv);
    }
}

// CSS filter list: the per-state dynamic array (append, save's deep copy,
// restore/set_filter_none's frees), the per-tile apply, and the spatial
// entries' scratch tile -- blur()'s ping-pong half and drop-shadow()'s
// two-tile growth (whose failed allocation must skip that entry, not the op).
static void scene_filter(struct canvas *__single cv) {
    canvas_add_filter_grayscale(cv, 1.0f);
    canvas_add_filter_brightness(cv, 1.2f);
    canvas_save(cv);
    canvas_add_filter_invert(cv, 0.5f);
    canvas_add_filter_blur(cv, 2.0f);
    canvas_add_filter_drop_shadow(cv, 2.0f, 2.0f, 1.0f, 0.1f, 0.2f, 0.3f, 0.8f);
    canvas_set_fill_rgba(cv, 0.9f, 0.4f, 0.2f, 0.8f);
    canvas_fill_rect(cv, 4.0f, 4.0f, 24.0f, 24.0f);
    canvas_restore(cv);
    canvas_set_filter_none(cv);
}

// isPointInPath flattens the current path to test containment.
static void scene_pointinpath(struct canvas *__single cv) {
    canvas_begin_path(cv);
    canvas_arc(cv, 16.0f, 16.0f, 10.0f, 0.0f, 6.2831853f, false);
    (void)canvas_is_point_in_path(cv, 16.0f, 16.0f, CANVAS_NONZERO);
    (void)canvas_is_point_in_path(cv, 0.0f, 0.0f, CANVAS_NONZERO);
}

// PNG encode: exercises cnvs_png's raw/zlib buffer allocations (two of the sites
// fixed alongside this harness).  Output is discarded.
static void scene_png(struct canvas *__single cv) {
    canvas_set_fill_rgba(cv, 0.3f, 0.5f, 0.7f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    (void)canvas_write_png(cv, "/dev/null");
}

// Run `fn` with each of its allocations failing in turn; it must never crash, and
// the canvas must stay usable afterwards.
static void sweep(scene_fn fn) {
    struct canvas *__single probe = canvas_create(W, H);
    CHECK(probe != NULL);
    if (!probe) {
        return;
    }
    cnvs_oom_fail_at(0);
    fn(probe);
    int allocs = cnvs_oom_seen();
    canvas_destroy(probe);
    CHECK(allocs > 0);  // the scene must allocate, or the sweep tests nothing

    for (int k = 1; k <= allocs; k++) {
        struct canvas *__single cv = canvas_create(W, H);  // fresh -> stable alloc sequence
        if (!cv) {
            continue;
        }
        cnvs_oom_fail_at(k);
        fn(cv);                                      // k-th allocation fails
        cnvs_oom_fail_at(0);
        canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)H);  // still usable
        canvas_destroy(cv);
    }
}

int main(void) {
    sweep(scene_fill);
    sweep(scene_curve);
    sweep(scene_gradient);
    sweep(scene_stroke);
    sweep(scene_image);
    sweep(scene_clip);
    sweep(scene_text);
    sweep(scene_pattern);
    sweep(scene_dash);
    sweep(scene_filter);
    sweep(scene_savestack);
    sweep(scene_pointinpath);
    sweep(scene_png);

    // canvas_create must itself fail to NULL under a single allocation failure
    // (rather than return a half-built canvas), for every alloc it makes.
    for (int k = 1; k <= 12; k++) {
        cnvs_oom_fail_at(k);
        struct canvas *__single cv = canvas_create(W, H);
        cnvs_oom_fail_at(0);
        if (cv) {
            canvas_destroy(cv);  // k past create's alloc count -> a real canvas
        }
    }

    // Sanity: with the injector disarmed everything still works end to end.
    cnvs_oom_fail_at(0);
    struct canvas *__single cv = canvas_create(W, H);
    CHECK(cv != NULL);
    if (cv) {
        canvas_set_fill_rgba(cv, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
        uint8_t px[4];
        canvas_get_image_data(cv, W / 2, H / 2, 1, 1, px, 4);
        CHECK(px[0] > 250 && px[1] < 5 && px[2] < 5 && px[3] == 255);
        canvas_destroy(cv);
    }
    return TEST_REPORT();
}
