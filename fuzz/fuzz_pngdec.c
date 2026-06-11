// Fuzzes the strict PNG decoder (cnvs_png_decode in src/cnvs_png.c) on
// adversarial bytes: chunk framing, per-chunk CRC verification, the IHDR
// field/dimension gate, IDAT-run sequencing, and the None/Up defilter -- the
// whole file-format parser in front of the (separately fuzzed) zlib inflate.
// In this build -fbounds-safety is off (stub ptrcheck.h), so ASan alone must
// witness that the decoder's explicit validation keeps every chunk slice and
// row index in bounds.  On a successful decode the pixels are re-encoded with
// cnvs_png_encode and decoded again, asserting dimension and pixel identity
// (the round-trip-through-success oracle), so the encoder's emission and the
// decoder's acceptance stay tied to each other.
//
// Output cap: the decoder itself caps dimensions at 16384 (a ~1 GiB
// worst-case decode); the harness skips inputs whose IHDR would pass that
// gate while declaring more than 1M pixels, so legitimate-looking dimension
// monsters don't thrash the fuzzer's RSS limit while clearly-rejected ones
// (w or h past the cap) still exercise the rejection path.
//
// Seeds: fuzz/seeds_pngdec/ (curated, committed -- a 1x1 and an 8x8 written
// by our own encoder).  Run with a /tmp scratch corpus first so libFuzzer's
// discoveries don't pollute the seed dir:
//   ./build/fuzz/fuzz_pngdec -max_len=4096 /tmp/pngdec_corpus fuzz/seeds_pngdec

#include "cnvs_png.h"

#include <assert.h>
#include <ptrcheck.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t peek32be(uint8_t const *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int LLVMFuzzerTestOneInput(uint8_t const *__counted_by(size) data, size_t size) {
    if (size > (1u << 20)) {
        return 0;
    }
    if (size >= 24) {  // the output cap described above
        uint32_t const w32 = peek32be(data + 16);
        uint32_t const h32 = peek32be(data + 20);
        if (w32 <= 16384u && h32 <= 16384u && w32 * h32 > (1u << 20)) {
            return 0;
        }
    }

    int w = 0, h = 0, len = 0;
    uint8_t *px = cnvs_png_decode(data, (int)size, &w, &h, &len);
    if (px) {
        assert(w > 0 && h > 0 && len == w * h * 4);
        int elen = 0;
        uint8_t *enc = cnvs_png_encode(px, w, h, &elen);
        assert(enc != NULL && elen > 0);
        int w2 = 0, h2 = 0, len2 = 0;
        uint8_t *back = cnvs_png_decode(enc, elen, &w2, &h2, &len2);
        assert(back != NULL);
        assert(w2 == w && h2 == h && len2 == len);
        assert(memcmp(back, px, (size_t)len) == 0);
        free(back);
        free(enc);
    }
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
        long const n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
        size_t const got = buf ? fread(buf, 1, n > 0 ? (size_t)n : 0, f) : 0;
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
