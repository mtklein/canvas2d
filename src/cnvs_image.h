#pragma once

#include <ptrcheck.h>
#include <stdint.h>

// Copy `w`x`h` RGBA8 pixels from `src` (a tightly packed sw x sh image) starting
// at (sx, sy) into `dst` (dw x dh) starting at (dx, dy).  The rectangle is
// clipped to both images independently -- negative or oversized offsets are
// fine, and dst pixels with no source counterpart are left untouched.
void cnvs_blit_rgba(uint8_t *__counted_by(dw * dh * 4) dst, int dw, int dh,
                    int dx, int dy,
                    uint8_t const *__counted_by(sw * sh * 4) src, int sw, int sh,
                    int sx, int sy, int w, int h);
