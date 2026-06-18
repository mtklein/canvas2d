#pragma once

// zlib streams (RFC 1950) carrying DEFLATE (RFC 1951), memory to memory, from
// scratch -- no zlib dependency.  The deflate side is simple (greedy
// LZ77 into fixed-Huffman blocks); the inflate side is the full decoder (stored
// + fixed + dynamic-Huffman blocks, 32K window, overlapping copies) and is
// strict: a stream either decodes completely and verifies, or returns -1.

#include <ptrcheck.h>
#include <stddef.h>
#include <stdint.h>

// Deflate `src` into `dst` as a complete zlib stream (RFC 1950 header, one
// fixed-Huffman DEFLATE block, adler32 trailer).  Returns the byte length
// written, or -1 if dst is too small (or the matcher's 256 KiB of hash-chain
// state can't be allocated).  Deterministic: same input -> same bytes, always.
int cnvs_zlib_deflate(uint8_t *__counted_by(dcap) dst, int dcap,
                      uint8_t const *__counted_by(n) src, int n);

// A dst size sufficient for cnvs_zlib_deflate of ANY n-byte input (see the
// 9-bits-per-byte argument at the definition).  Saturates at INT_MAX for
// enormous n -- deflate against any real buffer would just return -1 there.
int cnvs_zlib_bound(int n);

// Inflate a complete zlib stream into dst (the caller knows the decompressed
// size from context -- both consumers do).  Returns the byte count written, or
// -1 on ANY malformation: header, Huffman structure, out-of-window reference,
// output overflow, adler mismatch, trailing garbage.  Never a partial success,
// but dst may have been partially written before the error was detected.
int cnvs_zlib_inflate(uint8_t *__counted_by(dcap) dst, int dcap,
                      uint8_t const *__counted_by(n) src, int n);

// adler32 (RFC 1950), SIMD 16 bytes at a time; shared with the PNG encoder.
uint32_t cnvs_zlib_adler32(uint8_t const *__counted_by(n) data, size_t n);
