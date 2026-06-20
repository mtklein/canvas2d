// DEFLATE (RFC 1951) in a zlib wrapper (RFC 1950), memory to memory.  Deflate
// is greedy LZ77 over the 32K window via hash chains, emitted as a
// single fixed-Huffman block -- universally decodable, no dynamic tree
// construction on the emit side.  Inflate is the full decoder: every structure
// a hostile stream controls
// (Huffman code lengths, repeat counts, match distances, stored-block sizes) is
// validated arithmetically before it can drive an index, so malformed input
// returns -1 cleanly -- -fbounds-safety is the net underneath, not the error
// handler.

#include "cnvs_zlib.h"

#include "cnvs_math.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

// Length codes 257-285 and distance codes 0-29 (RFC 1951 3.2.5): each code
// covers [base, base + 2^extra) and the extra bits select within the range.
// Shared by the deflate emitter and the inflate decoder.
static uint16_t const len_base[29] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};
static uint8_t const len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};
static uint16_t const dist_base[30] = {
    1,   2,   3,   4,    5,    7,    9,    13,   17,   25,   33,    49,    65,    97,    129,
    193, 257, 385, 513,  769,  1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577,
};
static uint8_t const dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};


uint32_t cnvs_zlib_adler32(uint8_t const *__counted_by(n) data, size_t n) {
    // Position weights for a 16-byte block: in s1 + s2 = s1 + sum_i(s1 + sum_{j<=i} d[j]),
    // byte j contributes (16 - j) to s2, so wts[j] = 16 - j.
    uint16 const wts = { 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 };
    uint32_t s1 = 1, s2 = 0;
    size_t i = 0;
    while (i < n) {
        // Defer the mod across up to NMAX bytes (zlib's bound: the uint32
        // accumulators can't overflow within a chunk).
        size_t chunk = n - i;
        if (chunk > 5552) {
            chunk = 5552;
        }
        size_t const end = i + chunk;
        for (; i + 16 <= end; i += 16) {
            uchar16 v;
            memcpy(&v, data + i, sizeof v);  // unaligned vector load, still bounds-checked
            uint16 w = __builtin_convertvector(v, uint16);
            s2 += 16u * s1 + __builtin_reduce_add(w * wts);  // uses s1 before the block
            s1 += __builtin_reduce_add(w);
        }
        for (; i < end; i++) {  // tail of the chunk, scalar, mod deferred
            s1 += data[i];
            s2 += s1;
        }
        s1 %= 65521u;
        s2 %= 65521u;
    }
    return (s2 << 16) | s1;
}

// ---------------------------------------------------------------- deflate ----

enum {
    zlib_window    = 32768,  // RFC 1951 maximum back-reference distance
    zlib_min_match = 3,
    zlib_max_match = 258,
    zlib_hash_bits = 15,
    zlib_hash_size = 1 << zlib_hash_bits,
    // Bounded hash-chain walk: at most this many candidates are byte-verified
    // per position.  32 is deflate's own "fast" depth -- past it the match-gain
    // curve is flat for this encoder's consumers (PNG rows), and the bound
    // makes the matcher's worst case linear in the input, never quadratic.
    zlib_chain_max = 32,
    // Matches longer than this insert only every zlib_insert_stride-th covered
    // position into the chains (zlib's max_insert_length idea): long matches
    // are dominated by runs, whose interior positions all hash alike, so
    // dense insertion there is almost pure overhead.
    zlib_max_insert    = 32,
    zlib_insert_stride = 8,
};

// Output bit stream, LSB-first within each byte (RFC 1951 3.1.1).  Writes are
// guarded against `cap`; running out sets `full` and the encoder returns -1 --
// like cnvs_png's writer, __counted_by(cap) would trap a miscounted write, but
// the explicit check means it never gets the chance.
struct bitwr {
    uint8_t *__counted_by(cap) dst;
    int cap;
    int at;
    uint32_t acc;  // pending bits, LSB-first
    int nbits;     // bits in acc, < 8 between calls
    bool full;
};

