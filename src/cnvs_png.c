// PNG writer: 8-bit RGBA, non-interlaced, Up-filtered rows, deflate via
// cnvs_zlib.
//
// Row filtering is Up-ONLY (every row's filter byte is 2): each row encodes as
// its byte-wise difference from the row above, which vectorizes in both
// directions as a whole-row subtract (encode) / add (decode) -- no
// left-neighbor recurrence like Sub/Avg/Paeth, so the kernels are straight
// 16-lane vector ops with one bounds check per block.  The first row's
// implicit prior row is all zeros (PNG spec), so Up degenerates to None there;
// we still emit filter byte 2 uniformly so every row decodes through the same
// kernel.  Only our own files are decoded, so the adaptive five-filter
// chooser (which costs a per-row trial encode) is not used.

#include "cnvs_png.h"

#include "cnvs_zlib.h"

#include <limits.h>
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

static void put32be(struct writer *w, uint32_t v) {
    put8(w, (uint8_t)(v >> 24));
    put8(w, (uint8_t)(v >> 16));
    put8(w, (uint8_t)(v >>  8));
    put8(w, (uint8_t)(v      ));
}

// Bulk copy into the cursor; traps if at+n exceeds cap, since w->buf is
// __counted_by(cap).
static void put_bytes(struct writer *w, uint8_t const *__counted_by(n) src, size_t n) {
    memcpy(w->buf + w->at, src, n);
    w->at += n;
}

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>

// PNG/zlib's CRC is CRC-32/ISO-HDLC -- exactly what ARMv8's crc32 instructions
// compute, ~20x faster than the byte-at-a-time table.  Eight bytes at a time via
// memcpy (unaligned load, still bounds-checked under -fbounds-safety -- the same
// shape adler32 uses), then a scalar tail.  Shared by encode (chunk emission)
// and decode (chunk verification).
static uint32_t crc32_buf(uint8_t const *__counted_by(n) p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, sizeof v);
        c = __crc32d(c, v);
    }
    for (; i < n; i++) {
        c = __crc32b(c, p[i]);
    }
    return c ^ 0xFFFFFFFFu;
}

#else  // portable byte-at-a-time fallback (non-ARMv8 targets)

// The table lives on the stack, rebuilt per call (2048 cheap iterations --
// noise next to checksumming any PNG chunk): a lazily initialized file-scope
// static would be shared mutable state, which the thread-safety posture
// (distinct canvases are fully independent; src/ holds none) forbids.
static uint32_t crc32_buf(uint8_t const *__counted_by(n) p, size_t n) {
    uint32_t table[256];
    for (uint32_t v = 0; v < 256; v++) {
        uint32_t c = v;
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[v] = c;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        uint32_t const idx = (c ^ p[i]) & 0xFFu;
        c = table[idx] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}
#endif

typedef uint8_t pngu8x16 __attribute__((ext_vector_type(16)));

// Up-filter one row: out[i] = cur[i] - prev[i] mod 256.  Whole-vector ops via
// the memcpy idiom -- one bounds check per 16-byte block (the struct cnvs_cover
// resolve / blur pattern) -- then a scalar tail.  Lane subtraction wraps mod
// 256 by design (PNG filter arithmetic); the scalar tail's operands promote to
// int, so there is no unsigned wrap for -fsanitize=integer to flag.
static void filter_up(uint8_t *__counted_by(n) out,
                      uint8_t const *__counted_by(n) cur,
                      uint8_t const *__counted_by(n) prev, int n) {
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        pngu8x16 c, p;
        memcpy(&c, cur + i, sizeof c);   // bounds-checked vector loads
        memcpy(&p, prev + i, sizeof p);
        c -= p;
        memcpy(out + i, &c, sizeof c);   // bounds-checked vector store
    }
    for (; i < n; i++) {
        out[i] = (uint8_t)(cur[i] - prev[i]);
    }
}

