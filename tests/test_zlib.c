#include "cnvs_zlib.h"
#include "test_util.h"

#include <stdlib.h>
#include <string.h>

// ---- reference fixtures -----------------------------------------------------
// Compressed with reference zlib (1.2.12, via CPython's zlib module), generated
// once during development with
//   python3 -c 'import zlib; print(zlib.compress(plain, level).hex())'
// and committed as bytes, so inflate is proven against the ecosystem's encoder,
// not just our own.  Three block flavours: level 9 on repeated pangrams emits a
// dynamic-Huffman block (BTYPE=10, verified at generation time), level 0 emits
// a stored block (BTYPE=00 -- the shape every gallery PNG to date carries), and
// level 9 on a tiny string emits a fixed-Huffman block (BTYPE=01).

static uint8_t const fix_dynamic_plain[369] = {
    0x74, 0x68, 0x65, 0x20, 0x71, 0x75, 0x69, 0x63, 0x6B, 0x20, 0x62, 0x72,
    0x6F, 0x77, 0x6E, 0x20, 0x66, 0x6F, 0x78, 0x20, 0x6A, 0x75, 0x6D, 0x70,
    0x73, 0x20, 0x6F, 0x76, 0x65, 0x72, 0x20, 0x74, 0x68, 0x65, 0x20, 0x6C,
    0x61, 0x7A, 0x79, 0x20, 0x64, 0x6F, 0x67, 0x2E, 0x20, 0x70, 0x61, 0x63,
    0x6B, 0x20, 0x6D, 0x79, 0x20, 0x62, 0x6F, 0x78, 0x20, 0x77, 0x69, 0x74,
    0x68, 0x20, 0x66, 0x69, 0x76, 0x65, 0x20, 0x64, 0x6F, 0x7A, 0x65, 0x6E,
    0x20, 0x6C, 0x69, 0x71, 0x75, 0x6F, 0x72, 0x20, 0x6A, 0x75, 0x67, 0x73,
    0x2E, 0x20, 0x68, 0x6F, 0x77, 0x20, 0x76, 0x65, 0x78, 0x69, 0x6E, 0x67,
    0x6C, 0x79, 0x20, 0x71, 0x75, 0x69, 0x63, 0x6B, 0x20, 0x64, 0x61, 0x66,
    0x74, 0x20, 0x7A, 0x65, 0x62, 0x72, 0x61, 0x73, 0x20, 0x6A, 0x75, 0x6D,
    0x70, 0x21, 0x20, 0x74, 0x68, 0x65, 0x20, 0x71, 0x75, 0x69, 0x63, 0x6B,
    0x20, 0x62, 0x72, 0x6F, 0x77, 0x6E, 0x20, 0x66, 0x6F, 0x78, 0x20, 0x6A,
    0x75, 0x6D, 0x70, 0x73, 0x20, 0x6F, 0x76, 0x65, 0x72, 0x20, 0x74, 0x68,
    0x65, 0x20, 0x6C, 0x61, 0x7A, 0x79, 0x20, 0x64, 0x6F, 0x67, 0x2E, 0x20,
    0x70, 0x61, 0x63, 0x6B, 0x20, 0x6D, 0x79, 0x20, 0x62, 0x6F, 0x78, 0x20,
    0x77, 0x69, 0x74, 0x68, 0x20, 0x66, 0x69, 0x76, 0x65, 0x20, 0x64, 0x6F,
    0x7A, 0x65, 0x6E, 0x20, 0x6C, 0x69, 0x71, 0x75, 0x6F, 0x72, 0x20, 0x6A,
    0x75, 0x67, 0x73, 0x2E, 0x20, 0x68, 0x6F, 0x77, 0x20, 0x76, 0x65, 0x78,
    0x69, 0x6E, 0x67, 0x6C, 0x79, 0x20, 0x71, 0x75, 0x69, 0x63, 0x6B, 0x20,
    0x64, 0x61, 0x66, 0x74, 0x20, 0x7A, 0x65, 0x62, 0x72, 0x61, 0x73, 0x20,
    0x6A, 0x75, 0x6D, 0x70, 0x21, 0x20, 0x74, 0x68, 0x65, 0x20, 0x71, 0x75,
    0x69, 0x63, 0x6B, 0x20, 0x62, 0x72, 0x6F, 0x77, 0x6E, 0x20, 0x66, 0x6F,
    0x78, 0x20, 0x6A, 0x75, 0x6D, 0x70, 0x73, 0x20, 0x6F, 0x76, 0x65, 0x72,
    0x20, 0x74, 0x68, 0x65, 0x20, 0x6C, 0x61, 0x7A, 0x79, 0x20, 0x64, 0x6F,
    0x67, 0x2E, 0x20, 0x70, 0x61, 0x63, 0x6B, 0x20, 0x6D, 0x79, 0x20, 0x62,
    0x6F, 0x78, 0x20, 0x77, 0x69, 0x74, 0x68, 0x20, 0x66, 0x69, 0x76, 0x65,
    0x20, 0x64, 0x6F, 0x7A, 0x65, 0x6E, 0x20, 0x6C, 0x69, 0x71, 0x75, 0x6F,
    0x72, 0x20, 0x6A, 0x75, 0x67, 0x73, 0x2E, 0x20, 0x68, 0x6F, 0x77, 0x20,
    0x76, 0x65, 0x78, 0x69, 0x6E, 0x67, 0x6C, 0x79, 0x20, 0x71, 0x75, 0x69,
    0x63, 0x6B, 0x20, 0x64, 0x61, 0x66, 0x74, 0x20, 0x7A, 0x65, 0x62, 0x72,
    0x61, 0x73, 0x20, 0x6A, 0x75, 0x6D, 0x70, 0x21, 0x20,
};
static uint8_t const fix_dynamic_zlib[107] = {
    0x78, 0xDA, 0xE5, 0x8D, 0xDB, 0x11, 0x84, 0x20, 0x10, 0x04, 0x53, 0x99,
    0x4B, 0xC0, 0x9C, 0x40, 0x17, 0x58, 0x0F, 0x59, 0xE5, 0x29, 0x44, 0x7F,
    0x94, 0x65, 0x16, 0xF7, 0xDD, 0x3D, 0x3D, 0xD9, 0x11, 0xAE, 0xC2, 0xEB,
    0x17, 0x3A, 0x4A, 0x0B, 0x30, 0x72, 0x63, 0x2F, 0xC7, 0x99, 0x20, 0x95,
    0x22, 0xF2, 0xC4, 0x5E, 0x8D, 0x8E, 0x4D, 0xEC, 0x82, 0x53, 0x4D, 0xEF,
    0xE8, 0xD0, 0x53, 0x6A, 0x9C, 0x1D, 0x0C, 0x57, 0x9A, 0x68, 0x50, 0x80,
    0xE7, 0xAB, 0x48, 0x9C, 0x5B, 0x9B, 0x16, 0x38, 0x69, 0xA8, 0x74, 0x73,
    0xB0, 0xBE, 0xBF, 0xF9, 0x4D, 0x99, 0x8C, 0x41, 0x3A, 0xAA, 0xF4, 0x1C,
    0x7C, 0x9E, 0xF6, 0xFF, 0x5D, 0xFF, 0x00, 0x37, 0x34, 0x86, 0x47,
};
static uint8_t const fix_stored_plain[48] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23,
    0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
};
static uint8_t const fix_stored_zlib[59] = {
    0x78, 0x01, 0x01, 0x30, 0x00, 0xCF, 0xFF, 0x00, 0x01, 0x02, 0x03, 0x04,
    0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C,
    0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x48, 0x28, 0x04, 0x69,
};
static uint8_t const fix_fixed_plain[8] = {
    0x63, 0x61, 0x6E, 0x76, 0x61, 0x73, 0x32, 0x64,  // "canvas2d"
};
static uint8_t const fix_fixed_zlib[16] = {
    0x78, 0xDA, 0x4B, 0x4E, 0xCC, 0x2B, 0x4B, 0x2C, 0x36, 0x4A, 0x01, 0x00,
    0x0E, 0x4E, 0x03, 0x13,
};

