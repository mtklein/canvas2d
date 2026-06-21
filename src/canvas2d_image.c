#include "canvas2d_blit.h"

#include <string.h>

void canvas2d_blit_rgba(uint8_t *__counted_by(dw * dh * 4) dst, int dw, int dh,
                    int dx, int dy,
                    uint8_t const *__counted_by(sw * sh * 4) src, int sw, int sh,
                    int sx, int sy, int w, int h) {
    // Clip the copy rectangle so every src and dst access stays in bounds, then
    // copy only the overlap.
    int i0 = 0;
    if (-sx > i0) { i0 = -sx; }
    if (-dx > i0) { i0 = -dx; }
    int i1 = w;
    if (sw - sx < i1) { i1 = sw - sx; }
    if (dw - dx < i1) { i1 = dw - dx; }

    int j0 = 0;
    if (-sy > j0) { j0 = -sy; }
    if (-dy > j0) { j0 = -dy; }
    int j1 = h;
    if (sh - sy < j1) { j1 = sh - sy; }
    if (dh - dy < j1) { j1 = dh - dy; }

    if (i1 <= i0) {
        return;  // empty span: nothing to copy (also keeps the byte count >= 0)
    }
    // Each clipped row is one contiguous RGBA8 run -- copy it whole.  One bounds
    // check per row instead of four per pixel: vectorizes, and amortizes the
    // -fbounds-safety per-element cost a byte-by-byte loop would pay in full.
    size_t const span = (size_t)(i1 - i0) * 4;
    for (int j = j0; j < j1; j++) {
        int const s = ((sy + j) * sw + sx + i0) * 4;
        int const d = ((dy + j) * dw + dx + i0) * 4;
        memcpy(dst + d, src + s, span);
    }
}

void canvas2d_blit_f16(_Float16 *__counted_by(dw * dh * 4) dst, int dw, int dh,
                   int dx, int dy,
                   _Float16 const *__counted_by(sw * sh * 4) src, int sw, int sh,
                   int sx, int sy, int w, int h) {
    // Clip identically to canvas2d_blit_rgba (four _Float16 per pixel, not four u8).
    int i0 = 0;
    if (-sx > i0) { i0 = -sx; }
    if (-dx > i0) { i0 = -dx; }
    int i1 = w;
    if (sw - sx < i1) { i1 = sw - sx; }
    if (dw - dx < i1) { i1 = dw - dx; }

    int j0 = 0;
    if (-sy > j0) { j0 = -sy; }
    if (-dy > j0) { j0 = -dy; }
    int j1 = h;
    if (sh - sy < j1) { j1 = sh - sy; }
    if (dh - dy < j1) { j1 = dh - dy; }

    if (i1 <= i0) {
        return;  // empty span: nothing to copy (also keeps the count >= 0)
    }
    size_t const span = (size_t)(i1 - i0) * 4 * sizeof(_Float16);
    for (int j = j0; j < j1; j++) {
        int const s = ((sy + j) * sw + sx + i0) * 4;
        int const d = ((dy + j) * dw + dx + i0) * 4;
        memcpy(dst + d, src + s, span);
    }
}
