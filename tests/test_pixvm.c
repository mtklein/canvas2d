#include "pixvm.h"
#include "test_util.h"

#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

// Premultiplied source-over of a constant src colour, scaled by coverage, onto a
// loaded dst: out = src*cov + dst*(1 - src.a*cov).
static pixop const srcover[] = {
    { .kind = PIXOP_SPLAT, .dst = 0, .imm = 0.0f },  // src r,g,b,a (premultiplied)
    { .kind = PIXOP_SPLAT, .dst = 1, .imm = 0.5f },
    { .kind = PIXOP_SPLAT, .dst = 2, .imm = 0.0f },
    { .kind = PIXOP_SPLAT, .dst = 3, .imm = 0.5f },
    { .kind = PIXOP_LOAD_COV, .dst = 4 },
    { .kind = PIXOP_MUL, .dst = 0, .a = 0, .b = 4 },
    { .kind = PIXOP_MUL, .dst = 1, .a = 1, .b = 4 },
    { .kind = PIXOP_MUL, .dst = 2, .a = 2, .b = 4 },
    { .kind = PIXOP_MUL, .dst = 3, .a = 3, .b = 4 },
    { .kind = PIXOP_LOAD_DST, .dst = 8 },            // dst r,g,b,a in r8..r11
    { .kind = PIXOP_SPLAT, .dst = 5, .imm = 1.0f },
    { .kind = PIXOP_SUB, .dst = 6, .a = 5, .b = 3 }, // r6 = 1 - src.a
    { .kind = PIXOP_MAD, .dst = 8,  .a = 8,  .b = 6, .c = 0 },
    { .kind = PIXOP_MAD, .dst = 9,  .a = 9,  .b = 6, .c = 1 },
    { .kind = PIXOP_MAD, .dst = 10, .a = 10, .b = 6, .c = 2 },
    { .kind = PIXOP_MAD, .dst = 11, .a = 11, .b = 6, .c = 3 },
    { .kind = PIXOP_STORE, .dst = 8 },
};

static void run_vm(int threaded, pixop const *__counted_by(prog_len) prog, int prog_len,
                   uint8_t *__counted_by(n * 4) dst,
                   uint8_t const *__counted_by_or_null(n * 4) src,
                   uint8_t const *__counted_by_or_null(n) cov, int n) {
    if (threaded) {
        pixvm_run_threaded(prog, prog_len, dst, src, cov, n);
    } else {
        pixvm_run_switch(prog, prog_len, dst, src, cov, n);
    }
}

// Same programs and assertions for both backends; they must agree bit-for-bit.
static void check_backend(int threaded) {
    // Copy round-trip (LOAD_SRC then STORE); n is not a multiple of PIXVM_N, so the
    // final short chunk is exercised too.
    int const n = 20;
    int const len = n * 4;
    uint8_t *__counted_by(len) src = malloc((size_t)len);
    uint8_t *__counted_by(len) dst = malloc((size_t)len);
    CHECK(src != NULL && dst != NULL);
    if (src && dst) {
        for (int i = 0; i < len; i++) {
            src[i] = (uint8_t)(i * 7 + 1);
            dst[i] = 0;
        }
        pixop const copy[] = {
            { .kind = PIXOP_LOAD_SRC, .dst = 0 },
            { .kind = PIXOP_STORE, .dst = 0 },
        };
        run_vm(threaded, copy, 2, dst, src, NULL, n);
        bool same = true;
        for (int i = 0; i < len; i++) {
            if (dst[i] != src[i]) {
                same = false;
            }
        }
        CHECK(same);  // u8 -> float -> u8 is exact at integer values
    }
    free(src);
    free(dst);

    // Source-over: premultiplied green at 0.5 alpha over opaque blue, full coverage
    // -> (0, 0.5, 0.5, 1.0) -> RGBA8 (0, 128, 128, 255).
    int const m = 50;
    int const mlen = m * 4;
    uint8_t *__counted_by(mlen) px = malloc((size_t)mlen);
    uint8_t *__counted_by(m) cov = malloc((size_t)m);
    CHECK(px != NULL && cov != NULL);
    if (px && cov) {
        int const nops = (int)(sizeof srcover / sizeof srcover[0]);
        for (int i = 0; i < m; i++) {
            px[i * 4] = 0; px[i * 4 + 1] = 0; px[i * 4 + 2] = 255; px[i * 4 + 3] = 255;
            cov[i] = 255;
        }
        run_vm(threaded, srcover, nops, px, NULL, cov, m);
        CHECK(px[0] == 0 && px[1] == 128 && px[2] == 128 && px[3] == 255);
        CHECK(px[(m - 1) * 4 + 1] == 128 && px[(m - 1) * 4 + 2] == 128);

        // cov == NULL behaves as fully opaque coverage: same result.
        for (int i = 0; i < m; i++) {
            px[i * 4] = 0; px[i * 4 + 1] = 0; px[i * 4 + 2] = 255; px[i * 4 + 3] = 255;
        }
        run_vm(threaded, srcover, nops, px, NULL, NULL, m);
        CHECK(px[1] == 128 && px[2] == 128 && px[3] == 255);
    }
    free(px);
    free(cov);
}