// ---- helpers ----------------------------------------------------------------

// Deterministic bytes for the incompressible case (no rand(): same on every
// run).  xorshift32 in the house spelling: run in 64-bit and masked so the
// deliberate wrap stays out of -fsanitize=integer's lane (test_emoji pattern).
static uint32_t g_rng = 0x2545F491u;
static uint8_t rnd8(void) {
    uint64_t x = g_rng;
    x = (x ^ (x << 13)) & 0xFFFFFFFFu;
    x = x ^ (x >> 17);
    x = (x ^ (x << 5)) & 0xFFFFFFFFu;
    g_rng = (uint32_t)x;
    return (uint8_t)(x & 0xFFu);
}

// Bit-stream builder for hand-crafted deflate streams: bw_put writes LSB-first
// fields (headers, extra bits), bw_code writes Huffman codes MSB-first (RFC
// 1951 packs them reversed).
struct bw {
    uint8_t buf[512];
    int at;
    uint32_t acc;
    int nbits;
};

static void bw_put(struct bw *w, uint32_t v, int cnt) {
    w->acc |= v << w->nbits;
    w->nbits += cnt;
    while (w->nbits >= 8) {
        w->buf[w->at] = (uint8_t)(w->acc & 0xFFu);
        w->at += 1;
        w->acc >>= 8;
        w->nbits -= 8;
    }
}

