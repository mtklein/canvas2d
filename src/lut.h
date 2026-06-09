#pragma once

// Per-pixel 256-entry 8-bit lookup (gamma, sRGB, levels) -- the canonical
// data-dependent gather.  A probe of how -fbounds-safety handles an index that
// comes from the data: the scalar memory LUT is checked per element (the flag does
// not fold the uint8 index range, so `lut[px[i]]` keeps a provably-true bounds
// check), while the NEON version holds the table in registers (vqtbl4q_u8), so the
// gather has no per-lane memory index to check at all.  Both apply the same table.

#include <ptrcheck.h>
#include <stdint.h>

// Scalar: px[i] = lut[px[i]].  One bounds check per pixel on the LUT access.
void lut_apply_mem(uint8_t *__counted_by(n) px, int n,
                   uint8_t const *__counted_by(256) lut);

// NEON: the 256-entry table is loaded once into four register tables and gathered
// with vqtbl4q_u8; no per-lane bounds check, only the block load/store memcpys.
void lut_apply_neon(uint8_t *__counted_by(n) px, int n,
                    uint8_t const *__counted_by(256) lut);
