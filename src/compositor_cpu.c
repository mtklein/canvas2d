// Software backend for compositor.h: a pure C, bounds-checked implementation of
// the same interface the Metal shim provides.  No GPU, no system frameworks -- the
// target is a premultiplied cnvs_premul buffer, blend() runs the per-pixel kernel
// (cnvs_blend) over __counted_by tiles, and read() copies the target out.  Pick
// this backend instead of compositor_metal.m at build time (see configure.py).

#include "compositor.h"

#include "cnvs_blend.h"

#include <stdlib.h>
#include <string.h>

struct compositor {
    int width;
    int height;
    cnvs_premul *__counted_by(tn) target;  // premultiplied; starts transparent black
    int tn;                                // target count == width * height
    uint8_t *__counted_by(cn) clip;        // coverage 0..255, NULL = open
    int cn;                                // clip count (0 when open)
};

compositor *__single compositor_create(int width, int height) {
    if (width <= 0 || height <= 0) {
        return NULL;
    }
    compositor *__single c = calloc(1, sizeof *c);  // target/clip NULL, counts 0
    if (!c) {
        return NULL;
    }
    int const n = width * height;
    cnvs_premul *__counted_by(n) t = calloc((size_t)n, sizeof *t);  // 0 == transparent
    if (!t) {
        free(c);
        return NULL;
    }
    c->width = width;
    c->height = height;
    c->tn = n;
    c->target = t;  // count then pointer
    return c;
}

void compositor_destroy(compositor *__single c) {
    if (!c) {
        return;
    }
    free(c->target);
    free(c->clip);
    free(c);
}

void compositor_set_clip(compositor *__single c,
                         uint8_t const *__counted_by(len) mask, int len) {
    if (!c) {
        return;
    }
    if (!mask) {
        free(c->clip);
        c->cn = 0;
        c->clip = NULL;  // open
        return;
    }
    int const n = c->width * c->height;
    if (len < n) {
        return;
    }
    if (!c->clip) {
        uint8_t *__counted_by(n) m = malloc((size_t)n);
        if (!m) {
            return;
        }
        c->cn = n;
        c->clip = m;
    }
    memcpy(c->clip, mask, (size_t)n);
}

void compositor_blend(compositor *__single c, int x, int y, int w, int h,
                      cnvs_premul const *__counted_by(w * h) tile,
                      compositor_blend_mode mode) {
    if (!c || !tile || w <= 0 || h <= 0) {
        return;
    }
    if (x < 0 || y < 0 || x + w > c->width || y + h > c->height) {
        return;
    }
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int di = (y + row) * c->width + (x + col);  // target index, in [0, n)
            cnvs_premul s = tile[row * w + col];
            if (c->clip) {  // attenuate the (premultiplied) source by coverage
                float k = (float)c->clip[di] / 255.0f;
                s.r = (_Float16)((float)s.r * k);
                s.g = (_Float16)((float)s.g * k);
                s.b = (_Float16)((float)s.b * k);
                s.a = (_Float16)((float)s.a * k);
            }
            c->target[di] = cnvs_blend(s, c->target[di], mode);
        }
    }
}

void compositor_read(compositor *__single c, cnvs_premul *__counted_by(len) out, int len) {
    if (!c || !out || len < c->width * c->height) {
        return;
    }
    memcpy(out, c->target, (size_t)(c->width * c->height) * sizeof *out);
}
