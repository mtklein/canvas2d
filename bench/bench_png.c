// Isolated benchmark: PNG encoding (per-byte checked cursor + CRC32/Adler32).
// Encodes to /dev/null so disk I/O isn't part of the measurement (the encode
// processes ~megabytes; the single write per call is negligible and identical
// across variants).
#include "cnvs_png.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ITERS 80
#define DIM 256

int main(void) {
    const int w = DIM;
    const int h = DIM;
    const int len = w * h * 4;
    uint8_t *px = malloc((size_t)len);
    if (!px) {
        return 1;
    }
    for (int i = 0; i < len; i++) {
        px[i] = (uint8_t)((i * 37) & 0xFF);
    }

    double sink = 0.0;
    for (int it = 0; it < ITERS; it++) {
        if (cnvs_png_write("/dev/null", px, w, h)) {
            sink += 1.0;
        }
    }

    free(px);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
