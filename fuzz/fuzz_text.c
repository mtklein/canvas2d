// Fuzzes the UNCHECKED Core Text shim (src/cnvs_text_ct.c -- BOUNDARY_C, built
// WITHOUT -fbounds-safety) through the public text API.  This is the highest-risk
// remaining surface: a bug here is real corruption, not a trap, and ASan is the
// only memory-safety net in that TU.  The harness drives, on adversarial bytes:
//   - utf8_next        the hand-written UTF-8 decoder (must never read past the
//                      NUL terminator -- the key property under test);
//   - glyph_for_cp     surrogate-pair composition for astral code points;
//   - emit/CGPathApply the glyph-outline callback that indexes points[] per
//                      element type, mapped through the transform (gpt()).
//
// Text is NUL-terminated, per the API's __null_terminated contract.  A transform
// and font-size are mixed in so the outline-emit coordinate mapping is exercised.

#include "canvas.h"

#include <ptrcheck.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(uint8_t const *__counted_by(size) data, size_t size) {
    struct canvas *__single cv = canvas_create(96, 48);
    if (!cv) {
        return 0;
    }

    size_t at = 0;
    uint8_t b0 = at < size ? data[at++] : 0;
    uint8_t b1 = at < size ? data[at++] : 0;
    canvas_set_font_size(cv, (float)(b0 % 48) + 4.0f);
    canvas_rotate(cv, (float)((int)b1 - 128) * 0.01f);  // exercise emit()'s gpt() mapping

    // Remaining bytes are the UTF-8 string, NUL-terminated (the API contract;
    // an embedded NUL just ends the string early -- a valid case for utf8_next).
    size_t tlen = size - at;
    char *text = malloc(tlen + 1);
    if (text) {
        if (tlen) {
            memcpy(text, data + at, tlen);
        }
        text[tlen] = '\0';
        (void)canvas_measure_text(cv, text);      // utf8_next + glyph lookup + advances
        canvas_fill_text(cv, text, 4.0f, 30.0f);  // + CTFontCreatePathForGlyph + emit()
        canvas_stroke_text(cv, text, 4.0f, 30.0f);
        free(text);
    }

    canvas_destroy(cv);
    return 0;
}

#ifndef FUZZ_NO_MAIN
#include <stdio.h>
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            continue;
        }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
        size_t got = buf ? fread(buf, 1, n > 0 ? (size_t)n : 0, f) : 0;
        fclose(f);
        if (buf) {
            LLVMFuzzerTestOneInput(buf, got);
            free(buf);
        }
        (void)fprintf(stderr, "ok: %s (%zu bytes)\n", argv[i], got);
    }
    return 0;
}
#endif