static void bw_code(struct bw *w, uint32_t v, int cnt) {
    for (int i = cnt - 1; i >= 0; i--) {
        bw_put(w, (v >> i) & 1u, 1);
    }
}

static void bw_align(struct bw *w) {
    if (w->nbits != 0) {
        bw_put(w, 0, 8 - w->nbits);
    }
}

static void bw_be32(struct bw *w, uint32_t v) {
    bw_put(w, (v >> 24) & 0xFFu, 8);
    bw_put(w, (v >> 16) & 0xFFu, 8);
    bw_put(w, (v >> 8) & 0xFFu, 8);
    bw_put(w, v & 0xFFu, 8);
}

static void bw_hdr(struct bw *w) {  // the 0x78 0x01 zlib header
    bw_put(w, 0x78u, 8);
    bw_put(w, 0x01u, 8);
}

static void bw_lit(struct bw *w, int sym) {  // fixed-code literal, 0..255
    if (sym < 144) {
        bw_code(w, 0x30u + (uint32_t)sym, 8);
    } else {
        bw_code(w, 0x190u + (uint32_t)(sym - 144), 9);
    }
}

static void bw_eob(struct bw *w) {
    bw_code(w, 0, 7);
}

// Inflate the crafted stream into a roomy buffer and expect rejection.
static void expect_reject(struct bw *w) {
    uint8_t out[64];
    bw_align(w);
    CHECK(cnvs_zlib_inflate(out, (int)sizeof out, w->buf, w->at) == -1);
}

// Deflate + inflate must reproduce src exactly; deflate twice must be
// byte-identical (determinism); the deflated size must respect the bound.
static void round_trip(uint8_t const *__counted_by(n) src, int n) {
    int const zcap = cnvs_zlib_bound(n);
    int const bcap = n > 0 ? n : 1;
    uint8_t *__counted_by_or_null(zcap) z = malloc((size_t)zcap);
    uint8_t *__counted_by_or_null(zcap) z2 = malloc((size_t)zcap);
    uint8_t *__counted_by_or_null(bcap) back = malloc((size_t)bcap);
    CHECK(z != NULL && z2 != NULL && back != NULL);
    if (z && z2 && back) {
        int zn = cnvs_zlib_deflate(z, zcap, src, n);
        CHECK(zn > 0);
        CHECK(zn <= zcap);
        int zn2 = cnvs_zlib_deflate(z2, zcap, src, n);
        CHECK(zn2 == zn);
        if (zn > 0 && zn2 == zn) {
            CHECK(memcmp(z, z2, (size_t)zn) == 0);
            CHECK(cnvs_zlib_inflate(back, n, z, zn) == n);  // exact-size dst
            CHECK(n == 0 || memcmp(back, src, (size_t)n) == 0);
        }
    }
    free(back);
    free(z2);
    free(z);
}

// A reference-zlib stream must inflate to its known plaintext (at exact and
// slack dst caps), reject every truncation, reject trailing garbage and a
// corrupted adler, and overflow cleanly into a too-small dst.
static void fixture_check(uint8_t const *__counted_by(zn) zs, int zn,
                          uint8_t const *__counted_by(pn) plain, int pn) {
    int const cap = pn + 8;
    int const gn = zn + 1;
    uint8_t *__counted_by_or_null(cap) out = malloc((size_t)cap);
    uint8_t *__counted_by_or_null(gn) g = malloc((size_t)gn);
    CHECK(out != NULL && g != NULL);
    if (out && g) {
        CHECK(cnvs_zlib_inflate(out, pn, zs, zn) == pn);
        CHECK(memcmp(out, plain, (size_t)pn) == 0);
        CHECK(cnvs_zlib_inflate(out, cap, zs, zn) == pn);  // dst larger than needed
        CHECK(memcmp(out, plain, (size_t)pn) == 0);
        CHECK(cnvs_zlib_inflate(out, pn - 1, zs, zn) == -1);  // dst one byte short
        for (int k = 0; k < zn; k++) {  // every truncation fails cleanly
            CHECK(cnvs_zlib_inflate(out, cap, zs, k) == -1);
        }
        memcpy(g, zs, (size_t)zn);  // trailing garbage
        g[zn] = 0x00;
        CHECK(cnvs_zlib_inflate(out, cap, g, gn) == -1);
        g[zn - 1] ^= 0x01;  // bad adler, low byte
        CHECK(cnvs_zlib_inflate(out, cap, g, zn) == -1);
        g[zn - 1] ^= 0x01;
        g[zn - 4] ^= 0x80;  // bad adler, high byte
        CHECK(cnvs_zlib_inflate(out, cap, g, zn) == -1);
    }
    free(g);
    free(out);
}

