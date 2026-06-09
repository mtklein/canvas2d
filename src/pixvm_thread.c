#include "pixvm.h"

#include "pixvm_pixio.h"

#include <string.h>

// Design B: tail-call threaded dispatch.  Each opcode is a handler that does its
// work and [[clang::musttail]]-jumps to the next handler through a function-pointer
// table -- no switch, no loop; the chain of jumps is the interpreter.
//
// -fbounds-safety shapes this design (see docs/pixel-pipelines.md):
//   * The instruction pointer can't be a threaded `__counted_by` argument: passing
//     `prog + pc + 1` into a narrower-counted parameter inserts a bound check, and
//     musttail rejects a call whose result isn't returned directly.  So the counted
//     `prog` lives in the state struct (checked there) and only the __single state
//     pointer plus a plain `int pc` ride through the tail calls.
//   * Handlers return int, not void: `return void_call();` under musttail trips
//     -Wpedantic's "void function should not return void expression".
//   * A bad opcode traps for free -- handlers[kind] indexes a PIXOP_KIND_COUNT-sized
//     table, bounds-checked like any array.

typedef struct {
    pixv reg[PIXVM_REGS];
    int n;
    uint8_t *__counted_by(n * 4) dst;
    uint8_t const *__counted_by_or_null(n * 4) src;
    uint8_t const *__counted_by_or_null(n) cov;
    pixop const *__counted_by(prog_len) prog;
    int prog_len;
    int x;       // current chunk's pixel base
    int active;  // lanes live this chunk
} vmstate;

typedef int (*pixop_fn)(vmstate *st, int pc);

static int dispatch(vmstate *st, int pc);

static int op_splat(vmstate *st, int pc) {
    pixop op = st->prog[pc];
    st->reg[op.dst] = (_Float16)op.imm;
    [[clang::musttail]] return dispatch(st, pc + 1);
}

static int op_load_src(vmstate *st, int pc) {
    int d = st->prog[pc].dst;
    if (st->active == PIXVM_N) {
        u8x32 raw;
        memcpy(&raw, st->src + (size_t)st->x * 4, sizeof raw);
        pixio_unpack(raw, &st->reg[d], &st->reg[d + 1], &st->reg[d + 2], &st->reg[d + 3]);
    } else {
        for (int lane = 0; lane < st->active; lane++) {
            int p = (st->x + lane) * 4;
            st->reg[d][lane]     = pixio_from_u8(st->src[p]);
            st->reg[d + 1][lane] = pixio_from_u8(st->src[p + 1]);
            st->reg[d + 2][lane] = pixio_from_u8(st->src[p + 2]);
            st->reg[d + 3][lane] = pixio_from_u8(st->src[p + 3]);
        }
    }
    [[clang::musttail]] return dispatch(st, pc + 1);
}

static int op_load_dst(vmstate *st, int pc) {
    int d = st->prog[pc].dst;
    if (st->active == PIXVM_N) {
        u8x32 raw;
        memcpy(&raw, st->dst + (size_t)st->x * 4, sizeof raw);
        pixio_unpack(raw, &st->reg[d], &st->reg[d + 1], &st->reg[d + 2], &st->reg[d + 3]);
    } else {
        for (int lane = 0; lane < st->active; lane++) {
            int p = (st->x + lane) * 4;
            st->reg[d][lane]     = pixio_from_u8(st->dst[p]);
            st->reg[d + 1][lane] = pixio_from_u8(st->dst[p + 1]);
            st->reg[d + 2][lane] = pixio_from_u8(st->dst[p + 2]);
            st->reg[d + 3][lane] = pixio_from_u8(st->dst[p + 3]);
        }
    }
    [[clang::musttail]] return dispatch(st, pc + 1);
}

static int op_load_cov(vmstate *st, int pc) {
    int d = st->prog[pc].dst;
    if (!st->cov) {
        st->reg[d] = (_Float16)1.0f;
    } else if (st->active == PIXVM_N) {
        u8x8 cv;
        memcpy(&cv, st->cov + (size_t)st->x, sizeof cv);
        st->reg[d] = pixio_unit(cv);
    } else {
        for (int lane = 0; lane < st->active; lane++) {
            st->reg[d][lane] = pixio_from_u8(st->cov[st->x + lane]);
        }
    }
    [[clang::musttail]] return dispatch(st, pc + 1);
}

