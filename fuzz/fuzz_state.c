// Temporal-safety fuzz harness: concentrates on the canvas state machine's
// ownership transfers -- the interprocedural lifetime paths the static analyzer
// can't follow (it's intra-TU).  A coverage fuzzer drives deep save/restore
// nesting, clip()'s mask alloc/copy/free, the font cache's destroy-then-recreate,
// and image-data buffers; ASan (use-after-free / -scope / -return, all enabled in
// the fuzz build) is the oracle.  Complements fuzz_api.c, which spreads thin over
// the whole API; here the op set is small so the churn is dense.
//
// Build: ninja fuzzers.  Standard libFuzzer entry; the file-replay main (behind
// FUZZ_NO_MAIN) lets the same binary reproduce a crasher.

#include "canvas.h"

#include <ptrcheck.h>
#include <stdint.h>
#include <stddef.h>

struct cursor {
    uint8_t const *__counted_by(size) p;
    size_t size, at;
    int eof;
};

static uint8_t u8(struct cursor *c) {
    if (c->at >= c->size) { c->eof = 1; return 0; }
    return c->p[c->at++];
}
// A small device-space coordinate (kept on-canvas-ish so clips aren't all empty).
static float coord(struct cursor *c) { return (float)((int)u8(c) - 32); }

enum {
    S_SAVE, S_RESTORE, S_CLIP_RECT, S_CLIP_CIRCLE, S_TRANSLATE, S_SCALE, S_ROTATE,
    S_FONT_SIZE, S_MEASURE, S_FILL_TEXT, S_FILL_RECT, S_GET_IMAGE, S_PUT_IMAGE,
    S_GRADIENT, S_RESET, S_OP_COUNT,
};

int LLVMFuzzerTestOneInput(uint8_t const *__counted_by(size) data, size_t size) {
    struct cursor c = { .p = data, .size = size, .at = 0, .eof = 0 };
    struct canvas *__single cv = canvas(40, 40, CANVAS_CS_SRGB);
    if (!cv) { return 0; }

    uint8_t img[16 * 16 * 4];
    for (int i = 0; i < (int)sizeof img; i++) { img[i] = (uint8_t)(i * 11); }

    int budget = 0;
    while (!c.eof && budget++ < 3000) {
        switch ((int)((unsigned)u8(&c) % (unsigned)S_OP_COUNT)) {
            case S_SAVE:    canvas_save(cv); break;       // snapshots clip mask + state
            case S_RESTORE: canvas_restore(cv); break;    // frees cur mask, adopts saved
            case S_CLIP_RECT:
                canvas_begin_path(cv);
                canvas_rect(cv, coord(&c), coord(&c), coord(&c), coord(&c));
                canvas_clip(cv, CANVAS_NONZERO);                          // alloc full-canvas mask, intersect
                break;
            case S_CLIP_CIRCLE:
                canvas_begin_path(cv);
                canvas_arc(cv, coord(&c), coord(&c), coord(&c), 0.0f, 6.2831853f, false);
                canvas_clip(cv, CANVAS_NONZERO);
                break;
            case S_TRANSLATE: canvas_translate(cv, coord(&c), coord(&c)); break;
            case S_SCALE:     canvas_scale(cv, coord(&c) * 0.1f, coord(&c) * 0.1f); break;
            case S_ROTATE:    canvas_rotate(cv, coord(&c) * 0.1f); break;
            case S_FONT_SIZE: canvas_set_font_size(cv, (float)u8(&c)); break;  // font rebuild
            case S_MEASURE:   (void)canvas_measure_text(cv, "Ag1"); break;
            case S_FILL_TEXT: canvas_fill_text(cv, "Mq", coord(&c), coord(&c)); break;
            case S_FILL_RECT: canvas_fill_rect(cv, coord(&c), coord(&c),
                                               coord(&c), coord(&c)); break;
            case S_GET_IMAGE: {
                uint8_t out[8 * 8 * 4];
                canvas_get_image_data(cv, CANVAS_CS_SRGB, (int)u8(&c) - 8, (int)u8(&c) - 8, 8, 8,
                                      out, (int)sizeof out);
                break;
            }
            case S_PUT_IMAGE:
                canvas_put_image_data(cv, CANVAS_CS_SRGB, img, (int)sizeof img, 16, 16,
                                      (int)u8(&c) - 8, (int)u8(&c) - 8);
                break;
            case S_GRADIENT:
                canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, coord(&c), coord(&c));
                canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
                canvas_add_fill_color_stop(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
                break;
            case S_RESET: canvas_reset_transform(cv); break;
            default: break;
        }
    }
    canvas_free(cv);   // frees the whole state stack + cur mask + font
    return 0;
}

#ifndef FUZZ_NO_MAIN
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { continue; }
        fseek(f, 0, SEEK_END);
        long const n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
        size_t const got = buf ? fread(buf, 1, n > 0 ? (size_t)n : 0, f) : 0;
        fclose(f);
        if (buf) { LLVMFuzzerTestOneInput(buf, got); free(buf); }
        (void)fprintf(stderr, "ok: %s (%zu bytes)\n", argv[i], got);
    }
    return 0;
}
#endif