// ---- the tests --------------------------------------------------------------

static void test_round_trips(void) {
    uint8_t one = 0x42;
    round_trip(&one, 0);  // empty input is a valid (tiny) stream
    round_trip(&one, 1);

    enum { big = 200001, zeros = 100000, ab = 80000, rnd = 70001, rgba = 160 * 160 * 4 };
    int const cap = big;
    uint8_t *__counted_by_or_null(cap) buf = malloc((size_t)cap);
    CHECK(buf != NULL);
    if (!buf) {
        return;
    }

    memset(buf, 0, zeros);  // long runs -> overlapping dist-1 copies
    round_trip(buf, zeros);

    for (int i = 0; i < ab; i++) {  // dist-2 overlap, the classic
        buf[i] = (uint8_t)((i & 1) ? 'b' : 'a');
    }
    round_trip(buf, ab);

    for (int i = 0; i < big; i++) {  // text, crossing 64K, odd length
        buf[i] = fix_dynamic_plain[i % (int)sizeof fix_dynamic_plain];
    }
    round_trip(buf, big);

    for (int i = 0; i < rnd; i++) {  // incompressible: proves the bound holds
        buf[i] = rnd8();
    }
    round_trip(buf, rnd);

    for (int y = 0; y < 160; y++) {  // emoji-capture-like RGBA: mostly
        for (int x = 0; x < 160; x++) {  // transparent, a gradient disc of ink
            int o = (y * 160 + x) * 4;
            if ((x - 80) * (x - 80) + (y - 80) * (y - 80) < 60 * 60) {
                buf[o + 0] = (uint8_t)x;
                buf[o + 1] = (uint8_t)y;
                buf[o + 2] = (uint8_t)((x + y) / 2);
                buf[o + 3] = 255;
            } else {
                buf[o + 0] = buf[o + 1] = buf[o + 2] = buf[o + 3] = 0;
            }
        }
    }
    round_trip(buf, rgba);

    for (int i = 0; i < cap; i++) {  // lengths bracketing the 16-bit boundaries
        buf[i] = (uint8_t)((i % 251) ^ (i >> 6));
    }
    static int const sizes[] = { 2, 3, 255, 256, 257, 65534, 65535, 65536, 65537 };
    for (int s = 0; s < (int)(sizeof sizes / sizeof sizes[0]); s++) {
        round_trip(buf, sizes[s]);
    }

    free(buf);
}

static void test_deflate_dst_too_small(void) {
    uint8_t z[8];
    CHECK(cnvs_zlib_deflate(z, (int)sizeof z, fix_fixed_plain,
                            (int)sizeof fix_fixed_plain) == -1);
    CHECK(cnvs_zlib_deflate(z, 0, fix_fixed_plain, (int)sizeof fix_fixed_plain) == -1);
}

static void test_fixtures(void) {
    fixture_check(fix_dynamic_zlib, (int)sizeof fix_dynamic_zlib,
                  fix_dynamic_plain, (int)sizeof fix_dynamic_plain);
    fixture_check(fix_stored_zlib, (int)sizeof fix_stored_zlib,
                  fix_stored_plain, (int)sizeof fix_stored_plain);
    fixture_check(fix_fixed_zlib, (int)sizeof fix_fixed_zlib,
                  fix_fixed_plain, (int)sizeof fix_fixed_plain);
}

