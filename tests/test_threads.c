#include "canvas.h"
#include "test_util.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// The threaded-user harness (docs/rasterization.md 3.5): the library never
// creates a thread, but its callers will -- the classic design is N canvases
// over small cached tiles.  Emulate exactly that: one logical scene rendered
// as 256x256 tiles, each tile its OWN canvas (created, drawn, read, destroyed
// inside a worker), the scene translated per tile, workers pulling tiles off
// an atomic counter, rows stitched into one buffer.  The gate: the parallel
// stitch must byte-equal the same tiling rendered serially -- distinct
// canvases are fully independent, so any divergence is shared mutable state.
//
// Deliberately NOT gated: tiled-vs-whole-canvas equality.  Float translation
// is not bit-equivariant, so tile interiors may legitimately differ from a
// whole-canvas render; that comparison would test float algebra, not races.
//
// The scenes cross the interesting machinery: gradients (all three kinds),
// shadows, dashed strokes; a pattern fill plus Path2D fill/stroke/clip; and
// text -- Latin, Chinese, emoji -- whose per-canvas shape/glyph caches and
// Core Text boundary crossings run concurrently on distinct canvases.
//
// Workers never CHECK (test_util.h's g_test_fails is unsynchronized by
// design); each records into its own slot and main() checks after join.

// Interleaving rounds: every parallel round re-renders all tiles of all scenes
// and must reproduce the serial stitch.  ASan/TSan slow each render several-
// fold, so the sanitized builds run fewer rounds for the same gate.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SANITIZED 1
#endif
#endif

enum {
    TILE = 256,
    SCENE_W = 1024, SCENE_H = 768,                       // 4x3 = 12 tiles/scene
    COLS = SCENE_W / TILE, ROWS = SCENE_H / TILE, NTILES = COLS * ROWS,
    SCENE_LEN = SCENE_W * SCENE_H * 4,
    NSCENES = 3,
    NWORKERS = 8,
#if defined(SANITIZED)
    NROUNDS = 2,
#else
    NROUNDS = 6,
#endif
};

// ---------------------------------------------------------------------------
// Scenes, drawn in logical scene space ([0,SCENE_W) x [0,SCENE_H)).
// ---------------------------------------------------------------------------

