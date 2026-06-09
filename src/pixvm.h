#pragma once

// A small vectorized VM for per-pixel processing, built to probe how
// -fbounds-safety interacts with three dispatch styles.  This header is the shared
// ISA; pixvm_run_switch (one big switch), pixvm_run_threaded (tail-call threaded),
// and pixvm_run_pipe (an SkRasterPipeline-style register pipeline) are the three
// backends.
//
// A run walks `n` pixels PIXVM_N at a time.  Colour channels are _Float16 in [0,1]
// (the project's colour type, and native 128-bit NEON at 8 wide), held in a
// register file of PIXVM_N-wide vectors; load/store ops convert to and from tightly
// packed RGBA8 with the u8 quantization done in float.  Any geometric coordinate
// would stay float32 -- none arise in this colour/coverage ISA.

#include <ptrcheck.h>
#include <stdint.h>

#define PIXVM_N 8       // pixels per step (vector lanes)
#define PIXVM_REGS 16   // vector registers in the file

typedef _Float16 pixv __attribute__((ext_vector_type(PIXVM_N)));

// LOAD_SRC/LOAD_DST/STORE treat `dst` as the base of a four-register r,g,b,a group;
// the arithmetic ops read `a`/`b`/`c` and write `dst`; SPLAT writes `dst` from imm.
typedef enum {
    PIXOP_SPLAT,     // dst = imm
    PIXOP_LOAD_SRC,  // dst..dst+3 = src RGBA8 / 255
    PIXOP_LOAD_DST,  // dst..dst+3 = dst RGBA8 / 255
    PIXOP_LOAD_COV,  // dst = cov / 255 (1 when cov is NULL)
    PIXOP_STORE,     // dst RGBA8 = clamp(dst..dst+3) * 255
    PIXOP_MOV,       // dst = a
    PIXOP_ADD,       // dst = a + b
    PIXOP_SUB,       // dst = a - b
    PIXOP_MUL,       // dst = a * b
    PIXOP_MAD,       // dst = a * b + c
    PIXOP_KIND_COUNT,
} pixop_kind;

typedef struct {
    uint8_t kind;
    uint8_t dst, a, b, c;
    float imm;
} pixop;

// Run `prog` over `n` pixels.  dst is required; src (RGBA8) and cov (one byte per
// pixel) may be NULL when the program never loads them, so they are
// __counted_by_or_null -- a plain __counted_by rejects a NULL argument at compile
// time once the count is a known nonzero constant.  Out-of-range register operands
// and pixel indices both trap under -fbounds-safety.
void pixvm_run_switch(pixop const *__counted_by(prog_len) prog, int prog_len,
                      uint8_t *__counted_by(n * 4) dst,
                      uint8_t const *__counted_by_or_null(n * 4) src,
                      uint8_t const *__counted_by_or_null(n) cov, int n);

// Design B: same ISA and semantics as pixvm_run_switch, dispatched by tail-call
// threading instead of a switch.
void pixvm_run_threaded(pixop const *__counted_by(prog_len) prog, int prog_len,
                        uint8_t *__counted_by(n * 4) dst,
                        uint8_t const *__counted_by_or_null(n * 4) src,
                        uint8_t const *__counted_by_or_null(n) cov, int n);

// Design C: an SkRasterPipeline-style register pipeline -- channels threaded in
// registers, not a register file -- written fully within -fbounds-safety (no unsafe
// pointers, no forge).  It runs a fixed program: premultiplied source-over of a
// constant colour (premul green, 0.5 alpha) scaled by `cov` onto dst, the same
// computation A and B express as a source-over bytecode program.
void pixvm_run_pipe(uint8_t *__counted_by(n * 4) dst,
                    uint8_t const *__counted_by_or_null(n) cov, int n);
