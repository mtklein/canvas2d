#include "lut.h"

#include <arm_neon.h>
#include <string.h>

void lut_apply_mem(uint8_t *__counted_by(n) px, int n,
                   uint8_t const *__counted_by(256) lut) {
    for (int i = 0; i < n; i++) {
        px[i] = lut[px[i]];  // index is uint8 (0..255) yet still checked per element
    }
}

void lut_apply_neon(uint8_t *__counted_by(n) px, int n,
                    uint8_t const *__counted_by(256) lut) {
    // Load the 256-entry table once into four 64-byte register tables.  Each
    // vqtbl4q_u8 returns lut[idx] for indices in its own 64-wide range and 0
    // elsewhere (out-of-range index -> 0), so OR-combining the four reconstructs
    // lut[idx] for every idx in 0..255 -- the gather lives entirely in registers.
    uint8x16x4_t t0, t1, t2, t3;
    memcpy(&t0, lut, sizeof t0);          // checked: lut has 256 >= 64
    memcpy(&t1, lut + 64, sizeof t1);
    memcpy(&t2, lut + 128, sizeof t2);
    memcpy(&t3, lut + 192, sizeof t3);
    uint8x16_t const k64 = vdupq_n_u8(64), k128 = vdupq_n_u8(128), k192 = vdupq_n_u8(192);

    int i = 0;
    for (; i + 16 <= n; i += 16) {
        uint8x16_t idx;
        memcpy(&idx, px + i, sizeof idx);          // checked block load of 16 indices
        uint8x16_t r = vqtbl4q_u8(t0, idx);
        r = vorrq_u8(r, vqtbl4q_u8(t1, vsubq_u8(idx, k64)));
        r = vorrq_u8(r, vqtbl4q_u8(t2, vsubq_u8(idx, k128)));
        r = vorrq_u8(r, vqtbl4q_u8(t3, vsubq_u8(idx, k192)));
        memcpy(px + i, &r, sizeof r);
    }
    for (; i < n; i++) {
        px[i] = lut[px[i]];                        // scalar tail
    }
}
