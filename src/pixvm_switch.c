#include "pixvm.h"

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
                    for (int lane = 0; lane < active; lane++) {
                        int p = (x + lane) * 4;
                        reg[d][lane]     = (float)src[p]     / 255.0f;
                        reg[d + 1][lane] = (float)src[p + 1] / 255.0f;
                        reg[d + 2][lane] = (float)src[p + 2] / 255.0f;
                        reg[d + 3][lane] = (float)src[p + 3] / 255.0f;
                    }
                    break;
                case PIXOP_LOAD_DST:
                    for (int lane = 0; lane < active; lane++) {
                        int p = (x + lane) * 4;
                        reg[d][lane]     = (float)dst[p]     / 255.0f;
                        reg[d + 1][lane] = (float)dst[p + 1] / 255.0f;
                        reg[d + 2][lane] = (float)dst[p + 2] / 255.0f;
                        reg[d + 3][lane] = (float)dst[p + 3] / 255.0f;
                    }
                    break;
                case PIXOP_LOAD_COV:
                    for (int lane = 0; lane < active; lane++) {
                        reg[d][lane] = cov ? (float)cov[x + lane] / 255.0f : 1.0f;
                    }
                    break;
                case PIXOP_STORE:
                    for (int lane = 0; lane < active; lane++) {
                        int p = (x + lane) * 4;
                        dst[p]     = to_u8(reg[d][lane]);
                        dst[p + 1] = to_u8(reg[d + 1][lane]);
                        dst[p + 2] = to_u8(reg[d + 2][lane]);
                        dst[p + 3] = to_u8(reg[d + 3][lane]);
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
