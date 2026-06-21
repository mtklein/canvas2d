// Differential fuzz: our strict zlib inflate (src/canvas2d_zlib.c) against the
// SYSTEM zlib (-lz) on the same adversarial bytes at the same output cap --
// the H5 oracle from docs/decisions/codec-outsourcing.md at fuzz time, renting
// reference zlib's correctness without shipping its bytes.  The oracle is
// accept-set equality plus output equality: if the reference succeeds, ours
// must succeed with identical output; if ours succeeds, the reference must
// have too.  The one allowed asymmetry (pinned deterministically in
// tests/test_zlib_oracle.c) is trailing garbage: uncompress2 stops at stream
// end and calls leftover input success, while our strict end state rejects
// the buffer -- so when the reference reports unconsumed input, ours must
// reject the whole buffer but accept the exact consumed prefix, byte for
// byte.  Any payload either side inflates is then re-deflated by our encoder
// and must re-inflate identically under BOTH decoders, tying our emission to
// the reference's acceptance as well as our own.  This harness links -lz; the
// canvas library never does (configure.py's EXTRA_LIBS).
//
// Seeds: fuzz/seeds_zlib/ (shared with fuzz_inflate; curated, committed).
// Run with a /tmp scratch corpus first so libFuzzer's discoveries don't
// pollute the seed dir:
//   ./build/fuzz/fuzz_zlib_diff -max_len=4096 /tmp/zlib_diff_corpus fuzz/seeds_zlib

#include "canvas2d_zlib.h"

#include <assert.h>
#include <ptrcheck.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

// Reference inflate via uncompress2, which reports the input bytes it
// consumed.  Returns the payload length, or -1 when the reference rejects.
static int ref_inflate(uint8_t *dst, int dcap, uint8_t const *src, int n,
                       int *consumed) {
    uLongf dlen = (uLongf)dcap;
    uLong slen = (uLong)n;
    int const rc = uncompress2(dst, &dlen, src, &slen);
    *consumed = (int)slen;
    return rc == Z_OK ? (int)dlen : -1;
}

int LLVMFuzzerTestOneInput(uint8_t const *__counted_by(size) data, size_t size) {
    enum { cap = 1 << 16 };  // fixed output budget, comfortably above -max_len
    if (size > (1u << 20)) {
        return 0;
    }
    uint8_t *mine = malloc(cap);
    uint8_t *refs = malloc(cap);
    if (!mine || !refs) {
        free(refs);
        free(mine);
        return 0;
    }
    int consumed = 0;
    int const r = ref_inflate(refs, cap, data, (int)size, &consumed);
    int o = canvas2d_zlib_inflate(mine, cap, data, (int)size);
    if (r < 0) {
        assert(o == -1);  // ours never accepts what the reference rejects
    } else if (consumed == (int)size) {
        assert(o == r);  // both accept: identical length and bytes
        assert(r == 0 || memcmp(mine, refs, (size_t)r) == 0);
    } else {
        // Trailing garbage: the documented strictness delta, one way only.
        assert(o == -1);
        o = canvas2d_zlib_inflate(mine, cap, data, consumed);
        assert(o == r);  // the bare stream must agree exactly
        assert(r == 0 || memcmp(mine, refs, (size_t)r) == 0);
    }
    if (r >= 0) {
        // Round-trip the payload through our deflate, then back through BOTH
        // decoders against exact-size heap buffers (redzones bite on overrun).
        int const zcap = canvas2d_zlib_bound(r);
        uint8_t *z = malloc((size_t)zcap);
        uint8_t *back = malloc(r > 0 ? (size_t)r : 1);
        uint8_t *back2 = malloc(r > 0 ? (size_t)r : 1);
        if (z && back && back2) {
            int const zn = canvas2d_zlib_deflate(z, zcap, refs, r);
            assert(zn > 0);
            assert(canvas2d_zlib_inflate(back, r, z, zn) == r);
            assert(r == 0 || memcmp(back, refs, (size_t)r) == 0);
            int used = 0;
            assert(ref_inflate(back2, r, z, zn, &used) == r);
            assert(used == zn);  // the reference eats our whole stream
            assert(r == 0 || memcmp(back2, refs, (size_t)r) == 0);
        }
        free(back2);
        free(back);
        free(z);
    }
    free(refs);
    free(mine);
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
