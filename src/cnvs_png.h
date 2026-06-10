#pragma once

#include <ptrcheck.h>
#include <stdint.h>

// PNG encode: 8-bit RGBA (color type 6), non-interlaced, every row Up-filtered
// (filter byte 2), one IDAT carrying a cnvs_zlib deflate stream.  Up-only is a
// deliberate trade (see cnvs_png.c): it vectorizes as whole-row subtract/add
// with no left-neighbor recurrence, and the adaptive per-row filter chooser
// only beat it by ~2% across the gallery corpus.  Deterministic: identical
// pixels -> identical PNG bytes, always.

// Encode to a malloc'd buffer (caller frees), storing its byte length in
// *outlen.  `pixels` is tightly packed RGBA8, top row first.  On bad dimensions
// (non-positive, > 16384 -- bounded so every size computation fits an int) or
// out of memory: NULL, *outlen = 0.
uint8_t *__counted_by_or_null(*outlen)
cnvs_png_encode(uint8_t const *__counted_by(width * height * 4) pixels,
                int width, int height, int *__single outlen);

// cnvs_png_encode + write the bytes to `path`.
bool cnvs_png_write(char const *__null_terminated path,
                    uint8_t const *__counted_by(width * height * 4) pixels,
                    int width, int height);
