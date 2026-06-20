// The capstone of the record/replay arc: each committed gallery/<scene>.canvas
// program replays to its committed gallery/<scene>.png BYTE FOR BYTE, with
// ZERO Core Text boundary calls -- all 45 scenes, the format's whole surface.
//
// Every scene records a self-contained program alongside its PNG
// (examples/gallery.c's record_scene; src/cnvs_record.c serializes the
// font/glyph/bitmap/shape blocks the text needs, the image blocks behind
// drawImage/putImageData/patterns, and the path blocks behind the Path2D
// draws).  This test is the determinism gate: for each program it
//   (a) reads the committed PNG file (its IHDR gives the canvas dimensions),
//   (b) replays the program onto a fresh canvas of those dimensions,
//   (c) re-encodes the replayed canvas and byte-compares to the PNG file, and
//   (d) asserts the replay took ZERO shape/glyph boundary cache-misses.
//
// Both directions are the proof.  The re-encode-and-compare proves the replayed
// pixels match the committed render exactly AND that the encoder is
// deterministic (there is no decoder in the tree).  The zero-miss assertion
// proves the glyphs and emoji captures came from the program's embedded blocks,
// not from host fonts -- a single miss would mean a fill_text fell back through the
// boundary to Core Text (and on a machine WITHOUT the recording machine's
// fonts -- the CI runner has no Libian TC, it is download-on-demand -- that
// fallback would also have produced different, wrong, or blank glyphs, which
// the byte compare independently catches).  Scenes that draw no text pass (d)
// trivially; the thirteen TEXT scenes additionally assert the cache saw real
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// All 46 scenes, each a (program, image) pair of committed artifacts; `text`
// marks the fourteen text scenes whose cache traffic must be non-trivial.
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
    { "gallery/colorspaces.canvas",  "gallery/colorspaces.png",  true  },
    { "gallery/conic.canvas",        "gallery/conic.png",        false },
    { "gallery/dashes.canvas",       "gallery/dashes.png",       false },
    { "gallery/dirtyrect.canvas",    "gallery/dirtyrect.png",    false },
    { "gallery/drawimage.canvas",    "gallery/drawimage.png",    false },
    { "gallery/ellipserot.canvas",   "gallery/ellipserot.png",   false },
    { "gallery/emoji.canvas",        "gallery/emoji.png",        true  },
    { "gallery/emojiscale.canvas",   "gallery/emojiscale.png",   true  },
    { "gallery/extendedrange.canvas", "gallery/extendedrange.png", false },
    { "gallery/filters.canvas",      "gallery/filters.png",      false },
    { "gallery/fontfamily.canvas",   "gallery/fontfamily.png",   true  },
    { "gallery/fontstyle.canvas",    "gallery/fontstyle.png",    true  },
    { "gallery/gradients.canvas",    "gallery/gradients.png",    false },
    { "gallery/gradinterp.canvas",   "gallery/gradinterp.png",   true  },
    { "gallery/hittest.canvas",      "gallery/hittest.png",      false },
    { "gallery/imagecolorspace.canvas", "gallery/imagecolorspace.png", false },
    { "gallery/imagedata.canvas",    "gallery/imagedata.png",    false },
    { "gallery/imagedataf16.canvas", "gallery/imagedataf16.png", false },
    { "gallery/imagescale.canvas",   "gallery/imagescale.png",   false },
    { "gallery/joins.canvas",        "gallery/joins.png",        false },
    { "gallery/linearlight.canvas",  "gallery/linearlight.png",  false },
    { "gallery/miterdash.canvas",    "gallery/miterdash.png",    false },
    { "gallery/nestedclip.canvas",   "gallery/nestedclip.png",   false },
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
    { "gallery/textspacing.canvas",  "gallery/textspacing.png",  true  },
    { "gallery/winding.canvas",      "gallery/winding.png",      false },
};
enum { SCENE_N = (int)(sizeof k_scenes / sizeof k_scenes[0]) };

// Replay one scene's program onto a fresh canvas sized from its PNG's IHDR, then
// prove the re-encoded canvas matches the committed PNG byte for byte and the
// replay never crossed the text boundary.
static void check_scene(scene_pair s) {
    // Read the committed PNG file whole (the count sits beside its pointer, the
    // -fbounds-safety idiom; no decoder in the tree, just raw bytes).
    FILE *f = fopen(s.png, "rb");
    CHECK(f != NULL);
    if (!f) {
        (void)fprintf(stderr, "  open failed: %s\n", s.png);
        return;
    }
    long sz = -1;
    if (fseek(f, 0, SEEK_END) == 0) {
        sz = ftell(f);
    }
    rewind(f);
    CHECK(sz >= 24 && sz <= (1 << 30));  // signature + IHDR header (width/height)
    if (sz < 24 || sz > (1 << 30)) {
        (void)fclose(f);
        return;
    }
    int const fsz = (int)sz;
    uint8_t *__counted_by_or_null(fsz) want = malloc((size_t)fsz);
    CHECK(want != NULL);
    if (!want) {
        (void)fclose(f);
        return;
    }
    bool const rd = fread(want, 1, (size_t)fsz, f) == (size_t)fsz;
    (void)fclose(f);
    CHECK(rd);
    if (!rd) {
        free(want);
        return;
    }
    // The IHDR width/height (bytes 16..23, big-endian) size the replay canvas --
    // a fixed-offset peek of bytes we are about to byte-compare, not a decode.
    int const w = (int)(((uint32_t)want[16] << 24) | ((uint32_t)want[17] << 16) |
                        ((uint32_t)want[18] << 8) | (uint32_t)want[19]);
    int const h = (int)(((uint32_t)want[20] << 24) | ((uint32_t)want[21] << 16) |
                        ((uint32_t)want[22] << 8) | (uint32_t)want[23]);
    CHECK(w > 0 && h > 0 && w <= 16384 && h <= 16384);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        free(want);
        return;
    }

    struct canvas *__single cv = canvas(w, h, CANVAS_CS_SRGB);
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

    // Re-encode the replayed canvas through the real save path, then byte-compare
    // to the committed PNG file.
    int elen = 0;
    uint8_t *enc = canvas_encode_png(cv, &elen);
    CHECK(enc != NULL);
    CHECK(elen == fsz);
    if (enc && elen == fsz) {
        int const cmp = memcmp(enc, want, (size_t)fsz);
        CHECK(cmp == 0);
        if (cmp != 0) {
            (void)fprintf(stderr, "  %s DIVERGED from %s\n", s.canvas, s.png);
        } else if (getenv("REPLAY_GALLERY_VERBOSE")) {
            // Tests are silent on success (the suite's convention); the
            // per-scene IDENTICAL table is opt-in for a human run.
            (void)fprintf(stderr, "  %-32s IDENTICAL\n", s.canvas);
        }
    }

    free(enc);
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