static void scene_gradients(struct canvas *__single cv) {
    // Linear wash over the whole scene.
    canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, (float)SCENE_W, (float)SCENE_H);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 0.10f, 0.05f, 0.30f, 1.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.5f, 0.05f, 0.25f, 0.45f, 1.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 1.0f, 0.30f, 0.10f, 0.20f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)SCENE_W, (float)SCENE_H);

    // A grid of radial blobs, straddling tile seams on purpose.
    for (int i = 0; i < 12; i++) {
        float const cx = 128.0f + 256.0f * (float)(i % 4);
        float const cy = 128.0f + 256.0f * (float)(i / 4);
        canvas_set_fill_radial_gradient(cv, cx - 20.0f, cy - 20.0f, 8.0f,
                                        cx, cy, 110.0f);
        canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.9f, 0.4f, 1.0f);
        canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 1.0f, 0.8f, 0.2f, 0.4f, 0.0f);
        canvas_begin_path(cv);
        canvas_arc(cv, cx, cy, 110.0f, 0.0f, 6.2831853f, false);
        canvas_fill(cv, CANVAS_NONZERO);
    }

    // A conic-gradient pinwheel centred on a four-tile corner.
    canvas_set_fill_conic_gradient(cv, 0.5f, 512.0f, 256.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 0.2f, 0.8f, 0.9f, 0.9f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.5f, 0.9f, 0.3f, 0.8f, 0.9f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 1.0f, 0.2f, 0.8f, 0.9f, 0.9f);
    canvas_begin_path(cv);
    canvas_arc(cv, 512.0f, 256.0f, 150.0f, 0.0f, 6.2831853f, false);
    canvas_fill(cv, CANVAS_NONZERO);

    // Shadowed rounded rects.
    canvas_set_shadow_color_rgba(cv, CANVAS_CS_SRGB, 0.0f, 0.0f, 0.0f, 0.8f);
    canvas_set_shadow_blur(cv, 12.0f);
    canvas_set_shadow_offset_x(cv, 8.0f);
    canvas_set_shadow_offset_y(cv, 10.0f);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.95f, 0.85f, 0.30f, 1.0f);
    canvas_begin_path(cv);
    canvas_round_rect(cv, 180.0f, 430.0f, 300.0f, 200.0f, 28.0f);
    canvas_fill(cv, CANVAS_NONZERO);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.30f, 0.85f, 0.55f, 0.9f);
    canvas_begin_path(cv);
    canvas_round_rect(cv, 620.0f, 80.0f, 260.0f, 170.0f, 40.0f);
    canvas_fill(cv, CANVAS_NONZERO);
    canvas_set_shadow_color_rgba(cv, CANVAS_CS_SRGB, 0.0f, 0.0f, 0.0f, 0.0f);  // shadows off

    // A dashed gradient stroke weaving across every tile column.
    canvas_set_stroke_linear_gradient(cv, 0.0f, 600.0f, (float)SCENE_W, 700.0f);
    canvas_add_stroke_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_add_stroke_color_stop(cv, CANVAS_CS_SRGB, 1.0f, 1.0f, 0.4f, 0.1f, 1.0f);
    canvas_set_line_width(cv, 14.0f);
    canvas_set_line_cap(cv, CANVAS_CAP_ROUND);
    float const dash[2] = { 36.0f, 22.0f };
    canvas_set_line_dash(cv, dash, 2);
    canvas_begin_path(cv);
    canvas_move_to(cv, -20.0f, 620.0f);
    canvas_bezier_curve_to(cv, 300.0f, 500.0f, 700.0f, 760.0f, 1050.0f, 640.0f);
    canvas_stroke(cv);
}

static void scene_pattern_path2d(struct canvas *__single cv) {
    // Procedural 16x16 RGBA pattern source, built per render (per canvas: the
    // pattern source is borrowed, so each tile render owns its own copy).
    enum { PW = 16, PH = 16, PLEN = PW * PH * 4 };
    uint8_t pat[PLEN];
    for (int y = 0; y < PH; y++) {
        for (int x = 0; x < PW; x++) {
            int const i = (y * PW + x) * 4;
            bool const odd = ((x / 4) ^ (y / 4)) & 1;
            pat[i + 0] = odd ? 230 : 40;
            pat[i + 1] = (uint8_t)(40 + 12 * x);
            pat[i + 2] = odd ? 60 : 200;
            pat[i + 3] = 255;
        }
    }
    canvas_set_fill_pattern(cv, pat, PW, PH, CANVAS_REPEAT);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)SCENE_W, (float)SCENE_H);

    // A Path2D star spanning several tiles, filled even-odd then stroked.
    struct canvas_path2d *__single star = canvas_path2d();
    if (star) {
        for (int i = 0; i <= 10; i++) {
            float const r = (i & 1) ? 130.0f : 320.0f;
            float const a = 0.62832f * (float)i - 1.5708f;
            float const x = 512.0f + r * cosf(a);
            float const y = 384.0f + r * sinf(a);
            if (i == 0) {
                canvas_path2d_move_to(star, x, y);
            } else {
                canvas_path2d_line_to(star, x, y);
            }
        }
        canvas_path2d_close_path(star);
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.15f, 0.2f, 0.6f, 0.85f);
        canvas_fill_path(cv, star, CANVAS_EVENODD);
        canvas_set_stroke_rgba(cv, CANVAS_CS_SRGB, 1.0f, 1.0f, 1.0f, 1.0f);
        canvas_set_line_width(cv, 6.0f);
        canvas_set_line_join(cv, CANVAS_JOIN_ROUND);
        canvas_stroke_path(cv, star);
        canvas_path2d_free(star);
    }

    // A Path2D ring used as a clip; pattern-strokes inside it.
    struct canvas_path2d *__single ring = canvas_path2d();
    if (ring) {
        canvas_path2d_arc(ring, 260.0f, 200.0f, 150.0f, 0.0f, 6.2831853f, false);
        canvas_save(cv);
        canvas_clip_path(cv, ring, CANVAS_NONZERO);
        canvas_set_stroke_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.85f, 0.2f, 1.0f);
        canvas_set_line_width(cv, 10.0f);
        for (int i = 0; i < 6; i++) {
            canvas_begin_path(cv);
            canvas_move_to(cv, 60.0f, 60.0f + 50.0f * (float)i);
            canvas_line_to(cv, 460.0f, 110.0f + 50.0f * (float)i);
            canvas_stroke(cv);
        }
        canvas_restore(cv);
        canvas_path2d_free(ring);
    }
}

