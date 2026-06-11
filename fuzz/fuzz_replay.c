// Fuzzes the text canvas-program parser (src/cnvs_replay.c) on adversarial bytes.
// The parser reads untrusted text by index, copies numeric/text leaves across the
// __null_terminated seam, and dispatches to canvas_* -- a classic C text-parsing
// attack surface (tokenizing, number parsing, line handling).  ASan is the
// oracle; the input is raw fuzz bytes (need not be valid UTF-8 or NUL-terminated).

#include "canvas.h"
#include "cnvs_replay.h"

#include <ptrcheck.h>
#include <stdint.h>
#include <stddef.h>

int LLVMFuzzerTestOneInput(uint8_t const *data, size_t size) {
    struct canvas *cv = canvas_create(64, 48);
    if (!cv) {
        return 0;
    }
    cnvs_replay_text(cv, (char const *)data, size);
    canvas_destroy(cv);
    return 0;
}

#ifndef FUZZ_NO_MAIN
#include <stdio.h>
#include <stdlib.h>
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
