#include "cnvs_cover.h"
#include "test_util.h"

#include <stdlib.h>

static void add_poly(cnvs_cover *c, int w, int h,
                     float const *__counted_by(2 * n) pts, int n) {
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        cnvs_cover_add_edge(c, w, h, pts[2 * i], pts[2 * i + 1],
                            pts[2 * j], pts[2 * j + 1]);
    }
}

static int cov(uint8_t const *__counted_by(len) px, int len, int w, int x, int y) {
    (void)len;
    return px[y * w + x];
}

int main(void) {
    cnvs_cover c = { .acc = NULL, .cap = 0 };

    // Right triangle (0,0)-(16,0)-(0,16): inside is x+y <= 16.
    {
        int const w = 16, h = 16, len = w * h;
        uint8_t *__counted_by(len) out = malloc((size_t)len);
        CHECK(out != NULL);
        if (out) {
            float tri[6] = { 0, 0, 16, 0, 0, 16 };
            CHECK(cnvs_cover_reset(&c, w, h));
            add_poly(&c, w, h, tri, 3);
            cnvs_cover_resolve(&c, w, h, CNVS_NONZERO, out);
            CHECK(abs(cov(out, len, w, 3, 3) - 255) <= 1);    // interior
            CHECK(abs(cov(out, len, w, 0, 0) - 255) <= 1);    // corner interior
            CHECK(abs(cov(out, len, w, 8, 7) - 128) <= 2);    // 45° edge: half covered
            CHECK(abs(cov(out, len, w, 15, 0) - 128) <= 2);   // hypotenuse tip pixel
            CHECK(cov(out, len, w, 8, 8) <= 1);               // just outside the diagonal
            CHECK(cov(out, len, w, 14, 14) <= 1);             // far outside
            free(out);
        }
    }

    // Rect with a fractional left edge at x=2.5 (right/top/bottom integer).
    {
        int const w = 8, h = 8, len = w * h;
        uint8_t *__counted_by(len) out = malloc((size_t)len);
        CHECK(out != NULL);
        if (out) {
            float r[8] = { 2.5f, 2, 6, 2, 6, 6, 2.5f, 6 };
            CHECK(cnvs_cover_reset(&c, w, h));
            add_poly(&c, w, h, r, 4);
            cnvs_cover_resolve(&c, w, h, CNVS_NONZERO, out);
            CHECK(cov(out, len, w, 1, 3) <= 1);               // left of the edge
            CHECK(abs(cov(out, len, w, 2, 3) - 128) <= 2);    // half-covered column
            CHECK(abs(cov(out, len, w, 3, 3) - 255) <= 1);    // interior
            CHECK(abs(cov(out, len, w, 5, 3) - 255) <= 1);    // last interior column
            CHECK(cov(out, len, w, 6, 3) <= 1);               // right of the edge
            free(out);
        }
    }

    // Even-odd: an inner rect of the same winding punches a hole.
    {
        int const w = 8, h = 8, len = w * h;
        uint8_t *__counted_by(len) out = malloc((size_t)len);
        CHECK(out != NULL);
        if (out) {
            float outer[8] = { 1, 1, 7, 1, 7, 7, 1, 7 };
            float inner[8] = { 3, 3, 5, 3, 5, 5, 3, 5 };
            CHECK(cnvs_cover_reset(&c, w, h));
            add_poly(&c, w, h, outer, 4);
            add_poly(&c, w, h, inner, 4);
            cnvs_cover_resolve(&c, w, h, CNVS_EVENODD, out);
            CHECK(abs(cov(out, len, w, 2, 4) - 255) <= 1);    // ring
            CHECK(cov(out, len, w, 4, 4) <= 1);               // hole
            // Same accumulation under nonzero fills the centre solid.
            cnvs_cover_resolve(&c, w, h, CNVS_NONZERO, out);
            CHECK(abs(cov(out, len, w, 4, 4) - 255) <= 1);
            free(out);
        }
    }

    cnvs_cover_free(&c);
    return TEST_REPORT();
}
