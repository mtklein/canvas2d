// Isolated benchmark: PNG decoding (chunk CRC verification + strict inflate +
// un-Up defilter) of a committed gallery PNG -- the realistic decode workload,
// since the decoder is scoped to our own encoder's files.  gallery/blend.png is
// the largest committed scene (724x396, ~10x compression), so the inflate side
// sees the literal/match mix real scenes produce, not a synthetic ramp.  The
// file is read into memory once; only cnvs_png_decode is timed.  Run from the
// repo root (where `ninja benchcmp` / bench/profile.sh run everything).
#include "bench_reps.h"

#include "cnvs_png.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ITERS 20

int main(void) {
    FILE *f = fopen("gallery/blend.png", "rb");
    if (!f) {
        (void)fprintf(stderr, "bench_pngdec: cannot open gallery/blend.png "
                              "(run from the repo root)\n");
        return 1;
    }
    long sz = 0;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) <= 0 || sz > (long)INT_MAX ||
        fseek(f, 0, SEEK_SET) != 0) {
        (void)fclose(f);
        return 1;
    }
    int const n = (int)sz;
    uint8_t *__counted_by_or_null(n) buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        (void)fclose(f);
        return 1;
    }
    (void)fclose(f);

    double sink = 0.0;
    int reps = bench_reps();
    for (int rep = 0; rep < reps; rep++) {
        for (int it = 0; it < ITERS; it++) {
            int w = 0, h = 0, len = 0;
            uint8_t *px = cnvs_png_decode(buf, n, &w, &h, &len);
            if (!px) {
                free(buf);
                return 1;
            }
            sink += (double)px[(size_t)len / 2];
            free(px);
        }
    }

    free(buf);
    (void)fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
