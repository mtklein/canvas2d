// Fuzzes the PNG encoder (src/cnvs_png.c) directly via cnvs_png_write, writing to
// /dev/null so the full in-memory encode runs without real file I/O.  The encoder
// is the most-audited code in the tree (a cursor sized up front, every write
// checked against __counted_by(cap)), so this is lower-yield -- but it closes the
// "parser/encoder-shaped input" gap and gives ASan an independent take: in this
// build -fbounds-safety is off (stub ptrcheck.h), so the encoder's own bounds
// checks vanish and ASan alone must witness that the size arithmetic (rawlen /
// zcap / total), the Up-filter row kernel, the deflate it feeds, the cursor
// writes, and the CRC32 paths never run off their buffers, across fuzzed
// dimensions and pixel content.  (The decode side is fuzz_pngdec.)

#include "cnvs_png.h"

#include <ptrcheck.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(uint8_t const *__counted_by(size) data, size_t size) {
    if (size < 4) {
        return 0;
    }
    // Modest dimensions: enough to exercise multi-segment (>65535-byte) zlib
    // blocks and the size math, while keeping the w*h*4 buffer small per run.
    int w = (int)(((unsigned)data[0] | ((unsigned)data[1] << 8)) % 512u) + 1;  // 1..512
    int h = (int)(((unsigned)data[2] | ((unsigned)data[3] << 8)) % 512u) + 1;
    size_t need = (size_t)w * (size_t)h * 4u;

    uint8_t *px = malloc(need);
    if (!px) {
        return 0;
    }
    // Fill from the remaining fuzz bytes (repeating) so adler32/CRC see varied data.
    size_t src_len = size - 4;
    for (size_t i = 0; i < need; i++) {
        px[i] = src_len ? data[4 + (i % src_len)] : (uint8_t)i;
    }

    (void)cnvs_png_write("/dev/null", px, w, h);
    free(px);
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