// Design C runs a fixed source-over pipeline; it must agree with the A/B program.
static void check_pipe(void) {
    int const m = 50;
    int const mlen = m * 4;
    uint8_t *__counted_by(mlen) px = malloc((size_t)mlen);
    uint8_t *__counted_by(m) cov = malloc((size_t)m);
    CHECK(px != NULL && cov != NULL);
    if (px && cov) {
        for (int i = 0; i < m; i++) {
            px[i * 4] = 0; px[i * 4 + 1] = 0; px[i * 4 + 2] = 255; px[i * 4 + 3] = 255;
            cov[i] = 255;
        }
        pixvm_run_pipe(px, cov, m);
        CHECK(px[0] == 0 && px[1] == 128 && px[2] == 128 && px[3] == 255);
        CHECK(px[(m - 1) * 4 + 1] == 128 && px[(m - 1) * 4 + 2] == 128);

        for (int i = 0; i < m; i++) {
            px[i * 4] = 0; px[i * 4 + 1] = 0; px[i * 4 + 2] = 255; px[i * 4 + 3] = 255;
        }
        pixvm_run_pipe(px, NULL, m);  // NULL coverage == fully opaque
        CHECK(px[1] == 128 && px[2] == 128 && px[3] == 255);
    }
    free(px);
    free(cov);
}

// Run a one-instruction program under `threaded` in a child; return its wait status.
static int run_in_child(int threaded, pixop prog0) {
    pid_t pid = fork();
    if (pid == 0) {
        int const n = PIXVM_N;
        int const len = n * 4;
        uint8_t *__counted_by(len) dst = calloc((size_t)len, 1);
        if (dst) {
            pixop prog[1] = { prog0 };
            run_vm(threaded, prog, 1, dst, NULL, NULL, n);
            free(dst);
        }
        _exit(0);  // reached only if nothing trapped
    }
    int status = 0;
    (void)waitpid(pid, &status, 0);
    return status;
}

int main(void) {
    check_backend(0);  // switch
    check_backend(1);  // threaded
    check_pipe();      // SkRasterPipeline-style, fully bounds-checked

    // An out-of-range register operand traps in both backends: STORE reads
    // reg[20..23] of a 16-register file and writes the result to dst, so the OOB
    // read is live and -fbounds-safety catches it.
    volatile int bad_reg = PIXVM_REGS + 4;
    pixop store_bad = { .kind = PIXOP_STORE, .dst = (uint8_t)bad_reg };
    int reg_sw = run_in_child(0, store_bad);  // WIF* macros need an lvalue
    int reg_th = run_in_child(1, store_bad);
    CHECK(WIFSIGNALED(reg_sw));  // switch traps
    CHECK(WIFSIGNALED(reg_th));  // threaded traps

    // An out-of-range opcode traps only in the threaded backend, where the bytecode
    // value indexes the bounds-checked handler table.  The switch's `default` makes
    // it a silent no-op -- a real safety difference between the dispatch styles.
    volatile int bad_op = PIXOP_KIND_COUNT + 3;
    pixop op_bad = { .kind = (uint8_t)bad_op, .dst = 0 };
    int sw = run_in_child(0, op_bad);
    int th = run_in_child(1, op_bad);
    CHECK(WIFEXITED(sw) && WEXITSTATUS(sw) == 0);  // switch: no trap (default: break)
    CHECK(WIFSIGNALED(th));                        // threaded: handler-table index traps

    return TEST_REPORT();
}
