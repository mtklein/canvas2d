// canvas_write_png -> canvas_read_png round trips through the full pipeline:
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
static void round_trip(struct canvas *__single cv, int w, int h,
                       char const *__null_terminated path) {
    int const len = w * h * 4;
    uint8_t *__counted_by_or_null(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return;
    }
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    CHECK(canvas_write_png(cv, path));

    int lw = 0, lh = 0, llen = 0;
    uint8_t *back = canvas_read_png(path, &lw, &lh, &llen);
    CHECK(back != NULL && lw == w && lh == h && llen == len);
    if (back && llen == len) {
        CHECK(memcmp(back, px, (size_t)len) == 0);
    }
    free(back);
    free(px);
}

static void scene_solid(void) {
    int const w = 64, h = 48;
    struct canvas *__single cv = canvas(w, h);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.8f, 0.3f, 0.1f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    round_trip(cv, w, h, "build/test_pngread_solid.png");
    canvas_free(cv);
}

static void scene_gradient(void) {
    int const w = 120, h = 90;
    struct canvas *__single cv = canvas(w, h);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 0.1f, 0.2f, 0.9f, 1.0f);
    canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 1.0f, 0.9f, 0.8f, 0.1f, 0.5f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    round_trip(cv, w, h, "build/test_pngread_gradient.png");
    canvas_free(cv);
}

static void scene_text(void) {
    int const w = 160, h = 60;
    struct canvas *__single cv = canvas(w, h);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.1f, 0.1f, 0.4f, 1.0f);
    canvas_set_font_size(cv, 28.0f);
    canvas_fill_text(cv, "PNG \xE5\xBE\x80\xE8\xBF\x94", 8.0f, 40.0f);  // "PNG 往返"
    round_trip(cv, w, h, "build/test_pngread_text.png");
    canvas_free(cv);
}

static void scene_emoji(void) {
    int const w = 80, h = 80;
    struct canvas *__single cv = canvas(w, h);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.95f, 0.95f, 0.9f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)w, (float)h);
    canvas_set_font_size(cv, 56.0f);
    canvas_fill_text(cv, "\xF0\x9F\x8C\x88", 12.0f, 64.0f);  // rainbow U+1F308
    round_trip(cv, w, h, "build/test_pngread_emoji.png");
    canvas_free(cv);
}

// Every committed gallery PNG loads with sane dimensions: the decoder accepts
// the entire corpus the encoder ships.
//
// canvas_read_png takes a __null_terminated path, and -fbounds-safety has no
// safe conversion from a runtime-built byte buffer to __null_terminated -- a
// path assembled from dirent names could only cross via an unsafe bridge.  So
// the corpus is this explicit list of literal paths (string literals are
// __null_terminated already), and the directory walk below only COUNTS the
// committed .png entries to prove the list is complete: adding a gallery scene
// without listing it here fails the count check loudly.
static char const *__null_terminated const k_gallery[] = {
    "gallery/affine.png",
    "gallery/batch.png",
    "gallery/blend.png",
    "gallery/clip.png",
    "gallery/colorspaces.png",
    "gallery/conic.png",
    "gallery/dashes.png",
    "gallery/dirtyrect.png",
    "gallery/drawimage.png",
    "gallery/emoji.png",
    "gallery/emojiscale.png",
    "gallery/filters.png",
    "gallery/gradients.png",
    "gallery/gradinterp.png",
    "gallery/hittest.png",
    "gallery/imagedata.png",
    "gallery/imagescale.png",
    "gallery/joins.png",
    "gallery/linearlight.png",
    "gallery/miterdash.png",
    "gallery/path2d.png",
    "gallery/paths.png",
    "gallery/pattern.png",
    "gallery/porterduff.png",
    "gallery/roundrect.png",
    "gallery/rtl.png",
    "gallery/selection.png",
    "gallery/shadows.png",
    "gallery/shapes.png",
    "gallery/shaping.png",
    "gallery/smoothing.png",
    "gallery/strokerect.png",
    "gallery/subrect.png",
    "gallery/text.png",
    "gallery/textgrid.png",
    "gallery/textmaxwidth.png",
    "gallery/textmetrics.png",
    "gallery/winding.png",
};
enum { GALLERY_N = (int)(sizeof k_gallery / sizeof k_gallery[0]) };

static void gallery_corpus(void) {
    // Count the committed .png entries.  d_name is a fixed array from an
    // un-annotated system header, so its length is found by indexed scan --
    // every access bounds-checked against the array, no strlen, no NUL trust.
    DIR *d = opendir("gallery");
    CHECK(d != NULL);
    if (!d) {
        return;
    }
    int found = 0;
    for (struct dirent *e = readdir(d); e; e = readdir(d)) {
        size_t n = 0;
        while (n < sizeof e->d_name && e->d_name[n] != '\0') {
            n++;
        }
        if (n >= 4 && memcmp(e->d_name + n - 4, ".png", 4) == 0) {
            found += 1;
        }
    }
    (void)closedir(d);
    CHECK(found == GALLERY_N);  // the list covers all of gallery/, and the
                                // directory isn't accidentally empty

    for (int i = 0; i < GALLERY_N; i++) {
        int w = 0, h = 0, len = 0;
        uint8_t *px = canvas_read_png(k_gallery[i], &w, &h, &len);
        CHECK(px != NULL);
        CHECK(w > 0 && h > 0 && len == w * h * 4);
        if (!px) {
            (void)fprintf(stderr, "  failed: %s\n", k_gallery[i]);
        }
        free(px);
    }
}

int main(void) {
    scene_solid();
    scene_gradient();
    scene_text();
    scene_emoji();
    gallery_corpus();
    return TEST_REPORT();
}