// A multi-block stored stream, the exact shape cnvs_png's emit_zlib writes for
// every gallery PNG to date: 65535-byte blocks, last one final.
static void test_multiblock_stored(void) {
    enum { pn = 70000 };
    int const zn = 2 + 5 + 65535 + 5 + (pn - 65535) + 4;
    uint8_t *__counted_by_or_null(pn) plain = malloc((size_t)pn);
    uint8_t *__counted_by_or_null(zn) zs = malloc((size_t)zn);
    uint8_t *__counted_by_or_null(pn) out = malloc((size_t)pn);
    CHECK(plain != NULL && zs != NULL && out != NULL);
    if (plain && zs && out) {
        for (int i = 0; i < pn; i++) {
            plain[i] = (uint8_t)((i * 31) ^ (i >> 8));
        }
        int at = 0;
        zs[at++] = 0x78;
        zs[at++] = 0x01;
        zs[at++] = 0x00;  // BFINAL=0, BTYPE=00
        zs[at++] = 0xFF;  // LEN = 65535, little-endian
        zs[at++] = 0xFF;
        zs[at++] = 0x00;  // NLEN = ~LEN
        zs[at++] = 0x00;
        memcpy(zs + at, plain, 65535);
        at += 65535;
        int const rest = pn - 65535;
        int const nrest = 0xFFFF ^ rest;  // NLEN = ~LEN as a 16-bit field
        zs[at++] = 0x01;  // BFINAL=1, BTYPE=00
        zs[at++] = (uint8_t)(rest & 0xFF);
        zs[at++] = (uint8_t)(rest >> 8);
        zs[at++] = (uint8_t)(nrest & 0xFF);
        zs[at++] = (uint8_t)(nrest >> 8);
        memcpy(zs + at, plain + 65535, (size_t)rest);
        at += rest;
        uint32_t adler = cnvs_zlib_adler32(plain, pn);
        zs[at++] = (uint8_t)(adler >> 24);
        zs[at++] = (uint8_t)(adler >> 16);
        zs[at++] = (uint8_t)(adler >> 8);
        zs[at++] = (uint8_t)adler;
        CHECK(at == zn);
        CHECK(cnvs_zlib_inflate(out, pn, zs, zn) == pn);
        CHECK(memcmp(out, plain, pn) == 0);
    }
    free(out);
    free(zs);
    free(plain);
}

// Hand-crafted fixed-Huffman streams: the overlapping copy (distance < length),
// the distance == bytes-written boundary, and block-spanning output.
static void test_crafted_fixed(void) {
    uint8_t out[64];

    {  // "ab" + match(len 20, dist 2) -> "ab" * 11, the classic overlap
        struct bw w = { 0 };
        bw_hdr(&w);
        bw_put(&w, 1, 1);  // BFINAL
        bw_put(&w, 1, 2);  // BTYPE=01
        bw_lit(&w, 'a');
        bw_lit(&w, 'b');
        bw_code(&w, 269 - 256, 7);  // length code 269: base 19
        bw_put(&w, 1, 2);           // +1 -> 20
        bw_code(&w, 1, 5);          // distance code 1: dist 2
        bw_eob(&w);
        bw_align(&w);
        uint8_t want[22];
        for (int i = 0; i < 22; i++) {
            want[i] = (uint8_t)((i & 1) ? 'b' : 'a');
        }
        bw_be32(&w, cnvs_zlib_adler32(want, sizeof want));
        CHECK(cnvs_zlib_inflate(out, (int)sizeof out, w.buf, w.at) == 22);
        CHECK(memcmp(out, want, sizeof want) == 0);
        CHECK(cnvs_zlib_inflate(out, 21, w.buf, w.at) == -1);  // cap mid-copy
        CHECK(cnvs_zlib_inflate(out, 2, w.buf, w.at) == -1);   // cap before copy
    }
    {  // 'x' + match(len 3, dist 1) -> "xxxx": dist == bytes written, legal
        struct bw w = { 0 };
        bw_hdr(&w);
        bw_put(&w, 1, 1);
        bw_put(&w, 1, 2);
        bw_lit(&w, 'x');
        bw_code(&w, 257 - 256, 7);  // length 3
        bw_code(&w, 0, 5);          // dist 1
        bw_eob(&w);
        bw_align(&w);
        uint8_t const want[4] = { 'x', 'x', 'x', 'x' };
        bw_be32(&w, cnvs_zlib_adler32(want, sizeof want));
        CHECK(cnvs_zlib_inflate(out, (int)sizeof out, w.buf, w.at) == 4);
        CHECK(memcmp(out, want, sizeof want) == 0);
    }
    {  // two fixed blocks, "Hi" then "!": output runs across the block seam
        struct bw w = { 0 };
        bw_hdr(&w);
        bw_put(&w, 0, 1);  // BFINAL=0
        bw_put(&w, 1, 2);
        bw_lit(&w, 'H');
        bw_lit(&w, 'i');
        bw_eob(&w);
        bw_put(&w, 1, 1);  // BFINAL=1
        bw_put(&w, 1, 2);
        bw_lit(&w, '!');
        bw_eob(&w);
        bw_align(&w);
        uint8_t const want[3] = { 'H', 'i', '!' };
        bw_be32(&w, cnvs_zlib_adler32(want, sizeof want));
        CHECK(cnvs_zlib_inflate(out, (int)sizeof out, w.buf, w.at) == 3);
        CHECK(memcmp(out, want, sizeof want) == 0);
    }
}

