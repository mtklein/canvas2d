#include "cnvs_png.h"
#include "cnvs_zlib.h"
#include "test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32be(uint8_t const *__counted_by(n) b, int n, int off) {
    (void)n;
    return ((uint32_t)b[off] << 24) | ((uint32_t)b[off + 1] << 16) |
           ((uint32_t)b[off + 2] << 8) | (uint32_t)b[off + 3];
}

static void wr32be(uint8_t *__counted_by(n) b, int n, int off, uint32_t v) {
    (void)n;
    b[off] = (uint8_t)(v >> 24);
    b[off + 1] = (uint8_t)(v >> 16);
    b[off + 2] = (uint8_t)(v >> 8);
    b[off + 3] = (uint8_t)v;
}

// Test-local byte-table CRC32, independent of the encoder's implementation,
// for re-signing crafted/patched chunks.
static uint32_t tcrc(uint8_t const *__counted_by(n) p, int n) {
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
    }
    return c ^ 0xFFFFFFFFu;
}

// Recompute the CRC of the chunk whose length field starts at `off`.
static void resign(uint8_t *__counted_by(n) b, int n, int off) {
    int const clen = (int)rd32be(b, n, off);
    wr32be(b, n, off + 8 + clen, tcrc(b + off + 4, clen + 4));
}

// Decode must fail cleanly: NULL and all outs zeroed.
static void expect_reject(uint8_t const *__counted_by(n) buf, int n) {
    int w = -1, h = -1, len = -1;
    CHECK(cnvs_png_decode(buf, n, &w, &h, &len) == NULL);
    CHECK(w == 0 && h == 0 && len == 0);
}

// Append one chunk (length + type + payload + CRC) at b[at]; returns new at.
static int put_chunk(uint8_t *__counted_by(n) b, int n, int at,
                     char const *__counted_by(4) type,
                     uint8_t const *__counted_by(plen) payload, int plen) {
    wr32be(b, n, at, (uint32_t)plen);
    for (int i = 0; i < 4; i++) {
        b[at + 4 + i] = (uint8_t)type[i];
    }
    if (plen > 0) {
        memcpy(b + at + 8, payload, (size_t)plen);
    }
    resign(b, n, at);
    return at + 12 + plen;
}

// Build a complete PNG around a hand-built filtered stream (deflated by
// cnvs_zlib), so tests can plant arbitrary per-row filter bytes -- the one
// knob cnvs_png_encode never turns.  Returns total size, or -1.
static int build_png(uint8_t *__counted_by(cap) b, int cap, int w, int h,
                     uint8_t const *__counted_by(rawlen) raw, int rawlen) {
    (void)cap;
    int const zcap = cnvs_zlib_bound(rawlen);
    uint8_t *__counted_by_or_null(zcap) z = malloc((size_t)zcap);
    if (!z) {
        return -1;
    }
    int const zn = cnvs_zlib_deflate(z, zcap, raw, rawlen);
    if (zn < 0) {
        free(z);
        return -1;
    }
    static uint8_t const sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    memcpy(b, sig, sizeof sig);
    uint8_t ihdr[13] = { 0 };
    wr32be(ihdr, 13, 0, (uint32_t)w);
    wr32be(ihdr, 13, 4, (uint32_t)h);
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 6;   // colour type RGBA
    int at = put_chunk(b, cap, 8, "IHDR", ihdr, 13);
    at = put_chunk(b, cap, at, "IDAT", z, zn);
    at = put_chunk(b, cap, at, "IEND", NULL, 0);
    free(z);
    return at;
}