static void putbits(struct bitwr *w, uint32_t v, int cnt) {
    w->acc |= v << w->nbits;
    w->nbits += cnt;
    while (w->nbits >= 8) {
        if (w->at == w->cap) {
            w->full = true;
            w->acc = 0;
            w->nbits = 0;
            return;
        }
        w->dst[w->at] = (uint8_t)(w->acc & 0xFFu);
        w->at += 1;
        w->acc >>= 8;
        w->nbits -= 8;
    }
}

static void wr_align(struct bitwr *w) {
    if (w->nbits != 0) {
        putbits(w, 0, 8 - w->nbits);  // pad the last partial byte with zeros
    }
}

// Huffman codes are packed MSB-first within deflate's LSB-first bit stream, so
// the canonical code value is bit-reversed before putbits.
static uint32_t bitrev(uint32_t v, int cnt) {
    uint32_t r = 0;
    for (int i = 0; i < cnt; i++) {
        r = (r << 1) | (v & 1u);
        v >>= 1;
    }
    return r;
}

// Fixed literal/length code (RFC 1951 3.2.6).
static void put_litlen(struct bitwr *w, int sym) {
    if (sym < 144) {
        putbits(w, bitrev(0x30u + (uint32_t)sym, 8), 8);
    } else if (sym < 256) {
        putbits(w, bitrev(0x190u + (uint32_t)(sym - 144), 9), 9);
    } else if (sym < 280) {
        putbits(w, bitrev((uint32_t)(sym - 256), 7), 7);
    } else {
        putbits(w, bitrev(0xC0u + (uint32_t)(sym - 280), 8), 8);
    }
}

static void put_match(struct bitwr *w, int len, int dist) {
    int lc = 28;  // largest code whose base fits; len >= 3 = len_base[0] terminates
    while (len < len_base[lc]) {
        lc -= 1;
    }
    put_litlen(w, 257 + lc);
    putbits(w, (uint32_t)(len - len_base[lc]), len_extra[lc]);
    int dc = 29;  // dist >= 1 = dist_base[0] terminates
    while (dist < dist_base[dc]) {
        dc -= 1;
    }
    putbits(w, bitrev((uint32_t)dc, 5), 5);
    putbits(w, (uint32_t)(dist - dist_base[dc]), dist_extra[dc]);
}

// Fibonacci hash of the next 4 bytes down to zlib_hash_bits.  Four bytes, not
// the minimum-match three: with the chain walk capped at zlib_chain_max, what
// matters is how promising the candidates are, and the extra byte keeps
// positions that share only a 3-byte prefix off each other's chains.  The
// cost is that a position fewer than 4 bytes from the end can't be hashed, so
// a final 3-byte match goes out as literals.  The multiply wraps by design;
// __builtin_mul_overflow with the flag ignored is the sanctioned spelling of
// that intent, so the debug variant's -fsanitize=integer unsigned-wrap check
// leaves it alone.
static uint32_t hash4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    uint32_t v = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
    (void)__builtin_mul_overflow(v, 0x9E3779B1u, &v);
    return v >> (32 - zlib_hash_bits);
}

// Hash-chain state in one block: chains[0, zlib_hash_size) is head[] (the most
// recent position that hashed there, -1 for none); chains[zlib_hash_size + (pos
// & zlib_window-1)] is prev[] (the position before `pos` on its chain at insert
// time).  prev slots recycle every 32K positions, so a link can be stale or even
// belong to another hash's chain -- the walk treats every link as advisory:
// candidates are byte-verified against src, and a link that doesn't strictly
// decrease ends the walk.
static void insert(int *__counted_by(zlib_hash_size + zlib_window) chains,
                   uint8_t const *__counted_by(n) src, int n, int pos) {
    if (pos + 4 <= n) {  // hash4 reads 4 bytes
        uint32_t const h = hash4(src[pos], src[pos + 1], src[pos + 2], src[pos + 3]);
        chains[zlib_hash_size + (pos & (zlib_window - 1))] = chains[h];
        chains[h] = pos;
    }
}

// Safe dst sizing for cnvs_zlib_deflate of any n-byte input.  The costliest
// emitted byte is a 9-bit literal; a match never beats that per byte covered
// (the worst ratio is length 3: code 257 is 7 bits + at most 5+13 distance
// bits = 25 bits for 3 bytes).  So the block is <= 9n bits plus 10 bits of
// header/EOB, i.e. <= n + n/8 + 4 bytes, wrapped by 2 header + 4 adler bytes.
int cnvs_zlib_bound(int n) {
    if (n < 0) {
        n = 0;
    }
    long long bound = (long long)n + n / 8 + 16;
    return bound > INT_MAX ? INT_MAX : (int)bound;
}

