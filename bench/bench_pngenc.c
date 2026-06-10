// Isolated benchmark: PNG encoding of gallery-realistic pixels.  bench_png's
// synthetic ramp Up-filters to all-zero rows (~107x compression: pure long-
// match territory), while a real gallery scene compresses ~10x with the
// literal/short-match mix the LZ77 matcher actually sees -- so this is the
// honest encode number for real content.  The pixels are decoded once from the
// committed gallery/blend.png (lossless, so the input is stable even as the
// encoder's emitted bytes evolve); only cnvs_png_encode is timed.  Run from
// the repo root (where `ninja benchcmp` / bench/profile.sh run everything).
#include "bench_reps.h"

#include "cnvs_png.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ITERS 8

int main(void) {
    int w = 0, h = 0, len = 0;
    uint8_t *px = cnvs_png_read("gallery/blend.png", &w, &h, &len);
    if (!px) {
        (void)fprintf(stderr, "bench_pngenc: cannot read gallery/blend.png "
                              "(run from the repo root)\n");
        return 1;
    }

    double sink = 0.0;
    int reps = bench_reps();
    for (int rep = 0; rep < reps; rep++) {
        for (int it = 0; it < ITERS; it++) {
            int outlen = 0;
            uint8_t *out = cnvs_png_encode(px, w, h, &outlen);
            if (!out) {
                free(px);
                return 1;
            }
            sink += (double)outlen;
            free(out);
        }
    }

    free(px);
    (void)fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