uint8_t *__counted_by_or_null(*outlen)
cnvs_png_encode(uint8_t const *__counted_by(width * height * 4) pixels,
                int width, int height, int *__single outlen) {
    *outlen = 0;
    // Bounded so every size computation below fits an int: rawlen <=
    // 16384 * (16384*4 + 1) = 1,073,758,208 < INT_MAX, and cnvs_zlib_bound of
    // that is ~1.21e9, still < INT_MAX.
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        return NULL;
    }

    int const stride = width * 4;
    int const rawlen = height * (stride + 1);

    // The filtered stream: each row is a filter byte (2 = Up) + the row's
    // Up-filtered bytes.
    uint8_t *__counted_by_or_null(rawlen) raw = malloc((size_t)rawlen);
    if (!raw) {
        return NULL;
    }
    {
        int pos = 0;
        for (int y = 0; y < height; y++) {
            raw[pos] = 2;  // per-row filter byte: Up, uniformly
            pos += 1;
            uint8_t const *__counted_by(stride) cur = pixels + (size_t)y * (size_t)stride;
            if (y == 0) {
                // Row 0's implicit prior is all zeros: Up == None, a plain copy.
                memcpy(raw + pos, cur, (size_t)stride);
            } else {
                uint8_t const *__counted_by(stride) prev =
                    pixels + (size_t)(y - 1) * (size_t)stride;
                filter_up(raw + pos, cur, prev, stride);
            }
            pos += stride;
        }
    }

    int const zcap = cnvs_zlib_bound(rawlen);
    uint8_t *__counted_by_or_null(zcap) z = malloc((size_t)zcap);
    if (!z) {
        free(raw);
        return NULL;
    }
    int const zn = cnvs_zlib_deflate(z, zcap, raw, rawlen);
    free(raw);
    if (zn < 0) {  // only OOM in the matcher: zcap is sufficient by the bound
        free(z);
        return NULL;
    }

    size_t const total = 8u                  // signature
                       + (12u + 13u)         // IHDR
                       + (12u + (size_t)zn)  // IDAT
                       + 12u;                // IEND
    uint8_t *__counted_by_or_null(total) out = malloc(total);
    if (!out) {
        free(z);
        return NULL;
    }

    struct writer w = { .buf = out, .cap = total, .at = 0 };

    static uint8_t const sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    for (int i = 0; i < 8; i++) {
        put8(&w, sig[i]);
    }

    put32be(&w, 13u);
    size_t const ihdr = w.at;
    put8(&w, 'I'); put8(&w, 'H'); put8(&w, 'D'); put8(&w, 'R');
    put32be(&w, (uint32_t)width);
    put32be(&w, (uint32_t)height);
    put8(&w, 8u);  // bit depth
    put8(&w, 6u);  // colour type RGBA
    put8(&w, 0u);  // compression
    put8(&w, 0u);  // filter method
    put8(&w, 0u);  // interlace
    put32be(&w, crc32_buf(w.buf + ihdr, w.at - ihdr));

    put32be(&w, (uint32_t)zn);
    size_t const idat = w.at;
    put8(&w, 'I'); put8(&w, 'D'); put8(&w, 'A'); put8(&w, 'T');
    put_bytes(&w, z, (size_t)zn);
    put32be(&w, crc32_buf(w.buf + idat, w.at - idat));

    put32be(&w, 0u);
    size_t const iend = w.at;
    put8(&w, 'I'); put8(&w, 'E'); put8(&w, 'N'); put8(&w, 'D');
    put32be(&w, crc32_buf(w.buf + iend, w.at - iend));

    free(z);

    if (w.at != total) {  // unreachable: every put above is accounted in total
        free(out);
        return NULL;
    }
    *outlen = (int)total;
    return out;
}

bool cnvs_png_write(char const *__null_terminated path,
                    uint8_t const *__counted_by(width * height * 4) pixels,
                    int width, int height) {
    int total = 0;
    uint8_t *out = cnvs_png_encode(pixels, width, height, &total);
    if (!out) {
        return false;
    }
    bool ok = false;
    FILE *f = fopen(path, "wb");
    if (f) {
        ok = (fwrite(out, 1, (size_t)total, f) == (size_t)total);
        ok = (fclose(f) == 0) && ok;
    }
    free(out);
    return ok;
}
