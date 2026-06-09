#include "pixvm.h"

#include "pixvm_pixio.h"

#include <ptrcheck.h>
#include <string.h>

// Design C: an SkRasterPipeline-style register pipeline.  The live colour channels
// ride through the stages as _Float16x8 *arguments* (r,g,b,a = working colour,
// dr..da = backdrop), so they stay in NEON registers across the whole program --
// no register-file memory, the ceiling A and B hit.  Each stage does its work and
// [[clang::musttail]]-jumps to the next; the chain of tail jumps is the pipeline.
//
// -fbounds-safety shapes this design the most (see docs/pixel-pipelines.md):
//   * The advancing program pointer can't be `__counted_by` through musttail (the
//     B finding), so the program walk is a raw `__unsafe_indexable` pointer -- the
//     dispatch is *unchecked*.  This is what SkRasterPipeline does too; the program
//     is small and built by trusted code, not attacker input.
//   * Each stage's `void *ctx` is forged back to its typed context (the qsort hole,
//     now per stage).  But a buffer's bound survives the void* round-trip by living
//     in a sibling `int len` field, so the pixel loads/stores stay __counted_by and
//     checked -- the part that actually touches large memory keeps its guard.
//   * Channels thread as vector args, x/active as ints -- no counted pointer in
//     argument position, so musttail is happy.  Stages return int (a void musttail
//     return trips -Wpedantic).

typedef struct pipe_stage pipe_stage;
typedef int (*pipe_fn)(pipe_stage const *__unsafe_indexable st, int x, int active,
                       pixv r, pixv g, pixv b, pixv a,
                       pixv dr, pixv dg, pixv db, pixv da);
struct pipe_stage {
    pipe_fn fn;
    void const *__unsafe_indexable ctx;
};

// Advance the raw program pointer and tail-jump to the next stage, carrying the
// (possibly updated) channels.  Reads st/x/active from the enclosing stage.
#define PIPE_NEXT(R, G, B, A, DR, DG, DB, DA) \
    [[clang::musttail]] return st[1].fn(st + 1, x, active, R, G, B, A, DR, DG, DB, DA)

// Per-stage contexts.  Buffers carry their own bound (`len`) so the access stays
// checked after the void* ctx is forged back to a typed pointer.
typedef struct { _Float16 r, g, b, a; } src_ctx;
typedef struct { uint8_t *__counted_by(len) p; int len; } wbuf_ctx;
typedef struct { uint8_t const *__counted_by_or_null(len) p; int len; } rbuf_ctx;

// Splat a constant premultiplied colour into the working channels.
static int st_src(pipe_stage const *__unsafe_indexable st, int x, int active,
                  pixv r, pixv g, pixv b, pixv a,
                  pixv dr, pixv dg, pixv db, pixv da) {
    src_ctx const *c = __unsafe_forge_single(src_ctx const *, st->ctx);
    r = c->r; g = c->g; b = c->b; a = c->a;
    PIPE_NEXT(r, g, b, a, dr, dg, db, da);
}

// Scale the working colour by per-pixel coverage (NULL ctx buffer -> coverage 1).
static int st_scale_cov(pipe_stage const *__unsafe_indexable st, int x, int active,
                        pixv r, pixv g, pixv b, pixv a,
                        pixv dr, pixv dg, pixv db, pixv da) {
    rbuf_ctx const *c = __unsafe_forge_single(rbuf_ctx const *, st->ctx);
    pixv cov = (_Float16)1.0f;
    if (c->p && active == PIXVM_N) {
        u8x8 cv;
        memcpy(&cv, c->p + (size_t)x, sizeof cv);  // c->p is __counted_by(len): checked
        cov = pixio_unit(cv);
    } else if (c->p) {
        for (int lane = 0; lane < active; lane++) {
            cov[lane] = pixio_from_u8(c->p[x + lane]);
        }
    }
    r = r * cov; g = g * cov; b = b * cov; a = a * cov;
    PIPE_NEXT(r, g, b, a, dr, dg, db, da);
}

