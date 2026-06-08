#pragma once

#include <ptrcheck.h>
#include <stdint.h>

// Encode a tightly packed RGBA8 image (top row first) as a PNG file.
// `pixels` must hold width*height*4 bytes.  Returns true on success.
//
// The encoder uses zlib "stored" (uncompressed) deflate blocks, so the output
// is valid PNG that any decoder accepts, without pulling in a compression
// dependency -- and the byte-exact buffer bookkeeping is a deliberate workout
// for -fbounds-safety.
bool cnvs_png_write(const char *__null_terminated path,
                    const uint8_t *__counted_by(width * height * 4) pixels,
                    int width, int height);
