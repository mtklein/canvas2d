#pragma once

// Shared helpers for GPU pixel tests.  static inline so a test may include this
// without using every helper -- an unused static inline is warning-free, even
// under -Weverything.

#include "canvas.h"

#include <stdlib.h>

struct px4 {
    uint8_t r, g, b, a;
};

static inline struct px4 pixel_at(const uint8_t *__counted_by(len) px, int len,
                                  int w, int x, int y) {
    (void)len;
    int o = (y * w + x) * 4;
    return (struct px4){ .r = px[o], .g = px[o + 1],
                         .b = px[o + 2], .a = px[o + 3] };
}

static inline bool px_near(struct px4 p, int r, int g, int b, int a, int tol) {
    return abs((int)p.r - r) <= tol && abs((int)p.g - g) <= tol &&
           abs((int)p.b - b) <= tol && abs((int)p.a - a) <= tol;
}
