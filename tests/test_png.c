#include "cnvs_png.h"
#include "test_util.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t rd32be(uint8_t const *__counted_by(n) b, int n, int off) {
    (void)n;
    return ((uint32_t)b[off] << 24) | ((uint32_t)b[off + 1] << 16) |
           ((uint32_t)b[off + 2] << 8) | (uint32_t)b[off + 3];
}

int main(void) {
    int const w = 4;
    int const h = 3;
    int const len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }
    for (int i = 0; i < len; i++) {
        px[i] = (uint8_t)(i * 7 + 3);
    }

    char const *__null_terminated path = "build/test_png_out.png";
    CHECK(cnvs_png_write(path, px, w, h));
    free(px);

    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f) {
        return TEST_REPORT();
    }
    (void)fseek(f, 0, SEEK_END);
    long sz_l = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    CHECK(sz_l > 33);
    int const sz = (int)sz_l;
    uint8_t *__counted_by(sz) buf = malloc((size_t)sz);
    CHECK(buf != NULL);
    if (!buf) {
        (void)fclose(f);
        return TEST_REPORT();
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    (void)fclose(f);
    CHECK(got == (size_t)sz);

    static uint8_t const sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    for (int i = 0; i < 8; i++) {
        CHECK(buf[i] == sig[i]);
    }

    // IHDR chunk begins at offset 8.
    CHECK(rd32be(buf, sz, 8) == 13u);
    CHECK(buf[12] == 'I' && buf[13] == 'H' && buf[14] == 'D' && buf[15] == 'R');
    CHECK(rd32be(buf, sz, 16) == (uint32_t)w);
    CHECK(rd32be(buf, sz, 20) == (uint32_t)h);

    // IEND is the final 12 bytes.
    int e = sz - 12;
    CHECK(rd32be(buf, sz, e) == 0u);
    CHECK(buf[e + 4] == 'I' && buf[e + 5] == 'E' &&
          buf[e + 6] == 'N' && buf[e + 7] == 'D');

    free(buf);
    return TEST_REPORT();
}