static void scene_text(struct canvas *__single cv) {
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.98f, 0.96f, 0.92f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)SCENE_W, (float)SCENE_H);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.1f, 0.1f, 0.15f, 1.0f);

    // Latin + Chinese rows at several sizes: concurrent shaping on distinct
    // canvases, same strings, so the per-canvas shape caches fill in parallel.
    char const *__null_terminated const lines[4] = {
        "The quick brown fox jumps over the lazy dog",
        "pack my box with five dozen liquor jugs 0123456789",
        ("\xE6\x9B\xB8\xE6\xB3\x95\xE7\x9A\x84\xE7\xBE\x8E"     // 書法的美
         " \xE9\x9A\xB8\xE6\x9B\xB8"),                          // 隸書
        "mixed \xE4\xB8\xAD\xE6\x96\x87 and Latin",
    };
    for (int row = 0; row < 8; row++) {
        float const size = 22.0f + 7.0f * (float)(row % 4);
        canvas_set_font_size(cv, size);
        canvas_fill_text(cv, lines[row % 4], 24.0f, 60.0f + 88.0f * (float)row);
    }

    // measure_text feeds a position (the measure path crosses the boundary too).
    canvas_set_font_size(cv, 30.0f);
    float const w = canvas_measure_text(cv, "measured");
    canvas_fill_text(cv, "measured", 980.0f - w, 60.0f);

    // Emoji: the colour-glyph capture path, straddling tile seams.
    canvas_set_font_size(cv, 64.0f);
    canvas_fill_text(cv, "\xF0\x9F\x8C\x88\xF0\x9F\x8E\xA8", 200.0f, 280.0f);  // 🌈🎨
    canvas_set_font_size(cv, 40.0f);
    canvas_fill_text(cv, "\xF0\x9F\xA6\x8A", 500.0f, 530.0f);                  // 🦊

    // Stroked text, centre-aligned across a vertical seam.
    canvas_set_stroke_rgba(cv, CANVAS_CS_SRGB, 0.6f, 0.1f, 0.3f, 1.0f);
    canvas_set_line_width(cv, 1.5f);
    canvas_set_font_size(cv, 48.0f);
    canvas_set_text_align(cv, CANVAS_ALIGN_CENTER);
    canvas_stroke_text(cv, "seamless", 512.0f, 740.0f);
    canvas_set_text_align(cv, CANVAS_ALIGN_START);
}

static void draw_scene(struct canvas *__single cv, int scene) {
    switch (scene) {
        case 0: scene_gradients(cv);     break;
        case 1: scene_pattern_path2d(cv); break;
        default: scene_text(cv);          break;
    }
}

// ---------------------------------------------------------------------------
// Tile rendering + stitching.
// ---------------------------------------------------------------------------

