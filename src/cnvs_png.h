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

// Strict PNG decode, scoped to exactly what cnvs_png_encode produces.
// Accepted: 8-bit RGBA (colour type 6, bit depth 8), non-interlaced, IHDR
// first, one run of consecutive IDATs, IEND last with nothing after it, CRC
// verified on EVERY chunk (ancillary chunks are skipped, their CRCs still
// checked; unknown critical chunks reject), zlib via cnvs_zlib_inflate, and
// per-row filter bytes 0 (None) or 2 (Up) ONLY.  Everything else -- palette,
// gray, 16-bit, interlace, Sub/Avg/Paeth rows, bad magic, bad CRC, truncation,
// trailing garbage, dimension bombs -- fails cleanly: NULL, *w/*h/*len zeroed.
// Dimensions are capped at 16384 like the encoder, so w*h*4 and the filtered
// stream both fit an int and no allocation exceeds ~1 GiB regardless of what
// the input declares.  On success: a malloc'd RGBA8 buffer (top row first,
// caller frees) with *w/*h/*len filled in.
uint8_t *__counted_by_or_null(*len)
cnvs_png_decode(uint8_t const *__counted_by(n) src, int n,
                int *__single w, int *__single h, int *__single len);

// Read `path` and cnvs_png_decode it.  The file must fit an int (< 2 GiB,
// the same arithmetic cap the decoder works in); larger files, unreadable
// files, and anything cnvs_png_decode rejects all return NULL with the outs
// zeroed.
uint8_t *__counted_by_or_null(*len)
cnvs_png_read(char const *__null_terminated path,
              int *__single w, int *__single h, int *__single len);
