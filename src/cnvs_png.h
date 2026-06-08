#pragma once

#include <ptrcheck.h>
#include <stdint.h>

// `pixels` is tightly packed RGBA8, top row first.  Uses uncompressed (stored)
// deflate blocks -- valid PNG, no compression dependency, but larger files.
bool cnvs_png_write(char const *__null_terminated path,
                    uint8_t const *__counted_by(width * height * 4) pixels,
                    int width, int height);