static void test_bad_header(void) {
    uint8_t out[16];
    uint8_t z[sizeof fix_fixed_zlib];
    int const zn = (int)sizeof z;

    memcpy(z, fix_fixed_zlib, sizeof z);
    z[0] = 0x77;  // CM != 8
    CHECK(cnvs_zlib_inflate(out, (int)sizeof out, z, zn) == -1);

    memcpy(z, fix_fixed_zlib, sizeof z);
    z[0] = 0x88;  // CINFO > 7 (window beyond 32K)
    CHECK(cnvs_zlib_inflate(out, (int)sizeof out, z, zn) == -1);

    memcpy(z, fix_fixed_zlib, sizeof z);
    z[1] ^= 0x01;  // FCHECK no longer a multiple of 31
    CHECK(cnvs_zlib_inflate(out, (int)sizeof out, z, zn) == -1);

    memcpy(z, fix_fixed_zlib, sizeof z);
    z[1] = 0x20;  // FDICT set; 0x7820 IS a multiple of 31, so only FDICT trips
    CHECK(cnvs_zlib_inflate(out, (int)sizeof out, z, zn) == -1);

    uint8_t tiny[2] = { 0x78, 0x01 };  // header only: no block bits at all
    CHECK(cnvs_zlib_inflate(out, (int)sizeof out, tiny, 0) == -1);
    CHECK(cnvs_zlib_inflate(out, (int)sizeof out, tiny, 1) == -1);
    CHECK(cnvs_zlib_inflate(out, (int)sizeof out, tiny, 2) == -1);

    uint8_t reserved[3] = { 0x78, 0x01, 0x07 };  // BFINAL=1, BTYPE=11
    CHECK(cnvs_zlib_inflate(out, (int)sizeof out, reserved, 3) == -1);
}

static void test_bad_stored(void) {
    uint8_t out[16];
    // NLEN is not ~LEN.
    uint8_t bad[11] = { 0x78, 0x01, 0x01, 0x05, 0x00, 0xFB, 0xFF, 'a', 'b', 'c', 'd' };
    CHECK(cnvs_zlib_inflate(out, (int)sizeof out, bad, (int)sizeof bad) == -1);
    // LEN = 5 but only 2 payload bytes present.
    uint8_t cut[9] = { 0x78, 0x01, 0x01, 0x05, 0x00, 0xFA, 0xFF, 'a', 'b' };
    CHECK(cnvs_zlib_inflate(out, (int)sizeof out, cut, (int)sizeof cut) == -1);
}

static void test_bad_distances(void) {
    {  // 'a' then match(len 3, dist 2): reaches before the start of output
        struct bw w = { 0 };
        bw_hdr(&w);
        bw_put(&w, 1, 1);
        bw_put(&w, 1, 2);
        bw_lit(&w, 'a');
        bw_code(&w, 257 - 256, 7);
        bw_code(&w, 1, 5);  // dist 2 > 1 byte written
        expect_reject(&w);
    }
    {  // match as the very first symbol: any distance is too far
        struct bw w = { 0 };
        bw_hdr(&w);
        bw_put(&w, 1, 1);
        bw_put(&w, 1, 2);
        bw_code(&w, 257 - 256, 7);
        bw_code(&w, 0, 5);  // dist 1, output empty
        expect_reject(&w);
    }
    {  // distance code 30: encodable in the fixed code, but unassigned
        struct bw w = { 0 };
        bw_hdr(&w);
        bw_put(&w, 1, 1);
        bw_put(&w, 1, 2);
        bw_lit(&w, 'a');
        bw_code(&w, 257 - 256, 7);
        bw_code(&w, 30, 5);
        expect_reject(&w);
    }
    {  // literal/length code 286: encodable in the fixed code, but unassigned
        struct bw w = { 0 };
        bw_hdr(&w);
        bw_put(&w, 1, 1);
        bw_put(&w, 1, 2);
        bw_code(&w, 0xC0u + (286 - 280), 8);
        expect_reject(&w);
    }
}

