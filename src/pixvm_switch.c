#include "pixvm.h"

#include <string.h>

// RGBA8 (de)interleave runs on whole vectors, never lane-by-lane: writing a
// register file slot one lane at a time aliases the other slots the compiler holds
// in registers, and -Os then spills/reloads the whole file around each store.
// Shuffles keep every access whole-vector, so the file stays put.
typedef uint8_t u8x8  __attribute__((ext_vector_type(PIXVM_N)));
typedef uint8_t u8x16 __attribute__((ext_vector_type(PIXVM_N * 2)));
typedef uint8_t u8x32 __attribute__((ext_vector_type(PIXVM_N * 4)));
static_assert(PIXVM_N == 8, "the RGBA8 (de)interleave shuffles assume 8 lanes");

static pixv unit_of(u8x8 v) {
    return __builtin_convertvector(v, pixv) / 255.0f;
}

static pixv clamp01(pixv v) {
    pixv lo = 0.0f, hi = 1.0f;
    return __builtin_elementwise_min(__builtin_elementwise_max(v, lo), hi);
}

// 8 RGBA8 pixels (r0g0b0a0 r1g1b1a1 ...) -> four unit-range channel vectors.
static void unpack8888(u8x32 raw, pixv *r, pixv *g, pixv *b, pixv *a) {
    *r = unit_of(__builtin_shufflevector(raw, raw, 0, 4,  8, 12, 16, 20, 24, 28));
    *g = unit_of(__builtin_shufflevector(raw, raw, 1, 5,  9, 13, 17, 21, 25, 29));
    *b = unit_of(__builtin_shufflevector(raw, raw, 2, 6, 10, 14, 18, 22, 26, 30));
    *a = unit_of(__builtin_shufflevector(raw, raw, 3, 7, 11, 15, 19, 23, 27, 31));
}

// Four channel vectors -> 8 interleaved RGBA8 pixels, clamped and quantized.
static u8x32 pack8888(pixv r, pixv g, pixv b, pixv a) {
    u8x8 r8 = __builtin_convertvector(clamp01(r) * 255.0f + 0.5f, u8x8);
    u8x8 g8 = __builtin_convertvector(clamp01(g) * 255.0f + 0.5f, u8x8);
    u8x8 b8 = __builtin_convertvector(clamp01(b) * 255.0f + 0.5f, u8x8);
    u8x8 a8 = __builtin_convertvector(clamp01(a) * 255.0f + 0.5f, u8x8);
    u8x16 rg = __builtin_shufflevector(r8, g8, 0, 8, 1, 9, 2, 10, 3, 11,
                                               4, 12, 5, 13, 6, 14, 7, 15);
    u8x16 ba = __builtin_shufflevector(b8, a8, 0, 8, 1, 9, 2, 10, 3, 11,
                                               4, 12, 5, 13, 6, 14, 7, 15);
    return __builtin_shufflevector(rg, ba, 0, 1, 16, 17, 2, 3, 18, 19,
                                           4, 5, 20, 21, 6, 7, 22, 23,
                                           8, 9, 24, 25, 10, 11, 26, 27,
                                           12, 13, 28, 29, 14, 15, 30, 31);
}

static uint8_t to_u8(float v) {
    if (v < 0.0f) { v = 0.0f; }
    if (v > 1.0f) { v = 1.0f; }
    return (uint8_t)(v * 255.0f + 0.5f);
}

void pixvm_run_switch(pixop const *__counted_by(prog_len) prog, int prog_len,
                      uint8_t *__counted_by(n * 4) dst,
                      uint8_t const *__counted_by_or_null(n * 4) src,
                      uint8_t const *__counted_by_or_null(n) cov, int n) {
    for (int x = 0; x < n; x += PIXVM_N) {
        int active = n - x < PIXVM_N ? n - x : PIXVM_N;
        bool full = active == PIXVM_N;  // whole-vector fast path; tail goes scalar
        pixv reg[PIXVM_REGS] = {0};
        for (int pc = 0; pc < prog_len; pc++) {
            pixop op = prog[pc];
            // Operands come from the bytecode, so reg[d]/reg[a]/... are
            // -fbounds-safety-checked against the file at runtime.
            int d = op.dst, a = op.a, b = op.b, c = op.c;
            switch ((int)op.kind) {
                case PIXOP_SPLAT:
                    reg[d] = op.imm;
                    break;
                case PIXOP_LOAD_SRC:
                    if (full) {
                        u8x32 raw;
                        memcpy(&raw, src + (size_t)x * 4, sizeof raw);
                        unpack8888(raw, &reg[d], &reg[d + 1], &reg[d + 2], &reg[d + 3]);
                    } else {
                        for (int lane = 0; lane < active; lane++) {
                            int p = (x + lane) * 4;
                            reg[d][lane]     = (float)src[p]     / 255.0f;
                            reg[d + 1][lane] = (float)src[p + 1] / 255.0f;
                            reg[d + 2][lane] = (float)src[p + 2] / 255.0f;
                            reg[d + 3][lane] = (float)src[p + 3] / 255.0f;
                        }
                    }
                    break;
                case PIXOP_LOAD_DST:
                    if (full) {
                        u8x32 raw;
                        memcpy(&raw, dst + (size_t)x * 4, sizeof raw);
                        unpack8888(raw, &reg[d], &reg[d + 1], &reg[d + 2], &reg[d + 3]);
                    } else {
                        for (int lane = 0; lane < active; lane++) {
                            int p = (x + lane) * 4;
                            reg[d][lane]     = (float)dst[p]     / 255.0f;
                            reg[d + 1][lane] = (float)dst[p + 1] / 255.0f;
                            reg[d + 2][lane] = (float)dst[p + 2] / 255.0f;
                            reg[d + 3][lane] = (float)dst[p + 3] / 255.0f;
                        }
                    }
                    break;
                case PIXOP_LOAD_COV:
                    if (!cov) {
                        reg[d] = 1.0f;
                    } else if (full) {
                        u8x8 cv;
                        memcpy(&cv, cov + (size_t)x, sizeof cv);
                        reg[d] = unit_of(cv);
                    } else {
                        for (int lane = 0; lane < active; lane++) {
                            reg[d][lane] = (float)cov[x + lane] / 255.0f;
                        }
                    }
                    break;
                case PIXOP_STORE:
                    if (full) {
                        u8x32 out = pack8888(reg[d], reg[d + 1], reg[d + 2], reg[d + 3]);
                        memcpy(dst + (size_t)x * 4, &out, sizeof out);
                    } else {
                        for (int lane = 0; lane < active; lane++) {
                            int p = (x + lane) * 4;
                            dst[p]     = to_u8(reg[d][lane]);
                            dst[p + 1] = to_u8(reg[d + 1][lane]);
                            dst[p + 2] = to_u8(reg[d + 2][lane]);
                            dst[p + 3] = to_u8(reg[d + 3][lane]);
                        }
                    }
                    break;
                case PIXOP_MOV: reg[d] = reg[a];               break;
                case PIXOP_ADD: reg[d] = reg[a] + reg[b];      break;
                case PIXOP_SUB: reg[d] = reg[a] - reg[b];      break;
                case PIXOP_MUL: reg[d] = reg[a] * reg[b];      break;
                case PIXOP_MAD: reg[d] = reg[a] * reg[b] + reg[c]; break;
                default: break;
            }
        }
    }
}