int cnvs_zlib_deflate(uint8_t *__counted_by(dcap) dst, int dcap,
                      uint8_t const *__counted_by(n) src, int n) {
    if (n < 0 || dcap < 0) {
        return -1;
    }
    int const nslots = zlib_hash_size + zlib_window;
    int *__counted_by_or_null(nslots) chains = malloc((size_t)nslots * sizeof *chains);
    if (!chains) {
        return -1;
    }
    for (int i = 0; i < zlib_hash_size; i++) {
        chains[i] = -1;  // every head starts empty; prev slots are written before
    }                    // they become reachable (insert links prev, then head)

    struct bitwr w = { .dst = dst, .cap = dcap, .at = 0, .acc = 0, .nbits = 0, .full = false };
    // CMF/FLG 0x78 0x01: CM=8 (deflate), CINFO=7 (32K window), FLEVEL=0, no
    // dictionary, and 0x7801 is a multiple of 31 -- the same header the PNG
    // encoder has always written.
    putbits(&w, 0x78u, 8);
    putbits(&w, 0x01u, 8);
    putbits(&w, 1u, 1);  // BFINAL: one block carries the whole input
    putbits(&w, 1u, 2);  // BTYPE=01, fixed Huffman

    int i = 0;
    while (i < n) {
        int best_len = 0, best_dist = 0;
        if (i + 4 <= n) {  // hash4 reads 4 bytes
            int const limit = n - i < zlib_max_match ? n - i : zlib_max_match;
            int cand = chains[hash4(src[i], src[i + 1], src[i + 2], src[i + 3])];
            for (int steps = 0; steps < zlib_chain_max && cand >= 0 && i - cand <= zlib_window;
                 steps++) {
                // First-byte reject (zlib's scan_end idea): a candidate whose
                // byte at best_len mismatches can match at most best_len bytes
                // and could never update best, so skip its verify entirely.
                // Skipping never changes which match wins: emitted bytes are
                // unchanged.
                if (best_len >= 8 && src[cand + best_len] != src[i + best_len]) {
                    int const skip = chains[zlib_hash_size + (cand & (zlib_window - 1))];
                    if (skip >= cand) {
                        break;  // recycled slot: real chains strictly decrease
                    }
                    cand = skip;
                    continue;
                }
                // Byte-verify the advisory chain link, 8 bytes per step via the
                // memcpy idiom (the blit treatment): one bounds check per
                // 8-byte block instead of two per byte.  Both loads stay in
                // bounds while len + 8 <= limit, since cand < i and
                // i + limit <= n.  The first mismatching byte falls out of the
                // XOR's trailing zeros; the scalar loop then covers limit's
                // last sub-block bytes (and terminates instantly on the block
                // loop's mismatch byte).  Same len as the pure byte loop,
                // always -- emitted bytes are unchanged.
                int len = 0;
                while (len + 8 <= limit) {
                    uint64_t a, c;
                    memcpy(&a, src + i + len, sizeof a);     // bounds-checked
                    memcpy(&c, src + cand + len, sizeof c);  // 8-byte loads
                    uint64_t const x = a ^ c;
                    if (x != 0) {
                        len += __builtin_ctzll(x) >> 3;
                        break;
                    }
                    len += 8;
                }
                while (len < limit && src[cand + len] == src[i + len]) {
                    len += 1;
                }
                if (len > best_len) {
                    best_len = len;
                    best_dist = i - cand;
                    if (len == limit) {
                        break;
                    }
                }
                int const next = chains[zlib_hash_size + (cand & (zlib_window - 1))];
                if (next >= cand) {
                    break;  // recycled slot: real chains strictly decrease
                }
                cand = next;
            }
        }
        if (best_len >= zlib_min_match) {
            put_match(&w, best_len, best_dist);
            int const end = i + best_len;
            if (best_len <= zlib_max_insert) {
                for (; i < end; i++) {
                    insert(chains, src, n, i);
                }
            } else {
                for (int pos = i; pos < end; pos += zlib_insert_stride) {
                    insert(chains, src, n, pos);
                }
                i = end;
            }
        } else {
            put_litlen(&w, src[i]);
            insert(chains, src, n, i);
            i += 1;
        }
    }
    free(chains);

    put_litlen(&w, 256);  // end of block
    wr_align(&w);
    uint32_t const adler = cnvs_zlib_adler32(src, (size_t)n);
    putbits(&w, (adler >> 24) & 0xFFu, 8);  // trailer is big-endian, unlike the
    putbits(&w, (adler >> 16) & 0xFFu, 8);  // deflate bit stream
    putbits(&w, (adler >>  8) & 0xFFu, 8);
    putbits(&w,  adler        & 0xFFu, 8);
    return w.full ? -1 : w.at;
}

