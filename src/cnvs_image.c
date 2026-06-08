#include "cnvs_image.h"

void cnvs_blit_rgba(uint8_t *__counted_by(dw * dh * 4) dst, int dw, int dh,
                    int dx, int dy,
                    uint8_t const *__counted_by(sw * sh * 4) src, int sw, int sh,
                    int sx, int sy, int w, int h) {
    // Clip the copy rectangle so every src and dst access stays in bounds, then
    // iterate only the overlap (no per-pixel bounds tests needed).
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

    for (int j = j0; j < j1; j++) {
        int srow = ((sy + j) * sw + sx) * 4;
        int drow = ((dy + j) * dw + dx) * 4;
        for (int i = i0; i < i1; i++) {
            int s = srow + i * 4;
            int d = drow + i * 4;
            dst[d + 0] = src[s + 0];
            dst[d + 1] = src[s + 1];
            dst[d + 2] = src[s + 2];
            dst[d + 3] = src[s + 3];
        }
    }
}
