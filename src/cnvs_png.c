#include "cnvs_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Output cursor sized up front, so a wrong size estimate traps at buf[at]
// rather than corrupting the heap.
struct writer {
    uint8_t *__counted_by(cap) buf;
    size_t cap;
    size_t at;
};

static void put8(struct writer *w, uint8_t b) {
    w->buf[w->at] = b;
    w->at += 1;
}

static void put16le(struct writer *w, uint16_t v) {
    put8(w, (uint8_t)(v & 0xFFu));
    put8(w, (uint8_t)((v >> 8) & 0xFFu));
}

static void put32be(struct writer *w, uint32_t v) {
    put8(w, (uint8_t)(v >> 24));
    put8(w, (uint8_t)(v >> 16));
    put8(w, (uint8_t)(v >> 8));
    put8(w, (uint8_t)(v));
}

// Bulk copy `n` bytes into the cursor with one bounds check (vs n put8s); the
// memcpy still traps if at+n exceeds cap, since w->buf is __counted_by(cap).
static void put_bytes(struct writer *w, uint8_t const *__counted_by(n) src, size_t n) {
    memcpy(w->buf + w->at, src, n);
    w->at += n;
}

static uint32_t g_crc_table[256];
static bool g_crc_ready = false;

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        g_crc_table[n] = c;
    }
    g_crc_ready = true;
}

static uint32_t crc32_range(struct writer const *w, size_t start, size_t end) {
    if (!g_crc_ready) {
        crc_init();
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = start; i < end; i++) {
        uint32_t idx = (c ^ w->buf[i]) & 0xFFu;
        c = g_crc_table[idx] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static uint32_t adler32(uint8_t const *__counted_by(n) data, size_t n) {
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < n; i++) {
        s1 = (s1 + data[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    return (s2 << 16) | s1;
}

static void emit_zlib(struct writer *w,
                      uint8_t const *__counted_by(rawlen) raw, size_t rawlen) {
    put8(w, 0x78);  // CMF/FLG: 0x7801 selects deflate and is a multiple of 31
    put8(w, 0x01);
    size_t off = 0;
    while (off < rawlen) {
        size_t seg = rawlen - off;
        if (seg > 65535u) {
            seg = 65535u;
        }
        bool final = (off + seg >= rawlen);
        put8(w, final ? 0x01u : 0x00u);          // BFINAL + BTYPE=00 (stored)
        put16le(w, (uint16_t)seg);               // LEN
        put16le(w, (uint16_t)(~(uint16_t)seg));  // NLEN = ~LEN
        put_bytes(w, raw + off, seg);
        off += seg;
    }
    put32be(w, adler32(raw, rawlen));  // big-endian, unlike deflate's LEN/NLEN
}

bool cnvs_png_write(char const *__null_terminated path,
                    uint8_t const *__counted_by(width * height * 4) pixels,
                    int width, int height) {
    // Bounded so the size arithmetic below cannot overflow.
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        return false;
    }

    int stride = width * 4;
    size_t const rawlen = (size_t)height * (size_t)(stride + 1);

    // Each row is prefixed by a filter-type byte (0 = None).
    uint8_t *__counted_by(rawlen) raw = malloc(rawlen);
    if (!raw) {
        return false;
    }
    {
        size_t pos = 0;
        for (int y = 0; y < height; y++) {
            raw[pos] = 0;  // per-row filter byte (None)
            pos += 1;
            memcpy(raw + pos, pixels + (size_t)y * (size_t)stride, (size_t)stride);
            pos += (size_t)stride;
        }
    }

    size_t nseg = (rawlen + 65534u) / 65535u;
    size_t zlib_len = 2u + 5u * nseg + rawlen + 4u;
    size_t const total = 8u                       // signature
                       + (12u + 13u)              // IHDR
                       + (12u + zlib_len)          // IDAT
                       + 12u;                      // IEND
    uint8_t *__counted_by(total) out = malloc(total);
    if (!out) {
        free(raw);
        return false;
    }

    struct writer w = { .buf = out, .cap = total, .at = 0 };

    static uint8_t const sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    for (int i = 0; i < 8; i++) {
        put8(&w, sig[i]);
    }

    put32be(&w, 13u);
    size_t ihdr = w.at;
    put8(&w, 'I'); put8(&w, 'H'); put8(&w, 'D'); put8(&w, 'R');
    put32be(&w, (uint32_t)width);
    put32be(&w, (uint32_t)height);
    put8(&w, 8u);  // bit depth
    put8(&w, 6u);  // colour type RGBA
    put8(&w, 0u);  // compression
    put8(&w, 0u);  // filter method
    put8(&w, 0u);  // interlace
    put32be(&w, crc32_range(&w, ihdr, w.at));

    put32be(&w, (uint32_t)zlib_len);
    size_t idat = w.at;
    put8(&w, 'I'); put8(&w, 'D'); put8(&w, 'A'); put8(&w, 'T');
    emit_zlib(&w, raw, rawlen);
    put32be(&w, crc32_range(&w, idat, w.at));

    put32be(&w, 0u);
    size_t iend = w.at;
    put8(&w, 'I'); put8(&w, 'E'); put8(&w, 'N'); put8(&w, 'D');
    put32be(&w, crc32_range(&w, iend, w.at));

    free(raw);

    bool ok = (w.at == total);
    if (ok) {
        FILE *f = fopen(path, "wb");
        if (f) {
            ok = (fwrite(out, 1, total, f) == total);
            ok = (fclose(f) == 0) && ok;
        } else {
            ok = false;
        }
    }
    free(out);
    return ok;
}