// Load the backdrop into the dst channels.
static int st_load_dst(pipe_stage const *__unsafe_indexable st, int x, int active,
                       pixv r, pixv g, pixv b, pixv a,
                       pixv dr, pixv dg, pixv db, pixv da) {
    wbuf_ctx const *c = __unsafe_forge_single(wbuf_ctx const *, st->ctx);
    if (active == PIXVM_N) {
        u8x32 raw;
        memcpy(&raw, c->p + (size_t)x * 4, sizeof raw);
        pixio_unpack(raw, &dr, &dg, &db, &da);
    } else {
        dr = 0; dg = 0; db = 0; da = 0;
        for (int lane = 0; lane < active; lane++) {
            int p = (x + lane) * 4;
            dr[lane] = pixio_from_u8(c->p[p]);
            dg[lane] = pixio_from_u8(c->p[p + 1]);
            db[lane] = pixio_from_u8(c->p[p + 2]);
            da[lane] = pixio_from_u8(c->p[p + 3]);
        }
    }
    PIPE_NEXT(r, g, b, a, dr, dg, db, da);
}

// Premultiplied source-over: out = src + dst * (1 - src.a).
static int st_srcover(pipe_stage const *__unsafe_indexable st, int x, int active,
                      pixv r, pixv g, pixv b, pixv a,
                      pixv dr, pixv dg, pixv db, pixv da) {
    pixv ia = (_Float16)1.0f - a;
    r = r + dr * ia; g = g + dg * ia; b = b + db * ia; a = a + da * ia;
    PIPE_NEXT(r, g, b, a, dr, dg, db, da);
}

// Store the working colour to the destination.
static int st_store(pipe_stage const *__unsafe_indexable st, int x, int active,
                    pixv r, pixv g, pixv b, pixv a,
                    pixv dr, pixv dg, pixv db, pixv da) {
    wbuf_ctx const *c = __unsafe_forge_single(wbuf_ctx const *, st->ctx);
    if (active == PIXVM_N) {
        u8x32 out = pixio_pack(r, g, b, a);
        memcpy(c->p + (size_t)x * 4, &out, sizeof out);
    } else {
        for (int lane = 0; lane < active; lane++) {
            int p = (x + lane) * 4;
            c->p[p]     = pixio_to_u8((float)r[lane]);
            c->p[p + 1] = pixio_to_u8((float)g[lane]);
            c->p[p + 2] = pixio_to_u8((float)b[lane]);
            c->p[p + 3] = pixio_to_u8((float)a[lane]);
        }
    }
    PIPE_NEXT(r, g, b, a, dr, dg, db, da);
}

// Pipeline terminator: ends the tail-call chain for this chunk.
static int st_end(pipe_stage const *__unsafe_indexable st, int x, int active,
                  pixv r, pixv g, pixv b, pixv a,
                  pixv dr, pixv dg, pixv db, pixv da) {
    (void)st; (void)x; (void)active;
    (void)r; (void)g; (void)b; (void)a; (void)dr; (void)dg; (void)db; (void)da;
    return 0;
}

void pixvm_run_pipe(uint8_t *__counted_by(n * 4) dst,
                    uint8_t const *__counted_by_or_null(n) cov, int n) {
    src_ctx const src = { .r = (_Float16)0.0f, .g = (_Float16)0.5f,
                          .b = (_Float16)0.0f, .a = (_Float16)0.5f };  // premul green, 0.5 alpha
    wbuf_ctx const dstc = { .p = dst, .len = n * 4 };
    rbuf_ctx const covc = { .p = cov, .len = cov ? n : 0 };
    pipe_stage const prog[] = {
        { st_src,       &src },
        { st_scale_cov, &covc },
        { st_load_dst,  &dstc },
        { st_srcover,   NULL },
        { st_store,     &dstc },
        { st_end,       NULL },
    };
    for (int x = 0; x < n; x += PIXVM_N) {
        int active = n - x < PIXVM_N ? n - x : PIXVM_N;
        pixv z = (_Float16)0.0f;
        (void)prog[0].fn(prog, x, active, z, z, z, z, z, z, z, z);
    }
}
