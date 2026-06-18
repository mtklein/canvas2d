// The capstone of the record/replay arc: each committed gallery/<scene>.canvas
// program replays to its committed gallery/<scene>.png BYTE FOR BYTE, with
// ZERO Core Text boundary calls -- all 37 scenes, the format's whole surface.
//
// Every scene records a self-contained program alongside its PNG
// (examples/gallery.c's record_scene; src/cnvs_record.c serializes the
// font/glyph/bitmap/shape blocks the text needs, the image blocks behind
// drawImage/putImageData/patterns, and the path blocks behind the Path2D
// draws).  This test is the determinism gate: for each program it
//   (a) loads the committed PNG via canvas_read_png (the Z2 decoder),
//   (b) replays the program onto a fresh canvas of the PNG's dimensions,
//   (c) byte-compares the canvas's read_rgba to the decoded PNG, and
//   (d) asserts the replay took ZERO shape/glyph boundary cache-misses.
//
// Both directions are the proof.  The byte compare proves the replayed pixels
// match the committed render exactly.  The zero-miss assertion proves the
// glyphs and emoji captures came from the program's embedded blocks, not from
// host fonts -- a single miss would mean a fill_text fell back through the
// boundary to Core Text (and on a machine WITHOUT the recording machine's
// fonts -- the CI runner has no Libian TC, it is download-on-demand -- that
// fallback would also have produced different, wrong, or blank glyphs, which
// the byte compare independently catches).  Scenes that draw no text pass (d)
// trivially; the nine TEXT scenes additionally assert the cache saw real
// traffic (shape/glyph hits > 0), so the zero-miss claim can't pass vacuously
// where it matters.  Run on the fontless runner via bare ninja, this is what
// proves replay used the embedded data, not the host's fonts; locally (where
// the fonts exist) it must also pass, so a stale .canvas or a renderer change
// that didn't re-emit the program is caught here too.
//
// See test_record.c / test_record_text.c (the in-memory round-trip +
// strict-parse units) and docs/text-boundary.md.  The gallery PNGs/programs
// are committed build artifacts re-emitted in lockstep by `ninja images`.

#include "test_util.h"

#include "canvas.h"
#include "cnvs_text.h"

#include <dirent.h>
#include <ptrcheck.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// All 37 scenes, each a (program, image) pair of committed artifacts; `text`
// marks the nine text scenes whose cache traffic must be non-trivial.
// Literal paths so they are __null_terminated already (the same reason
// test_pngread.c lists its corpus rather than assembling paths from dirent
// names: -fbounds-safety has no safe runtime-bytes -> __null_terminated
// bridge).  The directory walk below counts the committed .canvas files to
// prove this list is complete -- recording a new scene without listing it
// here fails the count loudly.
typedef struct {
    char const *__null_terminated canvas;
    char const *__null_terminated png;
    bool text;
} scene_pair;

static scene_pair const k_scenes[] = {
    { "gallery/affine.canvas",       "gallery/affine.png",       false },
    { "gallery/batch.canvas",        "gallery/batch.png",        false },
    { "gallery/blend.canvas",        "gallery/blend.png",        false },
    { "gallery/clip.canvas",         "gallery/clip.png",         false },
    { "gallery/conic.canvas",        "gallery/conic.png",        false },
    { "gallery/dashes.canvas",       "gallery/dashes.png",       false },
    { "gallery/dirtyrect.canvas",    "gallery/dirtyrect.png",    false },
    { "gallery/drawimage.canvas",    "gallery/drawimage.png",    false },
    { "gallery/emoji.canvas",        "gallery/emoji.png",        true  },
    { "gallery/emojiscale.canvas",   "gallery/emojiscale.png",   true  },
    { "gallery/filters.canvas",      "gallery/filters.png",      false },
    { "gallery/gradients.canvas",    "gallery/gradients.png",    false },
    { "gallery/hittest.canvas",      "gallery/hittest.png",      false },
    { "gallery/imagedata.canvas",    "gallery/imagedata.png",    false },
    { "gallery/imagescale.canvas",   "gallery/imagescale.png",   false },
    { "gallery/joins.canvas",        "gallery/joins.png",        false },
    { "gallery/linearlight.canvas",  "gallery/linearlight.png",  false },
    { "gallery/miterdash.canvas",    "gallery/miterdash.png",    false },
    { "gallery/oklab.canvas",        "gallery/oklab.png",        false },
    { "gallery/path2d.canvas",       "gallery/path2d.png",       false },
    { "gallery/paths.canvas",        "gallery/paths.png",        false },
    { "gallery/pattern.canvas",      "gallery/pattern.png",      false },
    { "gallery/porterduff.canvas",   "gallery/porterduff.png",   false },
    { "gallery/roundrect.canvas",    "gallery/roundrect.png",    false },
    { "gallery/rtl.canvas",          "gallery/rtl.png",          true  },
    { "gallery/selection.canvas",    "gallery/selection.png",    true  },
    { "gallery/shadows.canvas",      "gallery/shadows.png",      false },
    { "gallery/shapes.canvas",       "gallery/shapes.png",       false },
    { "gallery/shaping.canvas",      "gallery/shaping.png",      true  },
    { "gallery/smoothing.canvas",    "gallery/smoothing.png",    false },
    { "gallery/strokerect.canvas",   "gallery/strokerect.png",   false },
    { "gallery/subrect.canvas",      "gallery/subrect.png",      false },
    { "gallery/text.canvas",         "gallery/text.png",         true  },
    { "gallery/textgrid.canvas",     "gallery/textgrid.png",     true  },
    { "gallery/textmaxwidth.canvas", "gallery/textmaxwidth.png", true  },
    { "gallery/textmetrics.canvas",  "gallery/textmetrics.png",  true  },
    { "gallery/winding.canvas",      "gallery/winding.png",      false },
};
enum { SCENE_N = (int)(sizeof k_scenes / sizeof k_scenes[0]) };

