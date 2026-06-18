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

// ---------------------------------------------------------------- decode ----

// Big-endian 32-bit read; callers establish at+4 <= n before slicing.
static uint32_t rd32be(uint8_t const *__counted_by(4) p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3]       ;
}

// Un-Up one row: out[i] = row[i] + prev[i] mod 256 -- filter_up's exact
// inverse, the same whole-vector shape (one bounds check per 16-byte block,
// no left-neighbor recurrence to serialize on).
static void defilter_up(uint8_t *__counted_by(n) out,
                        uint8_t const *__counted_by(n) prev,
                        uint8_t const *__counted_by(n) row, int n) {
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        pngu8x16 r, p;
        memcpy(&r, row + i, sizeof r);   // bounds-checked vector loads
        memcpy(&p, prev + i, sizeof p);
        r += p;
        memcpy(out + i, &r, sizeof r);   // bounds-checked vector store
    }
    for (; i < n; i++) {
        out[i] = (uint8_t)(row[i] + prev[i]);
    }
}

uint8_t *__counted_by_or_null(*len)
cnvs_png_decode(uint8_t const *__counted_by(n) src, int n,
                int *__single w, int *__single h, int *__single len) {
    *w = 0;
    *h = 0;
    *len = 0;

    // 8 signature + (12+13) IHDR + 12 IDAT + 12 IEND is the structural floor.
    static uint8_t const sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (n < 8 + 25 + 12 + 12 || memcmp(src, sig, sizeof sig) != 0) {
        return NULL;
    }

    // Pass 1 -- validate the whole chunk sequence before allocating anything
    // pixel-sized.  In order, per chunk: the 12 bytes of framing fit, the
    // declared length fits, the CRC verifies (EVERY chunk, ancillary
    // included), then the type rules: IHDR exactly first (and its fields are
    // exactly ours, dimensions in [1, 16384] -- the cap that bounds every
    // later allocation), IDATs form one consecutive run, IEND is last with
    // nothing after it, unknown ancillary chunks (lowercase first letter) are
    // skipped, unknown critical chunks reject.
    int width = 0, height = 0;
    int idat_total = 0;
    bool in_idat = false, idat_done = false, seen_iend = false;
    int at = 8;
    while (at < n) {
        if (n - at < 12) {
            return NULL;  // truncated framing
        }
        uint32_t const clen32 = rd32be(src + at);
        if (clen32 > (uint32_t)(n - at) - 12u) {
            return NULL;  // declared length runs past the end of input
        }
        int const clen = (int)clen32;
        if (rd32be(src + at + 8 + clen) != crc32_buf(src + at + 4, (size_t)clen + 4u)) {
            return NULL;  // CRC over type + payload
        }
        uint8_t const t0 = src[at + 4], t1 = src[at + 5];
        uint8_t const t2 = src[at + 6], t3 = src[at + 7];
        if (t0 == 'I' && t1 == 'H' && t2 == 'D' && t3 == 'R') {
            if (at != 8 || clen != 13) {
                return NULL;  // IHDR must be exactly the first chunk, once
            }
            uint32_t const w32 = rd32be(src + at + 8);
            uint32_t const h32 = rd32be(src + at + 12);
            if (w32 < 1u || w32 > 16384u || h32 < 1u || h32 > 16384u) {
                return NULL;  // zero, negative-as-unsigned, and overflow bombs
            }
            if (src[at + 16] != 8 ||   // bit depth
                src[at + 17] != 6 ||   // colour type: RGBA
                src[at + 18] != 0 ||   // compression method
                src[at + 19] != 0 ||   // filter method
                src[at + 20] != 0) {   // interlace
                return NULL;
            }
            width = (int)w32;
            height = (int)h32;
        } else if (width == 0) {
            return NULL;  // anything before IHDR
        } else if (t0 == 'I' && t1 == 'D' && t2 == 'A' && t3 == 'T') {
            if (idat_done) {
                return NULL;  // a second IDAT run
            }
            in_idat = true;
            idat_total += clen;  // both <= n, so no overflow
        } else {
            if (in_idat) {
                in_idat = false;
                idat_done = true;
            }
            if (t0 == 'I' && t1 == 'E' && t2 == 'N' && t3 == 'D') {
                if (clen != 0 || at + 12 != n) {
                    return NULL;  // IEND payload, or trailing garbage after it
                }
                seen_iend = true;
            } else if ((t0 & 0x20u) == 0) {
                return NULL;  // unknown critical chunk (PLTE and friends)
            }
            // else: ancillary chunk, skipped (CRC already verified)
        }
        at += 12 + clen;
    }
    if (!seen_iend || idat_total == 0) {
        return NULL;
    }

    // Pass 2 -- concatenate the IDAT payloads (framing re-walked, already
    // validated) and inflate.  The filtered stream must come out at exactly
    // rawlen: cnvs_zlib_inflate is itself strict about overflow, truncation,
    // adler, and trailing bytes, and a short stream fails the == check.
    int const ztotal = idat_total;  // a count decl must sit beside its pointer
    uint8_t *__counted_by_or_null(ztotal) zs = malloc((size_t)ztotal);
    if (!zs) {
        return NULL;
    }
    {
        int zat = 0;
        for (int c = 8; c < n; ) {
            int const clen = (int)rd32be(src + c);
            if (src[c + 4] == 'I' && src[c + 5] == 'D' &&
                src[c + 6] == 'A' && src[c + 7] == 'T' && clen > 0) {
                memcpy(zs + zat, src + c + 8, (size_t)clen);
                zat += clen;
            }
            c += 12 + clen;
        }
    }
    int const stride = width * 4;
    int const rawlen = height * (stride + 1);  // <= 16384 * 65537, fits int
    uint8_t *__counted_by_or_null(rawlen) raw = malloc((size_t)rawlen);
    if (!raw) {
        free(zs);
        return NULL;
    }
    int const got = cnvs_zlib_inflate(raw, rawlen, zs, ztotal);
    free(zs);
    if (got != rawlen) {
        free(raw);
        return NULL;
    }

    // Defilter.  Only None (0) and Up (2) exist in our own output; Sub/Avg/
    // Paeth (1/3/4) and anything else reject.  Row 0's prior row is all
    // zeros, so Up there is the identity copy.
    int const npx = width * height * 4;
    uint8_t *__counted_by_or_null(npx) px = malloc((size_t)npx);
    if (!px) {
        free(raw);
        return NULL;
    }
    for (int y = 0; y < height; y++) {
        uint8_t const f = raw[y * (stride + 1)];
        if (f != 0 && f != 2) {
            free(px);
            free(raw);
            return NULL;
        }
        uint8_t const *__counted_by(stride) row = raw + y * (stride + 1) + 1;
        uint8_t *__counted_by(stride) out = px + (size_t)y * (size_t)stride;
        if (f == 0 || y == 0) {
            memcpy(out, row, (size_t)stride);
        } else {
            uint8_t const *__counted_by(stride) prev =
                px + (size_t)(y - 1) * (size_t)stride;
            defilter_up(out, prev, row, stride);
        }
    }
    free(raw);

    *w = width;
    *h = height;
    *len = npx;
    return px;
}

uint8_t *__counted_by_or_null(*len)
cnvs_png_read(char const *__null_terminated path,
              int *__single w, int *__single h, int *__single len) {
    *w = 0;
    *h = 0;
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    uint8_t *px = NULL;
    long sz = 0;
    if (fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) > 0 && sz <= (long)INT_MAX &&
        fseek(f, 0, SEEK_SET) == 0) {
        int const fn = (int)sz;
        uint8_t *__counted_by_or_null(fn) buf = malloc((size_t)fn);
        if (buf && fread(buf, 1, (size_t)fn, f) == (size_t)fn) {
            px = cnvs_png_decode(buf, fn, w, h, len);
        }
        free(buf);
    }
    (void)fclose(f);
    return px;
}
