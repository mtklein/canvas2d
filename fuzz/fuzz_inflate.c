// Fuzzes the strict zlib inflate (src/cnvs_zlib.c) on adversarial bytes -- the
// most CVE-scarred parser class in C: Huffman table construction from untrusted
// code lengths, 16/17/18 repeat handling, window back-references (overlapping
// copies included), stored-block length fields, and the bit reader's
// end-of-input seam.  In this build -fbounds-safety is off (stub ptrcheck.h),
// so ASan alone must witness that the decoder's explicit validation -- not the
// flag -- keeps every index in bounds.  On a successful decode the output is
// deflated and inflated again and must round-trip byte-identically (the
// through-success oracle), tying the encoder's emission and the decoder's
// acceptance to each other; the re-inflate runs against exact-size heap
// buffers, so even a one-byte overrun lands in an ASan redzone.
//
// Seeds: fuzz/seeds_zlib/ (curated, committed -- a minimal stream, a
// dynamic-Huffman stream, a stored-block stream).  Run with a /tmp scratch
// corpus first so libFuzzer's discoveries don't pollute the seed dir:
//   ./build/fuzz/fuzz_inflate -max_len=4096 /tmp/zlib_corpus fuzz/seeds_zlib

#include "cnvs_zlib.h"

#include <assert.h>
#include <ptrcheck.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(uint8_t const *__counted_by(size) data, size_t size) {
    enum { cap = 1 << 16 };  // fixed output budget, comfortably above -max_len
    if (size > (1u << 20)) {
        return 0;
    }
    uint8_t *out = malloc(cap);
    if (!out) {
        return 0;
    }
    int const got = cnvs_zlib_inflate(out, cap, data, (int)size);
    if (got >= 0) {
        int const zcap = cnvs_zlib_bound(got);
        uint8_t *z = malloc((size_t)zcap);
        uint8_t *back = malloc(got > 0 ? (size_t)got : 1);  // exact-size: redzones bite
        if (z && back) {
            int const zn = cnvs_zlib_deflate(z, zcap, out, got);
            assert(zn > 0);
            int const bn = cnvs_zlib_inflate(back, got, z, zn);
            assert(bn == got);
            assert(got == 0 || memcmp(back, out, (size_t)got) == 0);
        }
        free(back);
        free(z);
    }
    free(out);
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