// The encoder's structure: signature, IHDR fields, IEND placement -- plus
// determinism (encode twice, byte-identical) and write == encode + fwrite.
static void test_encode_structure(void) {
    int const w = 4;
    int const h = 3;
    int const len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return;
    }
    for (int i = 0; i < len; i++) {
        px[i] = (uint8_t)(i * 7 + 3);
    }

    int sz = 0;
    uint8_t *enc = cnvs_png_encode(px, w, h, &sz);
    CHECK(enc != NULL && sz > 33);
    if (!enc) {
        free(px);
        return;
    }
    int sz2 = 0;
    uint8_t *enc2 = cnvs_png_encode(px, w, h, &sz2);  // deterministic
    CHECK(enc2 != NULL && sz2 == sz);
    if (enc2 && sz2 == sz) {
        CHECK(memcmp(enc, enc2, (size_t)sz) == 0);
    }
    free(enc2);

    char const *__null_terminated path = "build/test_png_out.png";
    CHECK(cnvs_png_write(path, px, w, h));

    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (f) {
        int const fsz = sz;  // a count decl must sit beside its pointer
        uint8_t *__counted_by_or_null(fsz) file = malloc((size_t)fsz);
        CHECK(file != NULL);
        if (file) {
            CHECK(fread(file, 1, (size_t)fsz, f) == (size_t)fsz);
            CHECK(fgetc(f) == EOF);  // same length...
            CHECK(memcmp(file, enc, (size_t)fsz) == 0);  // ...same bytes
            free(file);
        }
        (void)fclose(f);
    }

    static uint8_t const sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    for (int i = 0; i < 8; i++) {
        CHECK(enc[i] == sig[i]);
    }

    // IHDR chunk begins at offset 8.
    CHECK(rd32be(enc, sz, 8) == 13u);
    CHECK(enc[12] == 'I' && enc[13] == 'H' && enc[14] == 'D' && enc[15] == 'R');
    CHECK(rd32be(enc, sz, 16) == (uint32_t)w);
    CHECK(rd32be(enc, sz, 20) == (uint32_t)h);

    // IEND is the final 12 bytes.
    int e = sz - 12;
    CHECK(rd32be(enc, sz, e) == 0u);
    CHECK(enc[e + 4] == 'I' && enc[e + 5] == 'E' &&
          enc[e + 6] == 'N' && enc[e + 7] == 'D');

    // And the file decodes back to the exact source pixels.
    int dw = 0, dh = 0, dlen = 0;
    uint8_t *back = cnvs_png_read(path, &dw, &dh, &dlen);
    CHECK(back != NULL && dw == w && dh == h && dlen == len);
    if (back && dlen == len) {
        CHECK(memcmp(back, px, (size_t)len) == 0);
    }
    free(back);

    free(enc);
    free(px);
}

// Corruption of a valid encode: magic, every truncation, per-chunk CRC
// damage, trailing garbage, lying chunk lengths.
static void test_reject_corruption(void) {
    int const w = 5, h = 4, len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return;
    }
    for (int i = 0; i < len; i++) {
        px[i] = (uint8_t)(i * 13 + 1);
    }
    int sz = 0;
    uint8_t *enc = cnvs_png_encode(px, w, h, &sz);
    free(px);
    CHECK(enc != NULL && sz > 0);
    if (!enc) {
        return;
    }

    int const cap = sz + 16;
    uint8_t *__counted_by_or_null(cap) m = malloc((size_t)cap);
    CHECK(m != NULL);
    if (!m) {
        free(enc);
        return;
    }

    {  // sanity: the unmodified bytes decode
        int dw = 0, dh = 0, dlen = 0;
        memcpy(m, enc, (size_t)sz);
        uint8_t *p = cnvs_png_decode(m, sz, &dw, &dh, &dlen);
        CHECK(p != NULL && dw == w && dh == h && dlen == len);
        free(p);
    }

    // Every strict prefix fails: that is truncation at every chunk boundary
    // and at every byte within one.
    memcpy(m, enc, (size_t)sz);
    for (int k = 0; k < sz; k++) {
        expect_reject(m, k);
    }

    for (int i = 0; i < 8; i++) {  // bad magic, each byte
        memcpy(m, enc, (size_t)sz);
        m[i] ^= 0x40u;
        expect_reject(m, sz);
    }

    // A flipped bit anywhere in IHDR (fields or CRC), IDAT payload, or the
    // IEND CRC must fail the CRC pairing one way or the other.
    int const idat_off = 8 + 25;
    static int const damage[] = { 8 + 8, 8 + 8 + 13, idat_off + 9, 0 /*IEND crc*/ };
    for (int d = 0; d < 4; d++) {
        memcpy(m, enc, (size_t)sz);
        int off = damage[d] != 0 ? damage[d] : sz - 1;
        m[off] ^= 0x01u;
        expect_reject(m, sz);
    }

    {  // trailing garbage after IEND
        memcpy(m, enc, (size_t)sz);
        m[sz] = 0x00;
        expect_reject(m, sz + 1);
    }

    {  // IDAT length lies long (runs past EOF); resigned so only length trips
        memcpy(m, enc, (size_t)sz);
        wr32be(m, cap, idat_off, rd32be(m, cap, idat_off) + 4u);
        expect_reject(m, sz);
    }

    free(m);
    free(enc);
}