// ---------------------------------------------------------------- inflate ----

// Input bit stream over the untrusted bytes.  Every refill advances `at`
// through the __counted_by bound.  The accumulator is 64-bit so the common
// refill is ONE checked 8-byte load instead of a checked byte load per 8 bits;
// whole buffered bytes can therefore sit in acc between calls, and the two
// places that need a byte position (stored_block, the trailer check) recover
// it exactly as at - nbits/8.
struct bitrd {
    uint8_t const *__counted_by(n) src;
    int n;
    int at;        // next unread byte
    uint64_t acc;  // buffered bits, LSB-first
    int nbits;     // accounted bits in acc, <= 64
};

// Bulk refill: one checked 8-byte load tops the accumulator up to >= 56
// buffered bits while at least 8 input bytes remain.  `take` counts the whole
// bytes that fit below bit 64; the loaded word's bits beyond them stay in acc
// above nbits as exact duplicates of the bytes at `at` not yet consumed, so a
// later refill ORs identical bits over them and every consumer below nbits is
// unaffected.  Near the end of input this is a no-op and getbits' byte loop
// remains the (strict) refiller of record.
static void refill(struct bitrd *b) {
    if (b->nbits <= 48 && b->at + 8 <= b->n) {
        uint64_t v;
        memcpy(&v, b->src + b->at, sizeof v);  // one bounds check per 8 bytes
        // Mask to the bits that fit below bit 64 before shifting: same result
        // as letting the shift drop them, spelled so -fsanitize=integer's
        // unsigned-shift-base check sees no bits lost.
        b->acc |= (v & (~0ull >> b->nbits)) << b->nbits;
        int const take = (64 - b->nbits) >> 3;
        b->at += take;
        b->nbits += take * 8;
    }
}

// Pull cnt (<= 16) bits.  Returns false if the stream is exhausted, so a
// truncated stream can never supply bits that drive an index -- every caller
// turns false into the single -1 error path.
static bool getbits(struct bitrd *b, int cnt, uint32_t *__single out) {
    if (b->nbits < cnt) {
        refill(b);
    }
    while (b->nbits < cnt) {  // within the last 7 input bytes: byte-at-a-time
        if (b->at == b->n) {
            return false;
        }
        b->acc |= (uint64_t)b->src[b->at] << b->nbits;
        b->at += 1;
        b->nbits += 8;
    }
    *out = (uint32_t)(b->acc & ((1u << cnt) - 1u));
    b->acc >>= cnt;
    b->nbits -= cnt;
    return true;
}

static void rd_align(struct bitrd *b) {  // discard the partial byte's bits
    int const drop = b->nbits & 7;
    b->acc >>= drop;
    b->nbits -= drop;
}

// Canonical Huffman code as (count, symbol) tables: count[l] codes of length l,
// symbols in code order.  Decoding walks the lengths arithmetically (the puff
// shape) -- no untrusted length drives an index until construction has proven
// the code well-formed.
enum {
    zlib_max_bits   = 15,   // longest Huffman code
    zlib_nlitlen    = 288,  // literal/length alphabet (285 used + 2 reserved + EOB)
    zlib_ndist      = 32,   // fixed code covers all 5-bit codes; 30/31 rejected at decode
    zlib_ncl        = 19,   // code-length-code alphabet
    // Direct-lookup fast path: codes up to this long decode with one table
    // load.  9 covers every fixed-tree code (7-9 bits), i.e. all of our own
    // PNG streams; longer (dynamic-tree) codes fall back to the length walk.
    zlib_table_bits = 9,
};