// Render one tile of `scene` on its own canvas and stitch its rows into out.
// Returns false on canvas/alloc failure (never on pixel mismatch -- comparing
// is main()'s job).
static bool render_tile(int scene, int tile, uint8_t *__counted_by(len) out, int len) {
    (void)len;
    int const tx = TILE * (tile % COLS);
    int const ty = TILE * (tile / COLS);
    struct canvas *__single cv = canvas(TILE, TILE);
    if (!cv) {
        return false;
    }
    canvas_translate(cv, (float)-tx, (float)-ty);
    draw_scene(cv, scene);

    int const tlen = TILE * TILE * 4;
    uint8_t *__counted_by(tlen) px = malloc((size_t)tlen);
    bool const ok = px != NULL;
    if (ok) {
        canvas_read_rgba(cv, px, tlen);
        for (int row = 0; row < TILE; row++) {
            memcpy(out + ((ty + row) * SCENE_W + tx) * 4,
                   px + row * TILE * 4, TILE * 4);
        }
        free(px);
    }
    canvas_free(cv);
    return ok;
}

// One round's shared work queue: workers pull (scene, tile) pairs off `next`
// until all NSCENES * NTILES are done.  The counter is the only shared
// mutable word, behind its mutex; the stitched buffers' tile regions are
// disjoint, so workers write them unsynchronized.
struct queue {
    pthread_mutex_t lock;
    int next;
    uint8_t *__counted_by(NSCENES * SCENE_LEN) out;  // NSCENES stitched scenes
};

static int take(struct queue *__single q) {
    pthread_mutex_lock(&q->lock);
    int const job = q->next++;
    pthread_mutex_unlock(&q->lock);
    return job;
}

struct worker {
    struct queue *__single q;
    bool ok;       // worker-private; main() CHECKs it after join
};

static void *worker_main(void *arg) {
    struct worker *__single w = arg;
    struct queue *__single q = w->q;
    for (;;) {
        int const job = take(q);
        if (job >= NSCENES * NTILES) {
            return NULL;
        }
        int const scene = job / NTILES;
        uint8_t *__counted_by(SCENE_LEN) dst = q->out + scene * SCENE_LEN;
        if (!render_tile(scene, job % NTILES, dst, SCENE_LEN)) {
            w->ok = false;
        }
    }
}

int main(void) {
    int const all = NSCENES * SCENE_LEN;
    uint8_t *__counted_by(all) want = malloc((size_t)all);
    uint8_t *__counted_by(all) got = malloc((size_t)all);
    CHECK(want != NULL);
    CHECK(got != NULL);
    if (!want || !got) {
        free(want);
        free(got);
        return TEST_REPORT();
    }

    // The serial reference: same tiling, one tile at a time.
    for (int scene = 0; scene < NSCENES; scene++) {
        for (int tile = 0; tile < NTILES; tile++) {
            uint8_t *__counted_by(SCENE_LEN) dst = want + scene * SCENE_LEN;
            CHECK(render_tile(scene, tile, dst, SCENE_LEN));
        }
    }

    // Parallel rounds: every round must reproduce the serial stitch exactly.
    for (int round = 0; round < NROUNDS; round++) {
        struct queue q = { .lock = PTHREAD_MUTEX_INITIALIZER, .next = 0,
                           .out = got };
        struct worker w[NWORKERS];
        pthread_t t[NWORKERS];
        int spawned = 0;
        for (int i = 0; i < NWORKERS; i++) {
            w[i] = (struct worker){ .q = &q, .ok = true };
            // worker_main's checked `void *__single` parameters are plain
            // pointers at the ABI; the -strict variant of this warning flags
            // any checked function handed to an unannotated system callback
            // slot, which is exactly this seam and only this seam.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wincompatible-function-pointer-types-strict"
            int const rc = pthread_create(&t[i], NULL, worker_main, &w[i]);
#pragma clang diagnostic pop
            if (rc != 0) {
                break;
            }
            spawned++;
        }
        CHECK(spawned == NWORKERS);
        for (int i = 0; i < spawned; i++) {
            CHECK(pthread_join(t[i], NULL) == 0);
            CHECK(w[i].ok);
        }
        CHECK(memcmp(want, got, (size_t)all) == 0);
        memset(got, 0, (size_t)all);  // a stale round can't mask the next
    }

    free(want);
    free(got);
    return TEST_REPORT();
}