// IHDR field strictness: patch one field at a time and re-sign the CRC, so
// the field check itself -- not the CRC -- must reject.
static void test_reject_ihdr_fields(void) {
    int const w = 3, h = 3, len = w * h * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return;
    }
    memset(px, 0x5A, (size_t)len);
    int sz = 0;
    uint8_t *enc = cnvs_png_encode(px, w, h, &sz);
    free(px);
    CHECK(enc != NULL);
    if (!enc) {
        return;
    }
    int const msz = sz;  // a count decl must sit beside its pointer
    uint8_t *__counted_by_or_null(msz) m = malloc((size_t)msz);
    CHECK(m != NULL);
    if (!m) {
        free(enc);
        return;
    }

    struct {
        int off;      // absolute offset into the file
        uint32_t v;   // 32-bit value to plant (byte fields plant v as one byte)
        bool wide;
    } const cases[] = {
        { 16, 0u, true },           // width 0
        { 20, 0u, true },           // height 0
        { 16, 0x80000000u, true },  // width negative-as-unsigned
        { 20, 0xFFFFFFFFu, true },  // height negative-as-unsigned
        { 16, 16385u, true },       // width past the cap
        { 20, 46341u, true },       // height: w*h*4 would overflow int uncapped
        { 24, 16u, false },         // bit depth 16
        { 24, 1u, false },          // bit depth 1
        { 25, 0u, false },          // colour type 0: grayscale
        { 25, 2u, false },          // colour type 2: RGB
        { 25, 3u, false },          // colour type 3: palette
        { 26, 1u, false },          // nonzero compression method
        { 27, 1u, false },          // nonzero filter method
        { 28, 1u, false },          // interlace: Adam7
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        memcpy(m, enc, (size_t)sz);
        if (cases[i].wide) {
            wr32be(m, sz, cases[i].off, cases[i].v);
        } else {
            m[cases[i].off] = (uint8_t)cases[i].v;
        }
        resign(m, sz, 8);
        expect_reject(m, sz);
    }

    free(m);
    free(enc);
}

