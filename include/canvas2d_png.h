#pragma once

#include <ptrcheck.h>
#include <stdint.h>

// PNG encode: 16-bit RGBA (colour type 6), non-interlaced, every row Up-filtered
// (filter byte 2), one IDAT carrying a canvas2d_zlib deflate stream, plus a cICP
// chunk signalling BT.2100 (Rec.2020 primaries, PQ transfer).  The pixels are
// already in that encoding; the colour pipeline that produces them is in
// canvas.c (canvas2d_encode_png).  Up-only is a deliberate trade (see canvas2d_png.c):
// it vectorizes as a whole-row subtract with no left-neighbor recurrence.
// Deterministic: identical pixels -> identical PNG bytes, always.

// Encode to a malloc'd buffer (caller frees), storing its byte length in
// *outlen.  `pixels` is tightly packed RGBA, top row first, four uint16 samples
// per pixel (native-endian; serialized big-endian here).  On bad dimensions
// (non-positive, or large enough that the deflate size arithmetic would not fit
// an int) or out of memory: NULL, *outlen = 0.
uint8_t *__counted_by_or_null(*outlen)
canvas2d_png_encode(uint16_t const *__counted_by(width * height * 4) pixels,
                int width, int height, int *__single outlen);

// canvas2d_png_encode + write the bytes to `path`.
bool canvas2d_png_write(char const *__null_terminated path,
                    uint16_t const *__counted_by(width * height * 4) pixels,
                    int width, int height);
