#include "pixvm.h"

#include "pixvm_pixio.h"

#include <string.h>

// Design A: one big switch.  An outer loop over PIXVM_N-pixel chunks, an inner loop
// over the program, a switch on the opcode.  The register file is a stack array
// indexed by bytecode-supplied operands (so the indices are -fbounds-safety-checked
// at runtime); load/store go through the shared whole-vector pixel I/O.

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
            int d = op.dst, a = op.a, b = op.b, c = op.c;
            switch ((int)op.kind) {
                case PIXOP_SPLAT:
                    reg[d] = (_Float16)op.imm;
                    break;
                case PIXOP_LOAD_SRC:
                    if (full) {
                        u8x32 raw;
                        memcpy(&raw, src + (size_t)x * 4, sizeof raw);
                        pixio_unpack(raw, &reg[d], &reg[d + 1], &reg[d + 2], &reg[d + 3]);
                    } else {
                        for (int lane = 0; lane < active; lane++) {
                            int p = (x + lane) * 4;
                            reg[d][lane]     = pixio_from_u8(src[p]);
                            reg[d + 1][lane] = pixio_from_u8(src[p + 1]);
                            reg[d + 2][lane] = pixio_from_u8(src[p + 2]);
                            reg[d + 3][lane] = pixio_from_u8(src[p + 3]);
                        }
                    }
                    break;
                case PIXOP_LOAD_DST:
                    if (full) {
                        u8x32 raw;
                        memcpy(&raw, dst + (size_t)x * 4, sizeof raw);
                        pixio_unpack(raw, &reg[d], &reg[d + 1], &reg[d + 2], &reg[d + 3]);
                    } else {
                        for (int lane = 0; lane < active; lane++) {
                            int p = (x + lane) * 4;
                            reg[d][lane]     = pixio_from_u8(dst[p]);
                            reg[d + 1][lane] = pixio_from_u8(dst[p + 1]);
                            reg[d + 2][lane] = pixio_from_u8(dst[p + 2]);
                            reg[d + 3][lane] = pixio_from_u8(dst[p + 3]);
                        }
                    }
                    break;
                case PIXOP_LOAD_COV:
                    if (!cov) {
                        reg[d] = (_Float16)1.0f;
                    } else if (full) {
                        u8x8 cv;
                        memcpy(&cv, cov + (size_t)x, sizeof cv);
                        reg[d] = pixio_unit(cv);
                    } else {
                        for (int lane = 0; lane < active; lane++) {
                            reg[d][lane] = pixio_from_u8(cov[x + lane]);
                        }
                    }
                    break;
                case PIXOP_STORE:
                    if (full) {
                        u8x32 out = pixio_pack(reg[d], reg[d + 1], reg[d + 2], reg[d + 3]);
                        memcpy(dst + (size_t)x * 4, &out, sizeof out);
                    } else {
                        for (int lane = 0; lane < active; lane++) {
                            int p = (x + lane) * 4;
                            dst[p]     = pixio_to_u8((float)reg[d][lane]);
                            dst[p + 1] = pixio_to_u8((float)reg[d + 1][lane]);
                            dst[p + 2] = pixio_to_u8((float)reg[d + 2][lane]);
                            dst[p + 3] = pixio_to_u8((float)reg[d + 3][lane]);
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
