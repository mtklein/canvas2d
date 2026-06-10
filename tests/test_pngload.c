// canvas_write_png -> canvas_load_png round trips through the full pipeline:
// for each scene, the loaded RGBA must be byte-identical to canvas_read_rgba's
// view of the canvas that wrote it.  PNG (RGBA8, lossless) and the encoder's
// strict determinism make exact equality the right bar.  Then every committed
// gallery PNG must load -- the decoder eats the whole corpus our own encoder
// produced.

#include "canvas.h"
#include "test_util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Render -> write -> load -> compare for one already-drawn canvas.
static void round_trip(canvas *__single cv, int w, int h,
                       char const *__null_terminated path) {
    int const len = w * h * 4;
    uint8_t *__counted_by_or_null(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return;
    }
    canvas_read_rgba(cv, px, len);
    CHECK(canvas_write_png(cv, path));

    int lw = 0, lh = 0, llen = 0;
    uint8_t *back = canvas_load_png(path, &lw, &lh, &llen);
    CHECK(back != NULL && lw == w && lh == h && llen == len);
    if (back && llen == len) {
        CHECK(memcmp(back, px, (size_t)len) == 0);
    }
    free(back);
    free(px);
}

static void scene_solid(void) {
    int const w = 64, h = 48;
    canvas *__single cv = canvas_create(w, h);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas_set_fill_rgba(cv, 0.8f, 0.3f, 0.1f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    round_trip(cv, w, h, "build/test_pngload_solid.png");
    canvas_destroy(cv);
}

static void scene_gradient(void) {
    int const w = 120, h = 90;
    canvas *__single cv = canvas_create(w, h);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_add_fill_color_stop(cv, 0.0f, 0.1f, 0.2f, 0.9f, 1.0f);
    canvas_add_fill_color_stop(cv, 1.0f, 0.9f, 0.8f, 0.1f, 0.5f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    round_trip(cv, w, h, "build/test_pngload_gradient.png");
    canvas_destroy(cv);
}

static void scene_text(void) {
    int const w = 160, h = 60;
    canvas *__single cv = canvas_create(w, h);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas_set_fill_rgba(cv, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_rgba(cv, 0.1f, 0.1f, 0.4f, 1.0f);
    canvas_set_font_size(cv, 28.0f);
    canvas_fill_text(cv, "PNG \xE5\xBE\x80\xE8\xBF\x94", 8.0f, 40.0f);  // "PNG 往返"
    round_trip(cv, w, h, "build/test_pngload_text.png");
    canvas_destroy(cv);
}

static void scene_emoji(void) {
    int const w = 80, h = 80;
    canvas *__single cv = canvas_create(w, h);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas_set_fill_rgba(cv, 0.95f, 0.95f, 0.9f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_font_size(cv, 56.0f);
    canvas_fill_text(cv, "\xF0\x9F\x8C\x88", 12.0f, 64.0f);  // rainbow U+1F308
    round_trip(cv, w, h, "build/test_pngload_emoji.png");
    canvas_destroy(cv);
}

// Every committed gallery PNG loads with sane dimensions: the decoder accepts
// the entire corpus the encoder ships.
static void gallery_corpus(void) {
    DIR *d = opendir("gallery");
    CHECK(d != NULL);
    if (!d) {
        return;
    }
    int loaded = 0;
    for (struct dirent *e = readdir(d); e; e = readdir(d)) {
        // d_name is a fixed array from an un-annotated system header; measure
        // it once through the explicit unsafe seam, then work on the indexable
        // array view and build the path by hand (no snprintf: its header macro
        // trips -Wgnu-statement-expression).
        size_t n = strlen(__unsafe_null_terminated_from_indexable(e->d_name));
        if (n < 4 || n > 64 || memcmp(e->d_name + n - 4, ".png", 4) != 0) {
            continue;
        }
        char path[80] = "gallery/";
        memcpy(path + 8, e->d_name, n);
        path[8 + n] = '\0';
        int w = 0, h = 0, len = 0;
        uint8_t *px = canvas_load_png(__unsafe_null_terminated_from_indexable(path),
                                      &w, &h, &len);
        CHECK(px != NULL);
        CHECK(w > 0 && h > 0 && len == w * h * 4);
        if (!px) {
            (void)fprintf(stderr, "  failed: %s\n", path);
        }
        free(px);
        loaded += 1;
    }
    (void)closedir(d);
    CHECK(loaded >= 30);  // all of gallery/, not an accidentally empty dir
}

int main(void) {
    scene_solid();
    scene_gradient();
    scene_text();
    scene_emoji();
    gallery_corpus();
    return TEST_REPORT();
}
