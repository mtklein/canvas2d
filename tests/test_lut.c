#include "lut.h"
#include "test_util.h"

#include <stdlib.h>

// Apply `lut` to `n` pixels both ways and check they agree with each other and with
// the reference mapping.  n is not a multiple of 16, so the NEON tail runs too.
static void check_lut(uint8_t const *__counted_by(256) lut, int n) {
    int const len = n;  // counted locals need a const count, not the parameter
    uint8_t *__counted_by(len) a = malloc((size_t)len);
    uint8_t *__counted_by(len) b = malloc((size_t)len);
    uint8_t *__counted_by(len) ref = malloc((size_t)len);
    CHECK(a != NULL && b != NULL && ref != NULL);
    if (a && b && ref) {
        for (int i = 0; i < n; i++) {
            uint8_t v = (uint8_t)((i * 53 + 7) & 0xFF);
            a[i] = v; b[i] = v; ref[i] = lut[v];
        }
        lut_apply_mem(a, n, lut);
        lut_apply_neon(b, n, lut);
        bool ok = true;
        for (int i = 0; i < n; i++) {
            if (a[i] != ref[i] || b[i] != ref[i]) {
                ok = false;
            }
        }
        CHECK(ok);
    }
    free(a); free(b); free(ref);
}

int main(void) {
    uint8_t invert[256], gamma[256], ramp16[256];
    for (int i = 0; i < 256; i++) {
        invert[i] = (uint8_t)(255 - i);
        // crude gamma-ish curve and a 16-level posterize, to exercise varied tables
        gamma[i] = (uint8_t)((i * i + 255) / 256);
        ramp16[i] = (uint8_t)((i & 0xF0) | (i >> 4));
    }
    check_lut(invert, 100);
    check_lut(gamma, 1000);
    check_lut(ramp16, 16);    // exact multiple of the vector width (no tail)
    check_lut(invert, 7);     // shorter than the vector width (all tail)
    return TEST_REPORT();
}