static int op_store(vmstate *st, int pc) {
    int d = st->prog[pc].dst;
    if (st->active == PIXVM_N) {
        u8x32 out = pixio_pack(st->reg[d], st->reg[d + 1], st->reg[d + 2], st->reg[d + 3]);
        memcpy(st->dst + (size_t)st->x * 4, &out, sizeof out);
    } else {
        for (int lane = 0; lane < st->active; lane++) {
            int p = (st->x + lane) * 4;
            st->dst[p]     = pixio_to_u8((float)st->reg[d][lane]);
            st->dst[p + 1] = pixio_to_u8((float)st->reg[d + 1][lane]);
            st->dst[p + 2] = pixio_to_u8((float)st->reg[d + 2][lane]);
            st->dst[p + 3] = pixio_to_u8((float)st->reg[d + 3][lane]);
        }
    }
    [[clang::musttail]] return dispatch(st, pc + 1);
}

static int op_mov(vmstate *st, int pc) {
    pixop op = st->prog[pc];
    st->reg[op.dst] = st->reg[op.a];
    [[clang::musttail]] return dispatch(st, pc + 1);
}

static int op_add(vmstate *st, int pc) {
    pixop op = st->prog[pc];
    st->reg[op.dst] = st->reg[op.a] + st->reg[op.b];
    [[clang::musttail]] return dispatch(st, pc + 1);
}

static int op_sub(vmstate *st, int pc) {
    pixop op = st->prog[pc];
    st->reg[op.dst] = st->reg[op.a] - st->reg[op.b];
    [[clang::musttail]] return dispatch(st, pc + 1);
}

static int op_mul(vmstate *st, int pc) {
    pixop op = st->prog[pc];
    st->reg[op.dst] = st->reg[op.a] * st->reg[op.b];
    [[clang::musttail]] return dispatch(st, pc + 1);
}

static int op_mad(vmstate *st, int pc) {
    pixop op = st->prog[pc];
    st->reg[op.dst] = st->reg[op.a] * st->reg[op.b] + st->reg[op.c];
    [[clang::musttail]] return dispatch(st, pc + 1);
}

static pixop_fn const handlers[PIXOP_KIND_COUNT] = {
    [PIXOP_SPLAT]    = op_splat,
    [PIXOP_LOAD_SRC] = op_load_src,
    [PIXOP_LOAD_DST] = op_load_dst,
    [PIXOP_LOAD_COV] = op_load_cov,
    [PIXOP_STORE]    = op_store,
    [PIXOP_MOV]      = op_mov,
    [PIXOP_ADD]      = op_add,
    [PIXOP_SUB]      = op_sub,
    [PIXOP_MUL]      = op_mul,
    [PIXOP_MAD]      = op_mad,
};

static int dispatch(vmstate *st, int pc) {
    if (pc >= st->prog_len) {
        return 0;  // program done for this chunk
    }
    // Bytecode-supplied opcode indexes the table -- a bad opcode traps here.
    [[clang::musttail]] return handlers[st->prog[pc].kind](st, pc);
}

void pixvm_run_threaded(pixop const *__counted_by(prog_len) prog, int prog_len,
                        uint8_t *__counted_by(n * 4) dst,
                        uint8_t const *__counted_by_or_null(n * 4) src,
                        uint8_t const *__counted_by_or_null(n) cov, int n) {
    vmstate st = { .n = n, .dst = dst, .src = src, .cov = cov,
                   .prog = prog, .prog_len = prog_len };
    for (st.x = 0; st.x < n; st.x += PIXVM_N) {
        st.active = n - st.x < PIXVM_N ? n - st.x : PIXVM_N;
        memset(st.reg, 0, sizeof st.reg);
        (void)dispatch(&st, 0);
    }
}
