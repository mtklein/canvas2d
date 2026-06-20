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

// cnvs_blit_rgba's f16 twin: four _Float16 components per pixel instead of four
// bytes (the rgba-float16 ImageData layout).  Same clipping contract -- the copy
// rectangle is clipped to both images independently, and dst pixels with no
// source counterpart are left untouched (get_image_data_f16 pre-zeros so they
// read back transparent).
void cnvs_blit_f16(_Float16 *__counted_by(dw * dh * 4) dst, int dw, int dh,
                   int dx, int dy,
                   _Float16 const *__counted_by(sw * sh * 4) src, int sw, int sh,
                   int sx, int sy, int w, int h);