struct huffman {
    uint16_t count[zlib_max_bits + 1];
    uint16_t symbol[zlib_nlitlen];
    // fast[peek] memoizes the walk for codes <= zlib_table_bits long:
    // (len << 9) | sym for the code whose LSB-first bits are peek's low len
    // bits (replicated across the unused upper bits), 0 where no short code
    // lands -- the walk handles those.  Pure memoization: a table hit returns
    // exactly what the walk would, so acceptance behavior is unchanged.
    uint16_t fast[1 << zlib_table_bits];
};

// Build h from code lengths (0 = unused).  Returns 0 for a complete code, > 0
// (the unclaimed code space) for an incomplete one, -1 for an oversubscribed
// one.  This is the Mark Adler enough-codes check, and it is arithmetic:
// walking lengths 1..15, the live code space doubles and each length's codes
// consume it; going negative means the lengths describe more codes than the
// tree has leaves -- the classic table-overrun feedstock, rejected here before
// any table is built or indexed.  Every call site guarantees lens[] <= 15
// structurally (3-bit fields, CL symbols 0-15, or constants), so count[] is
// indexed in bounds by construction; -fbounds-safety would trap otherwise.
static int huff_build(struct huffman *h, uint8_t const *__counted_by(nsym) lens, int nsym) {
    memset(h->fast, 0, sizeof h->fast);  // all-miss until proven otherwise: a
                                         // no-codes table must decode nothing
    int cnt[zlib_max_bits + 1] = { 0 };
    for (int s = 0; s < nsym; s++) {
        cnt[lens[s]] += 1;
    }
    for (int l = 0; l <= zlib_max_bits; l++) {
        h->count[l] = (uint16_t)cnt[l];
    }
    if (cnt[0] == nsym) {
        return 0;  // no codes at all: complete by fiat; any decode against it fails
    }
    int left = 1;
    for (int len = 1; len <= zlib_max_bits; len++) {
        left <<= 1;
        left -= cnt[len];
        if (left < 0) {
            return -1;  // oversubscribed
        }
    }
    // Counting sort the symbols by code length; offs[l] tracks where length-l
    // symbols land.  offs[15] + count[15] <= nsym <= 288, so symbol[] writes
    // stay in bounds arithmetically.
    int offs[zlib_max_bits + 1];
    offs[1] = 0;
    for (int len = 1; len < zlib_max_bits; len++) {
        offs[len + 1] = offs[len] + cnt[len];
    }
    for (int s = 0; s < nsym; s++) {
        if (lens[s] != 0) {
            h->symbol[offs[lens[s]]] = (uint16_t)s;
            offs[lens[s]] += 1;
        }
    }
    // Fill the fast table by enumerating exactly what huff_decode's walk
    // tracks: codes of one length are consecutive from `first`, their symbols
    // from `index`.  A length-len code's stream bits arrive MSB-first, so its
    // table slots are its bit-reversed value at every setting of the unused
    // upper bits.  first + k < 2^len here: the oversubscription check above
    // already rejected anything else.
    {
        int first = 0, index = 0;
        for (int len = 1; len <= zlib_table_bits; len++) {
            for (int k = 0; k < cnt[len]; k++) {
                uint16_t const entry =
                    (uint16_t)((len << zlib_table_bits) | h->symbol[index + k]);
                for (uint32_t j = bitrev((uint32_t)(first + k), len);
                     j < (1u << zlib_table_bits); j += 1u << len) {
                    h->fast[j] = entry;
                }
            }
            index += cnt[len];
            first = (first + cnt[len]) << 1;
        }
    }
    return left;
}

