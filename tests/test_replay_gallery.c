// The capstone of the text-determinism arc: each committed gallery/<scene>.canvas
// text program replays to its committed gallery/<scene>.png BYTE FOR BYTE, with
// ZERO Core Text boundary calls.
//
// The seven text scenes record self-contained programs alongside their PNGs
// (examples/gallery.c's record_scene; src/cnvs_record.c serializes the font/
// glyph/bitmap/shape blocks the text needs).  This test is the cross-machine
// determinism gate: for each program it
//   (a) loads the committed PNG via canvas_load_png (the Z2 decoder),
//   (b) replays the program onto a fresh canvas of the PNG's dimensions,
//   (c) byte-compares the canvas's read_rgba to the decoded PNG, and
//   (d) asserts the replay took ZERO shape/glyph boundary cache-misses.
//
// Both directions are the proof.  The byte compare proves the replayed pixels
// match the committed render exactly.  The zero-miss assertion proves the
// glyphs and emoji captures came from the program's embedded blocks, not from
// host fonts -- a single miss would mean a fill_text fell back through the
// boundary to Core Text (and on a machine WITHOUT the recording machine's fonts
// -- the CI runner has no Libian TC, it is download-on-demand -- that fallback
// would also have produced different, wrong, or blank glyphs, which the byte
// compare independently catches).  Run on the fontless runner via bare ninja,
// this is what proves replay used the embedded data, not the host's fonts;
// locally (where the fonts exist) it must also pass, so a stale .canvas or a
// renderer change that didn't re-emit the program is caught here too.
//
// See test_record_text.c (the in-memory round-trip + strict-parse unit) and
// docs/text-boundary.md.  The gallery PNGs/programs are committed build
// artifacts re-emitted in lockstep by `ninja images`.

#include "test_util.h"

#include "canvas.h"
#include "cnvs_text.h"

#include <dirent.h>
#include <ptrcheck.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The seven text scenes, each a (program, image) pair of committed artifacts.
// Literal paths so they are __null_terminated already (the same reason
// test_pngload.c lists its corpus rather than assembling paths from dirent
// names: -fbounds-safety has no safe runtime-bytes -> __null_terminated bridge).
// The directory walk below counts the committed .canvas files to prove this
// list is complete -- recording a new text scene without listing it here fails
// the count loudly.
typedef struct {
    char const *__null_terminated canvas;
    char const *__null_terminated png;
} scene_pair;

static scene_pair const k_scenes[] = {
    { "gallery/emoji.canvas",        "gallery/emoji.png"        },
    { "gallery/emojiscale.canvas",   "gallery/emojiscale.png"   },
    { "gallery/shaping.canvas",      "gallery/shaping.png"      },
    { "gallery/text.canvas",         "gallery/text.png"         },
    { "gallery/textgrid.canvas",     "gallery/textgrid.png"     },
    { "gallery/textmaxwidth.canvas", "gallery/textmaxwidth.png" },
    { "gallery/textmetrics.canvas",  "gallery/textmetrics.png"  },
};
enum { SCENE_N = (int)(sizeof k_scenes / sizeof k_scenes[0]) };

// Replay one scene's program onto a fresh canvas sized to its PNG, then prove
// the pixels match the PNG byte for byte and the replay never crossed the text
// boundary.
static void check_scene(scene_pair s) {
    int w = 0, h = 0, len = 0;
    uint8_t *ref = canvas_load_png(s.png, &w, &h, &len);
    CHECK(ref != NULL);
    CHECK(w > 0 && h > 0 && len == w * h * 4);
    if (!ref || len <= 0) {
        (void)fprintf(stderr, "  load failed: %s\n", s.png);
        free(ref);
        return;
    }

    canvas *__single cv = canvas_create(w, h);
    CHECK(cv != NULL);
    if (!cv) {
        free(ref);
        return;
    }

    bool ok = canvas_replay_from(cv, s.canvas);
    CHECK(ok);
    if (!ok) {
        (void)fprintf(stderr, "  replay failed: %s\n", s.canvas);
    }

    // Zero boundary misses: every shape and glyph (outline AND emoji capture --
    // captures bump glyph_misses on a boundary fetch) came from the program's
    // embedded blocks.  A nonzero miss is a Core Text fallback -- exactly what a
    // fontless machine cannot do.
    cnvs_text_cache *__single c = cnvs_canvas_text_cache(cv);
    CHECK(c->shape_misses == 0);
    CHECK(c->glyph_misses == 0);
    CHECK(c->shape_hits > 0);   // the program really exercised the text cache
    CHECK(c->glyph_hits > 0);
    if (c->shape_misses != 0 || c->glyph_misses != 0) {
        (void)fprintf(stderr,
                      "  %s crossed the boundary: shape_miss=%d glyph_miss=%d\n",
                      s.canvas, c->shape_misses, c->glyph_misses);
    }

    int const n = len;
    uint8_t *__counted_by_or_null(n) got = malloc((size_t)n);
    CHECK(got != NULL);
    if (got) {
        canvas_read_rgba(cv, got, n);
        int cmp = memcmp(ref, got, (size_t)n);
        CHECK(cmp == 0);
        if (cmp != 0) {
            (void)fprintf(stderr, "  %s diverged from %s\n", s.canvas, s.png);
        }
    }

    free(got);
    free(ref);
    canvas_destroy(cv);
}

// The scene list covers every committed gallery/*.canvas (and the directory
// isn't accidentally empty).  d_name is a fixed array from an un-annotated
// system header, scanned by index -- every access bounds-checked, no strlen, no
// NUL trust (the test_pngload.c posture).
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