// Replay one scene's program onto a fresh canvas sized to its PNG, then prove
// the pixels match the PNG byte for byte and the replay never crossed the text
// boundary.
static void check_scene(scene_pair s) {
    int w = 0, h = 0, len = 0;
    uint8_t *want = canvas_read_png(s.png, &w, &h, &len);
    CHECK(want != NULL);
    CHECK(w > 0 && h > 0 && len == w * h * 4);
    if (!want || len <= 0) {
        (void)fprintf(stderr, "  load failed: %s\n", s.png);
        free(want);
        return;
    }

    struct canvas *__single cv = canvas(w, h);
    CHECK(cv != NULL);
    if (!cv) {
        free(want);
        return;
    }

    bool const ok = canvas_replay_from(cv, s.canvas);
    CHECK(ok);
    if (!ok) {
        (void)fprintf(stderr, "  replay failed: %s\n", s.canvas);
    }

    // Zero boundary misses: every shape and glyph (outline AND emoji capture --
    // captures bump glyph_misses on a boundary fetch) came from the program's
    // embedded blocks.  A nonzero miss is a Core Text fallback -- exactly what a
    // fontless machine cannot do.  (Trivially zero for a scene with no text.)
    struct cnvs_text_cache *__single c = cnvs_canvas_text_cache(cv);
    CHECK(c->shaping_misses == 0);
    CHECK(c->glyph_misses == 0);
    if (s.text) {
        CHECK(c->shaping_hits > 0);  // the program really exercised the text cache
        CHECK(c->glyph_hits > 0);
    }
    if (c->shaping_misses != 0 || c->glyph_misses != 0) {
        (void)fprintf(stderr,
                      "  %s crossed the boundary: shape_miss=%d glyph_miss=%d\n",
                      s.canvas, c->shaping_misses, c->glyph_misses);
    }

    int const n = len;
    uint8_t *__counted_by_or_null(n) got = malloc((size_t)n);
    CHECK(got != NULL);
    if (got) {
        canvas_read_rgba(cv, CANVAS_CS_SRGB, got, n);
        int const cmp = memcmp(want, got, (size_t)n);
        CHECK(cmp == 0);
        if (cmp != 0) {
            (void)fprintf(stderr, "  %s DIVERGED from %s\n", s.canvas, s.png);
        } else if (getenv("REPLAY_GALLERY_VERBOSE")) {
            // Tests are silent on success (the suite's convention); the
            // per-scene IDENTICAL table is opt-in for a human run.
            (void)fprintf(stderr, "  %-32s IDENTICAL\n", s.canvas);
        }
    }

    free(got);
    free(want);
    canvas_free(cv);
}

// The scene list covers every committed gallery/*.canvas (and the directory
// isn't accidentally empty).  d_name is a fixed array from an un-annotated
// system header, scanned by index -- every access bounds-checked, no strlen, no
// NUL trust (the test_pngread.c posture).
static void list_is_complete(void) {
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
        if (n >= 7 && memcmp(e->d_name + n - 7, ".canvas", 7) == 0) {
            found += 1;
        }
    }
    (void)closedir(d);
    CHECK(found == SCENE_N);
}

int main(void) {
    for (int i = 0; i < SCENE_N; i++) {
        check_scene(k_scenes[i]);
    }
    list_is_complete();
    return TEST_REPORT();
}