// The dynamic-Huffman rejection set: every stream below carries a structurally
// poisoned code-length payload that must be refused arithmetically.  Streams
// are built bit-exact with bw_code emitting each crafted CL code MSB-first.
static void test_bad_dynamic(void) {
    {  // oversubscribed code-length code: three 1-bit CL codes
        struct bw w = { 0 };
        bw_hdr(&w);
        bw_put(&w, 1, 1);
        bw_put(&w, 2, 2);  // BTYPE=10
        bw_put(&w, 0, 5);  // HLIT  -> 257
        bw_put(&w, 0, 5);  // HDIST -> 1
        bw_put(&w, 0, 4);  // HCLEN -> 4 entries: lengths for CL symbols 16,17,18,0
        bw_put(&w, 1, 3);
        bw_put(&w, 1, 3);
        bw_put(&w, 1, 3);
        bw_put(&w, 0, 3);
        expect_reject(&w);
    }
    {  // incomplete code-length code: a single 1-bit CL code (must be complete)
        struct bw w = { 0 };
        bw_hdr(&w);
        bw_put(&w, 1, 1);
        bw_put(&w, 2, 2);
        bw_put(&w, 0, 5);
        bw_put(&w, 0, 5);
        bw_put(&w, 0, 4);
        bw_put(&w, 1, 3);
        bw_put(&w, 0, 3);
        bw_put(&w, 0, 3);
        bw_put(&w, 0, 3);
        expect_reject(&w);
    }
    {  // repeat-previous (16) with no previous length
        struct bw w = { 0 };
        bw_hdr(&w);
        bw_put(&w, 1, 1);
        bw_put(&w, 2, 2);
        bw_put(&w, 0, 5);
        bw_put(&w, 0, 5);
        bw_put(&w, 0, 4);  // CL lens: sym16=1, sym17=1 -> codes 16->0, 17->1
        bw_put(&w, 1, 3);
        bw_put(&w, 1, 3);
        bw_put(&w, 0, 3);
        bw_put(&w, 0, 3);
        bw_code(&w, 0, 1);  // symbol 16 first: nothing to repeat
        bw_put(&w, 0, 2);
        expect_reject(&w);
    }
    {  // zero-repeat (18) runs past the combined length list
        struct bw w = { 0 };
        bw_hdr(&w);
        bw_put(&w, 1, 1);
        bw_put(&w, 2, 2);
        bw_put(&w, 0, 5);
        bw_put(&w, 0, 5);
        bw_put(&w, 0, 4);  // CL lens: sym17=1, sym18=1 -> codes 17->0, 18->1
        bw_put(&w, 0, 3);
        bw_put(&w, 1, 3);
        bw_put(&w, 1, 3);
        bw_put(&w, 0, 3);
        bw_code(&w, 1, 1);    // 18: 138 zeros (got 138)
        bw_put(&w, 127, 7);
        bw_code(&w, 1, 1);    // 18: 138 more > the 120 slots left of 258
        bw_put(&w, 127, 7);
        expect_reject(&w);
    }
    // The remaining cases share a CL code over {0, 1, 2, 18} with lengths
    // {2, 3, 3, 1}: canonical codes 18->0, 0->10, 1->110, 2->111.  Permutation
    // indices: 18@2, 0@3, 2@15, 1@17, so HCLEN=14 sends 18 entries.
    {  // oversubscribed literal/length code: four 1-bit codes (EOB present)
        struct bw w = { 0 };
        bw_hdr(&w);
        bw_put(&w, 1, 1);
        bw_put(&w, 2, 2);
        bw_put(&w, 0, 5);    // HLIT  -> 257
        bw_put(&w, 0, 5);    // HDIST -> 1
        bw_put(&w, 14, 4);   // HCLEN -> 18
        for (int i = 0; i < 18; i++) {
            int v = i == 2 ? 1 : i == 3 ? 2 : i == 15 ? 3 : i == 17 ? 3 : 0;
            bw_put(&w, (uint32_t)v, 3);
        }
        bw_code(&w, 6, 3);   // len[0]=1
        bw_code(&w, 6, 3);   // len[1]=1
        bw_code(&w, 6, 3);   // len[2]=1
        bw_code(&w, 0, 1);   // 18: 138 zeros        (got 141)
        bw_put(&w, 127, 7);
        bw_code(&w, 0, 1);   // 18: 115 zeros        (got 256)
        bw_put(&w, 104, 7);
        bw_code(&w, 6, 3);   // len[256]=1 -> count[1]=4: oversubscribed
        bw_code(&w, 2, 2);   // dist len = 0
        expect_reject(&w);
    }
    {  // incomplete literal/length code that is NOT a single 1-bit code
        struct bw w = { 0 };
        bw_hdr(&w);
        bw_put(&w, 1, 1);
        bw_put(&w, 2, 2);
        bw_put(&w, 0, 5);
        bw_put(&w, 0, 5);
        bw_put(&w, 14, 4);
        for (int i = 0; i < 18; i++) {
            int v = i == 2 ? 1 : i == 3 ? 2 : i == 15 ? 3 : i == 17 ? 3 : 0;
            bw_put(&w, (uint32_t)v, 3);
        }
        bw_code(&w, 6, 3);   // len[0]=1
        bw_code(&w, 0, 1);   // 18: 138 zeros        (got 139)
        bw_put(&w, 127, 7);
        bw_code(&w, 0, 1);   // 18: 117 zeros        (got 256)
        bw_put(&w, 106, 7);
        bw_code(&w, 7, 3);   // len[256]=2 -> {1-bit, 2-bit}: incomplete, not single
        bw_code(&w, 2, 2);   // dist len = 0
        expect_reject(&w);
    }
    {  // end-of-block symbol has no code: the block could never terminate
        struct bw w = { 0 };
        bw_hdr(&w);
        bw_put(&w, 1, 1);
        bw_put(&w, 2, 2);
        bw_put(&w, 0, 5);
        bw_put(&w, 0, 5);
        bw_put(&w, 14, 4);
        for (int i = 0; i < 18; i++) {
            int v = i == 2 ? 1 : i == 3 ? 2 : i == 15 ? 3 : i == 17 ? 3 : 0;
            bw_put(&w, (uint32_t)v, 3);
        }
        bw_code(&w, 6, 3);   // len[0]=1
        bw_code(&w, 6, 3);   // len[1]=1 (complete 1-bit pair)
        bw_code(&w, 0, 1);   // 18: 138 zeros        (got 140)
        bw_put(&w, 127, 7);
        bw_code(&w, 0, 1);   // 18: 118 zeros        (got 258, len[256]=0)
        bw_put(&w, 107, 7);
        expect_reject(&w);
    }
}