// Decode one symbol: walk lengths 1..15 tracking the first code and symbol-
// table offset of each length (canonical codes of one length are consecutive).
// huff_build validated count[], so index + (code - first) < sum(count) <= nsym
// -- the untrusted stream picks which in-range symbol, never the range.  With
// >= 15 buffered bits (the common case after one bulk refill) the walk runs on
// locals with no per-bit refill or exhaustion checks and writes the consumed
// length back once; the byte-at-a-time path only runs within the stream's
// last bytes, where getbits' exhaustion check is the truncation detector.
static int huff_decode(struct bitrd *b, struct huffman const *h) {
    if (b->nbits < zlib_max_bits) {
        refill(b);
    }
    // Fast path: one load answers any code <= zlib_table_bits long.  The
    // entry is replicated across the peek's unused upper bits, so it depends
    // only on its own len bits; elen <= nbits keeps the lookup to accounted
    // bits even within the stream's last bytes.
    uint32_t const peek = (uint32_t)b->acc & ((1u << zlib_table_bits) - 1u);
    uint32_t const e = h->fast[peek];
    int const elen = (int)(e >> zlib_table_bits);
    if (elen != 0 && elen <= b->nbits) {
        b->acc >>= elen;
        b->nbits -= elen;
        return (int)(e & ((1u << zlib_table_bits) - 1u));
    }
    if (b->nbits >= zlib_max_bits) {
        uint32_t acc = (uint32_t)b->acc;
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= zlib_max_bits; len++) {
            code |= (int)(acc & 1u);
            acc >>= 1;
            int count = h->count[len];
            if (code - first < count) {
                b->acc >>= len;
                b->nbits -= len;
                return h->symbol[index + (code - first)];
            }
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return -1;  // ran into an incomplete code's unclaimed space
    }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= zlib_max_bits; len++) {
        uint32_t bit;
        if (!getbits(b, 1, &bit)) {
            return -1;
        }
        code |= (int)bit;
        int const count = h->count[len];
        if (code - first < count) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;  // ran into an incomplete code's unclaimed space
}

// zlib's compatibility posture (puff's rule): an incomplete code is permitted
// only as a single 1-bit code -- a one-symbol alphabet has no complete
// canonical Huffman code, so real encoders emit exactly that shape (zlib's
// one-distance-code streams).  err is huff_build's return.
static bool huff_ok(struct huffman const *h, int err, int nsym) {
    if (err < 0) {
        return false;
    }
    return err == 0 || nsym == h->count[0] + h->count[1];
}

// Decode one block's symbol stream into dst at *outp.  Order of checks per
// symbol: range-check the symbol itself (286/287 and distance 30/31 are
// encodable but unassigned), then the back-reference against bytes written so
// far (d <= out: no read before the start of output), then the copy against
// dcap -- only then do bytes move.
static bool run_block(struct bitrd *b, uint8_t *__counted_by(dcap) dst, int dcap,
                      int *__single outp,
                      struct huffman const *lit, struct huffman const *dist) {
    int out = *outp;
    for (;;) {
        int const sym = huff_decode(b, lit);
        if (sym < 0) {
            return false;
        }
        if (sym < 256) {
            if (out == dcap) {
                return false;  // output overflow
            }
            dst[out] = (uint8_t)sym;
            out += 1;
        } else if (sym == 256) {  // end of block
            *outp = out;
            return true;
        } else {
            if (sym > 285) {
                return false;
            }
            int const lc = sym - 257;
            uint32_t extra;
            if (!getbits(b, len_extra[lc], &extra)) {
                return false;
            }
            int const len = len_base[lc] + (int)extra;
            int const ds = huff_decode(b, dist);
            if (ds < 0 || ds > 29) {
                return false;
            }
            if (!getbits(b, dist_extra[ds], &extra)) {
                return false;
            }
            int const d = dist_base[ds] + (int)extra;
            if (d > out) {
                return false;  // back-reference past the start of output
            }
            if (len > dcap - out) {
                return false;  // output overflow
            }
            if (d >= 8 && len + 7 <= dcap - out) {
                // Inline 8-byte chunks (the memcpy idiom: fixed-size loads
                // and stores the compiler keeps inline, one bounds check per
                // 8 bytes, no libc call per match).  With the period >= 8,
                // chunk k's source bytes sit at least 8 behind its
                // destination, so they're written before any chunk reads
                // them -- for disjoint and self-overlapping runs alike, and
                // a chunk that reads this match's own earlier bytes reads
                // correct run content by induction.  The last chunk may
                // scribble up to 7 bytes past the run: inside dcap by the
                // guard, positions strictly above the write cursor, each
                // properly rewritten by whichever later symbol owns it (or
                // dead beyond the final count).
                for (int k = 0; k < len; k += 8) {
                    uint64_t v;
                    memcpy(&v, dst + (out - d + k), sizeof v);
                    memcpy(dst + (out + k), &v, sizeof v);
                }
            } else if (d >= len) {
                // Disjoint source and destination (with no room to overshoot
                // above): one checked memcpy moves the whole run.
                memcpy(dst + out, dst + (out - d), (size_t)len);
            } else {
                // Overlapping copy (d < len) is the classic LZ77 RLE idiom:
                // the run repeats with period d, so it builds by doubling.
                // Each chunk's source [out-d, out-d+chunk) is fully written
                // (chunk <= done + d) and ends at or before its destination
                // out + done, so every memcpy is a plain disjoint copy, and
                // done stays a multiple of d (chunk == have, except the last)
                // which keeps the period aligned.  Byte-identical to the
                // byte-at-a-time loop.
                int done = 0, have = d;
                while (done < len) {
                    int const chunk = have < len - done ? have : len - done;
                    memcpy(dst + (out + done), dst + (out - d), (size_t)chunk);
                    done += chunk;
                    have <<= 1;
                }
            }
            out += len;
        }
    }
}