// Filter bytes and stream shape, via hand-built filtered streams: only
// None (0) and Up (2) decode; Sub/Avg/Paeth (1/3/4) and junk reject; a
// declared size that disagrees with the stream rejects.
static void test_filter_bytes(void) {
    enum { w = 2, h = 2, stride = w * 4, rawlen = h * (stride + 1) };
    uint8_t const row0[stride] = { 10, 20, 30, 40, 50, 60, 70, 80 };
    uint8_t const row1[stride] = { 11, 22, 33, 44, 55, 66, 77, 88 };
    uint8_t raw[rawlen];
    uint8_t png[256];
    int dw, dh, dlen;

    {  // None on both rows: pixels pass through
        raw[0] = 0;
        memcpy(raw + 1, row0, stride);
        raw[stride + 1] = 0;
        memcpy(raw + stride + 2, row1, stride);
        int n = build_png(png, (int)sizeof png, w, h, raw, rawlen);
        CHECK(n > 0);
        uint8_t *p = cnvs_png_decode(png, n, &dw, &dh, &dlen);
        CHECK(p != NULL && dw == w && dh == h && dlen == w * h * 4);
        if (p && dlen == w * h * 4) {
            CHECK(memcmp(p, row0, stride) == 0);
            CHECK(memcmp(p + stride, row1, stride) == 0);
        }
        free(p);
    }
    {  // Up on row 1: deltas reconstruct row1 from row0
        raw[0] = 0;
        memcpy(raw + 1, row0, stride);
        raw[stride + 1] = 2;
        for (int i = 0; i < stride; i++) {
            raw[stride + 2 + i] = (uint8_t)(row1[i] - row0[i]);
        }
        int n = build_png(png, (int)sizeof png, w, h, raw, rawlen);
        CHECK(n > 0);
        uint8_t *p = cnvs_png_decode(png, n, &dw, &dh, &dlen);
        CHECK(p != NULL);
        if (p && dlen == w * h * 4) {
            CHECK(memcmp(p, row0, stride) == 0);
            CHECK(memcmp(p + stride, row1, stride) == 0);
        }
        free(p);
    }
    // Sub/Avg/Paeth and out-of-range filter bytes reject, on either row.
    static uint8_t const bad[] = { 1, 3, 4, 5, 255 };
    for (size_t i = 0; i < sizeof bad; i++) {
        for (int yrow = 0; yrow < 2; yrow++) {
            raw[0] = 0;
            memcpy(raw + 1, row0, stride);
            raw[stride + 1] = 0;
            memcpy(raw + stride + 2, row1, stride);
            raw[yrow * (stride + 1)] = bad[i];
            int n = build_png(png, (int)sizeof png, w, h, raw, rawlen);
            CHECK(n > 0);
            expect_reject(png, n);
        }
    }
    {  // stream longer than the declared dimensions imply (a 2x2 stream in a
       // 2x1 header) and shorter (2x2 stream, 2x3 header) both reject
        raw[0] = 0;
        memcpy(raw + 1, row0, stride);
        raw[stride + 1] = 0;
        memcpy(raw + stride + 2, row1, stride);
        int n = build_png(png, (int)sizeof png, w, 1, raw, rawlen);
        CHECK(n > 0);
        expect_reject(png, n);
        n = build_png(png, (int)sizeof png, w, 3, raw, rawlen);
        CHECK(n > 0);
        expect_reject(png, n);
    }
}