// A dynamic block whose distance code is a single 1-bit code -- incomplete, but
// the one shape real encoders emit (zlib's one-distance streams), so it must be
// ACCEPTED; its unclaimed code space must still be rejected if used.  Litlen
// code: 'a'->0 (1 bit), 256->10, 257->11; dist code: dist 1 -> 0.
static void test_dynamic_single_dist(void) {
    uint8_t out[16];
    for (int poison = 0; poison < 2; poison++) {
        struct bw w = { 0 };
        bw_hdr(&w);
        bw_put(&w, 1, 1);
        bw_put(&w, 2, 2);   // BTYPE=10
        bw_put(&w, 1, 5);   // HLIT  -> 258 (needs symbol 257)
        bw_put(&w, 0, 5);   // HDIST -> 1
        bw_put(&w, 14, 4);  // HCLEN -> 18
        // CL code over values {1, 2, 18}, lengths {1, 2, 2}: codes 1->0,
        // 2->10, 18->11.  Permutation indices: 18@2, 2@15, 1@17.
        for (int i = 0; i < 18; i++) {
            int v = i == 2 ? 2 : i == 15 ? 2 : i == 17 ? 1 : 0;
            bw_put(&w, (uint32_t)v, 3);
        }
        bw_code(&w, 3, 2);  // 18: 97 zeros   (lens[0..96] = 0)
        bw_put(&w, 86, 7);
        bw_code(&w, 0, 1);  // lens[97] = 1   ('a')
        bw_code(&w, 3, 2);  // 18: 138 zeros  (lens[98..235] = 0)
        bw_put(&w, 127, 7);
        bw_code(&w, 3, 2);  // 18: 20 zeros   (lens[236..255] = 0)
        bw_put(&w, 9, 7);
        bw_code(&w, 2, 2);  // lens[256] = 2  (EOB)
        bw_code(&w, 2, 2);  // lens[257] = 2  (length-3 code)
        bw_code(&w, 0, 1);  // dist lens[0] = 1: the single 1-bit code
        bw_code(&w, 0, 1);  // 'a'
        bw_code(&w, 3, 2);  // length code 257: 3 bytes from...
        bw_code(&w, (uint32_t)poison, 1);  // ...dist 1 (0), or the dead space (1)
        if (poison) {
            expect_reject(&w);
            continue;
        }
        bw_code(&w, 2, 2);  // EOB
        bw_align(&w);
        uint8_t const want[4] = { 'a', 'a', 'a', 'a' };
        bw_be32(&w, cnvs_zlib_adler32(want, sizeof want));
        CHECK(cnvs_zlib_inflate(out, (int)sizeof out, w.buf, w.at) == 4);
        CHECK(memcmp(out, want, sizeof want) == 0);
    }
}

int main(void) {
    test_round_trips();
    test_deflate_dst_too_small();
    test_fixtures();
    test_multiblock_stored();
    test_crafted_fixed();
    test_bad_header();
    test_bad_stored();
    test_bad_distances();
    test_bad_dynamic();
    test_dynamic_single_dist();
    return TEST_REPORT();
}
