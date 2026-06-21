#pragma once

#include "canvas.h"  // enum canvas_color_space, enum canvas_alpha_type

#include <ptrcheck.h>
#include <stdint.h>

// Reified images, in the Skia vocabulary: an image is a thing you draw FROM,
// a surface (the canvas) a thing you draw TO, a bitmap the raw RGBA8 memory
// realizing either.  An image owns a copy of its pixels, which gives them
// identity -- the hook derived data caches against.  The canvas methods that
// draw FROM an image (canvas_draw_image / _scaled / _subrect) and the snapshot
// constructor (canvas_snapshot) live in canvas.h.
struct canvas_image;

// Construct an image, one typed constructor per colour type, each taking its
// alpha type -- the four formats are peers, none favoured.  Pixels are RGBA,
// top row first, copied in (lossless: a 1:1 draw of an unorm8 unpremul image
// in the working space is byte-identical to drawing the bitmap directly).
// `space` tags how to interpret the pixels' colours: the image is filtered in
// that space (its mip chain too), and the resolved sample converts to the
// canvas working space on deposit -- a no-op when they match.  NULL on bad
// dimensions or allocation failure; free with canvas_image_free (which, like
// free(), accepts NULL).
struct canvas_image *__single canvas_image_unorm8(
    enum canvas_color_space space,
    uint8_t const *__counted_by(w * h * 4) px, int w, int h,
    enum canvas_alpha_type at);
struct canvas_image *__single canvas_image_f16(
    enum canvas_color_space space,
    _Float16 const *__counted_by(w * h * 4) px, int w, int h,
    enum canvas_alpha_type at);

// Build the image's mip pyramid -- the one-time cost that minifying draws at
// medium/high smoothing quality then sample with trilinear filtering.
// Deliberately explicit: WITHOUT built mips a minifying image draw falls
// back to plain bilinear rather than hiding a rebuild (the bitmap entry
// points' behaviour) -- the caller decides if and when to pay.  Idempotent;
// false on allocation failure (the image stays valid, mip-less).
bool canvas_image_build_mips(struct canvas_image *__single img);

int canvas_image_width(struct canvas_image const *__single img);
int canvas_image_height(struct canvas_image const *__single img);
void canvas_image_free(struct canvas_image *__single img);