static bool stored_block(struct bitrd *b, uint8_t *__counted_by(dcap) dst, int dcap,
                         int *__single outp) {
    rd_align(b);  // LEN starts at the next byte boundary
    // The 64-bit reader may hold whole buffered bytes; hand them back so LEN,
    // NLEN and the bulk copy read from src directly (every buffered byte maps
    // 1:1 to an input byte, so the position recovers exactly).
    b->at -= b->nbits / 8;
    b->acc = 0;
    b->nbits = 0;
    if (b->n - b->at < 4) {
        return false;  // truncated LEN/NLEN
    }
    uint32_t const len  = (uint32_t)b->src[b->at]     | ((uint32_t)b->src[b->at + 1] << 8);
    uint32_t const nlen = (uint32_t)b->src[b->at + 2] | ((uint32_t)b->src[b->at + 3] << 8);
    b->at += 4;
    if ((len ^ nlen) != 0xFFFFu) {
        return false;
    }
    int out = *outp;
    int const remaining = (int)len;
    if (remaining > dcap - out) {
        return false;  // output overflow
    }
    if (remaining > b->n - b->at) {
        return false;  // truncated payload
    }
    if (remaining > 0) {
        memcpy(dst + out, b->src + b->at, (size_t)remaining);
        b->at += remaining;
        out += remaining;
    }
    *outp = out;
    return true;
}

// RFC 1951 3.2.6 fixed code lengths, built through the same construction path
// dynamic blocks use.  Both are complete by construction (the distance code is
// built over all 32 5-bit symbols; 30/31 are rejected at decode like any other
// unassigned symbol).
static void fixed_tables(struct huffman *lit, struct huffman *dist) {
    uint8_t lens[zlib_nlitlen];
    int i = 0;
    for (; i < 144; i++) { lens[i] = 8; }
    for (; i < 256; i++) { lens[i] = 9; }
    for (; i < 280; i++) { lens[i] = 7; }
    for (; i < zlib_nlitlen; i++) { lens[i] = 8; }
    (void)huff_build(lit, lens, zlib_nlitlen);
    for (i = 0; i < zlib_ndist; i++) { lens[i] = 5; }
    (void)huff_build(dist, lens, zlib_ndist);
}

// The code-length-code step (RFC 1951 3.2.7): read the 19 CL code lengths in
// the fixed permutation order, build the CL code (which must be complete),
// then decode the literal/length + distance code lengths through it, with
// 16/17/18 run-length expansion validated against the combined total.
static int const cl_order[zlib_ncl] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
};