// Chunk sequencing: consecutive IDATs concatenate; ancillary chunks are
// skipped wherever they sit EXCEPT splitting the IDAT run; unknown critical
// chunks reject; a corrupt ancillary CRC still rejects.
static void test_chunk_sequencing(void) {
    enum { w = 2, h = 1, stride = w * 4, rawlen = h * (stride + 1) };
    uint8_t const row0[stride] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t raw[rawlen];
    raw[0] = 2;  // Up on row 0 == None (zero prior)
    memcpy(raw + 1, row0, stride);

    int const zcap = cnvs_zlib_bound(rawlen);
    uint8_t *__counted_by_or_null(zcap) z = malloc((size_t)zcap);
    CHECK(z != NULL);
    if (!z) {
        return;
    }
    int const zn = cnvs_zlib_deflate(z, zcap, raw, rawlen);
    CHECK(zn > 1);
    if (zn <= 1) {
        free(z);
        return;
    }

    uint8_t ihdr[13] = { 0 };
    wr32be(ihdr, 13, 0, w);
    wr32be(ihdr, 13, 4, h);
    ihdr[8] = 8;
    ihdr[9] = 6;
    static uint8_t const sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    uint8_t const note[4] = { 'h', 'i', '!', 0 };
    enum { cap = 512 };
    uint8_t png[cap];
    int dw, dh, dlen;

    {  // IDAT split in two consecutive chunks: equivalent to one
        memcpy(png, sig, sizeof sig);
        int at = put_chunk(png, cap, 8, "IHDR", ihdr, 13);
        at = put_chunk(png, cap, at, "IDAT", z, 1);
        at = put_chunk(png, cap, at, "IDAT", z + 1, zn - 1);
        at = put_chunk(png, cap, at, "IEND", NULL, 0);
        uint8_t *p = cnvs_png_decode(png, at, &dw, &dh, &dlen);
        CHECK(p != NULL && dw == w && dh == h);
        if (p && dlen == w * h * 4) {
            CHECK(memcmp(p, row0, stride) == 0);
        }
        free(p);
    }
    {  // ancillary chunks before and after the IDAT run: skipped
        memcpy(png, sig, sizeof sig);
        int at = put_chunk(png, cap, 8, "IHDR", ihdr, 13);
        at = put_chunk(png, cap, at, "tEXt", note, 4);
        at = put_chunk(png, cap, at, "IDAT", z, zn);
        at = put_chunk(png, cap, at, "tIME", note, 4);
        at = put_chunk(png, cap, at, "IEND", NULL, 0);
        uint8_t *p = cnvs_png_decode(png, at, &dw, &dh, &dlen);
        CHECK(p != NULL && dw == w && dh == h);
        free(p);
    }
    {  // ...but an ancillary chunk with a bad CRC rejects (CRC checked on all)
        memcpy(png, sig, sizeof sig);
        int at = put_chunk(png, cap, 8, "IHDR", ihdr, 13);
        int text_at = at;
        at = put_chunk(png, cap, at, "tEXt", note, 4);
        png[text_at + 8 + 4] ^= 0x01u;  // CRC byte
        at = put_chunk(png, cap, at, "IDAT", z, zn);
        at = put_chunk(png, cap, at, "IEND", NULL, 0);
        expect_reject(png, at);
    }
    {  // an ancillary chunk splitting the IDAT run rejects
        memcpy(png, sig, sizeof sig);
        int at = put_chunk(png, cap, 8, "IHDR", ihdr, 13);
        at = put_chunk(png, cap, at, "IDAT", z, 1);
        at = put_chunk(png, cap, at, "tEXt", note, 4);
        at = put_chunk(png, cap, at, "IDAT", z + 1, zn - 1);
        at = put_chunk(png, cap, at, "IEND", NULL, 0);
        expect_reject(png, at);
    }
    {  // an unknown critical chunk rejects, valid CRC or not
        memcpy(png, sig, sizeof sig);
        int at = put_chunk(png, cap, 8, "IHDR", ihdr, 13);
        at = put_chunk(png, cap, at, "PLTE", note, 3);
        at = put_chunk(png, cap, at, "IDAT", z, zn);
        at = put_chunk(png, cap, at, "IEND", NULL, 0);
        expect_reject(png, at);
    }
    {  // IEND with a payload rejects; a missing IEND rejects
        memcpy(png, sig, sizeof sig);
        int at = put_chunk(png, cap, 8, "IHDR", ihdr, 13);
        at = put_chunk(png, cap, at, "IDAT", z, zn);
        at = put_chunk(png, cap, at, "IEND", note, 1);
        expect_reject(png, at);
        memcpy(png, sig, sizeof sig);
        at = put_chunk(png, cap, 8, "IHDR", ihdr, 13);
        at = put_chunk(png, cap, at, "IDAT", z, zn);
        expect_reject(png, at);
    }

    free(z);
}

static void test_read_failures(void) {
    int w = -1, h = -1, len = -1;
    CHECK(cnvs_png_read("build/test_png_does_not_exist.png", &w, &h, &len) == NULL);
    CHECK(w == 0 && h == 0 && len == 0);
}

int main(void) {
    test_encode_structure();
    test_reject_corruption();
    test_reject_ihdr_fields();
    test_filter_bytes();
    test_chunk_sequencing();
    test_read_failures();
    return TEST_REPORT();
}
