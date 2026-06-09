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

// An out-of-range register operand must trap.  STORE reads reg[20..23] of a
// 16-register file and writes the result to dst, so the OOB read is live (no DCE)
// and -fbounds-safety catches it.  Returns normally only if no trap fired.
static void run_bad_register(void) {
    int const n = PIXVM_N;
    int const len = n * 4;
    uint8_t *__counted_by(len) dst = calloc((size_t)len, 1);
    if (!dst) {
        return;
    }
    volatile int bad = PIXVM_REGS + 4;
    pixop prog[1] = { { .kind = PIXOP_STORE, .dst = (uint8_t)bad } };
    pixvm_run_switch(prog, 1, dst, NULL, NULL, n);
    free(dst);
}

int main(void) {
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
        pixvm_run_switch(copy, 2, dst, src, NULL, n);
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
        pixvm_run_switch(srcover, nops, px, NULL, cov, m);
        CHECK(px[0] == 0 && px[1] == 128 && px[2] == 128 && px[3] == 255);
        CHECK(px[(m - 1) * 4 + 1] == 128 && px[(m - 1) * 4 + 2] == 128);

        // cov == NULL behaves as fully opaque coverage: same result.
        for (int i = 0; i < m; i++) {
            px[i * 4] = 0; px[i * 4 + 1] = 0; px[i * 4 + 2] = 255; px[i * 4 + 3] = 255;
        }
        pixvm_run_switch(srcover, nops, px, NULL, NULL, m);
        CHECK(px[1] == 128 && px[2] == 128 && px[3] == 255);
    }
    free(px);
    free(cov);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        run_bad_register();
        _exit(0);  // reached only if no trap fired
    }
    int status = 0;
    (void)waitpid(pid, &status, 0);
    bool trapped = WIFSIGNALED(status);
    CHECK(trapped);  // bytecode register index is bounds-checked at runtime
    if (!trapped && WIFEXITED(status)) {
        (void)fprintf(stderr, "bad-register child exited %d without trapping\n",
                      WEXITSTATUS(status));
    }

    return TEST_REPORT();
}