static bool dynamic_tables(struct bitrd *b, struct huffman *lit, struct huffman *dist) {
    uint32_t hlit, hdist, hclen;
    if (!getbits(b, 5, &hlit) || !getbits(b, 5, &hdist) || !getbits(b, 4, &hclen)) {
        return false;
    }
    int nlit = (int)hlit + 257, ndist = (int)hdist + 1, ncl = (int)hclen + 4;
    if (nlit > 286 || ndist > 30) {
        return false;
    }
    uint8_t cl_lens[zlib_ncl] = { 0 };
    for (int i = 0; i < ncl; i++) {
        uint32_t v;
        if (!getbits(b, 3, &v)) {
            return false;
        }
        cl_lens[cl_order[i]] = (uint8_t)v;  // 3-bit field: <= 7 < zlib_max_bits
    }
    struct huffman cl;
    if (huff_build(&cl, cl_lens, zlib_ncl) != 0) {
        return false;  // the CL code itself must be complete, exactly
    }

    uint8_t lens[286 + 30];
    int const total = nlit + ndist;
    int got = 0;
    while (got < total) {
        int const sym = huff_decode(b, &cl);
        if (sym < 0) {
            return false;
        }
        if (sym < 16) {  // a literal code length, 0..15
            lens[got] = (uint8_t)sym;
            got += 1;
            continue;
        }
        uint8_t v = 0;
        uint32_t extra;
        int rep;
        if (sym == 16) {  // repeat the previous length 3-6 times
            if (got == 0) {
                return false;  // nothing to repeat
            }
            v = lens[got - 1];
            if (!getbits(b, 2, &extra)) {
                return false;
            }
            rep = 3 + (int)extra;
        } else if (sym == 17) {  // 3-10 zeros
            if (!getbits(b, 3, &extra)) {
                return false;
            }
            rep = 3 + (int)extra;
        } else {  // 18: 11-138 zeros
            if (!getbits(b, 7, &extra)) {
                return false;
            }
            rep = 11 + (int)extra;
        }
        if (rep > total - got) {
            return false;  // repeat would overrun the combined length list
        }
        for (int k = 0; k < rep; k++) {
            lens[got] = v;
            got += 1;
        }
    }
    if (lens[256] == 0) {
        return false;  // end-of-block must be encodable or the block can't end
    }
    int err = huff_build(lit, lens, nlit);
    if (!huff_ok(lit, err, nlit)) {
        return false;
    }
    err = huff_build(dist, lens + nlit, ndist);
    return huff_ok(dist, err, ndist);
}

int cnvs_zlib_inflate(uint8_t *__counted_by(dcap) dst, int dcap,
                      uint8_t const *__counted_by(n) src, int n) {
    if (dcap < 0 || n < 2) {
        return -1;
    }
    int cmf = src[0], flg = src[1];
    if ((cmf & 0x0F) != 8) {
        return -1;  // CM: deflate is the only defined method
    }
    if ((cmf >> 4) > 7) {
        return -1;  // CINFO: window beyond 32K is illegal
    }
    if ((cmf * 256 + flg) % 31 != 0) {
        return -1;  // FCHECK
    }
    if (flg & 0x20) {
        return -1;  // FDICT: preset dictionaries unsupported
    }

    struct bitrd b = { .src = src, .n = n, .at = 2, .acc = 0, .nbits = 0 };
    int out = 0;
    for (;;) {
        uint32_t bfinal, btype;
        if (!getbits(&b, 1, &bfinal) || !getbits(&b, 2, &btype)) {
            return -1;
        }
        if (btype == 0) {
            if (!stored_block(&b, dst, dcap, &out)) {
                return -1;
            }
        } else if (btype == 1 || btype == 2) {
            struct huffman lit, dist;
            if (btype == 1) {
                fixed_tables(&lit, &dist);
            } else if (!dynamic_tables(&b, &lit, &dist)) {
                return -1;
            }
            if (!run_block(&b, dst, dcap, &out, &lit, &dist)) {
                return -1;
            }
        } else {
            return -1;  // BTYPE=11 is reserved
        }
        if (bfinal != 0) {
            break;
        }
    }

    // Strict end state: after the final block, exactly the 4 adler bytes remain
    // -- truncation and trailing garbage both fail here.  After discarding the
    // partial byte, whatever whole bytes the 64-bit reader still buffers map
    // 1:1 to input bytes, so the subtraction recovers the exact byte position.
    rd_align(&b);
    int const pos = b.at - b.nbits / 8;
    if (n - pos != 4) {
        return -1;
    }
    uint32_t want = ((uint32_t)src[pos]     << 24) | ((uint32_t)src[pos + 1] << 16) |
                    ((uint32_t)src[pos + 2] <<  8) |  (uint32_t)src[pos + 3]       ;
    if (want != cnvs_zlib_adler32(dst, (size_t)out)) {
        return -1;
    }
    return out;
}
