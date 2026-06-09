#include "pixvm.h"

#include "pixvm_pixio.h"

#include <string.h>

// Design C: an SkRasterPipeline-style register pipeline, fully bounds-checked.  The
// live colour channels ride through the stages as _Float16x8 *arguments* (r,g,b,a =
// working colour, dr..da = backdrop), so they stay in NEON registers across the
// whole program -- there is no register file, the memory ceiling A and B both hit.
// Each stage does its work and [[clang::musttail]]-jumps to the next; the chain of
// tail jumps is the pipeline.  It runs the same source-over computation A and B
// express as bytecode (verified bit-identical in test_pixvm).
//
// This stays entirely within -fbounds-safety -- no __unsafe_indexable, no forge.
// The two places SkRasterPipeline's C++ reaches for a raw pointer have checked
// spellings that cost nothing measurable (proved by benchmarking both):
//   * The program is a __counted_by array walked by a checked `int` index threaded
//     through musttail.  A counted *pointer* can't ride musttail (passing prog+1
//     into a narrower count inserts a check, which musttail rejects), but a plain
//     index can, and prog[idx] is bounds-checked.
//   * The per-stage context is a typed union, not a void* forged per stage.  Its
//     buffer members keep `__counted_by(len)` with a sibling `len`, so after a plain
//     union access the pixel loads/stores are still checked.  (Counted pointers
//     inside a union compile and check fine.)
// Channels thread as vector args (no counted pointer in argument position), so
// musttail lowers to indirect jumps; stages return int (a void musttail return
// trips -Wpedantic).

typedef struct pipe_state pipe_state;
typedef int (*pipe_fn)(pipe_state const *p, int idx, int x, int active,
                       pixv r, pixv g, pixv b, pixv a,
                       pixv dr, pixv dg, pixv db, pixv da);

// Per-stage context: a typed union so the bounds survive without a forge.  Buffer
// members carry their own `len`, so the pixel accesses stay __counted_by-checked.
typedef union {
    struct { _Float16 r, g, b, a; } color;
    struct { uint8_t *__counted_by(len) p; int len; } wbuf;
    struct { uint8_t const *__counted_by_or_null(len) p; int len; } rbuf;
} pipe_ctx;

typedef struct {
    pipe_fn fn;
    pipe_ctx ctx;
} pipe_stage;

struct pipe_state {
    pipe_stage const *__counted_by(nstages) prog;
    int nstages;
};

static int dispatch(pipe_state const *p, int idx, int x, int active,
                    pixv r, pixv g, pixv b, pixv a,
                    pixv dr, pixv dg, pixv db, pixv da);

// Thread the (updated) channels to the next stage; checked index walk, no raw ptr.
#define PIPE_NEXT(R, G, B, A, DR, DG, DB, DA) \
    [[clang::musttail]] return dispatch(p, idx + 1, x, active, R, G, B, A, DR, DG, DB, DA)

// Splat a constant premultiplied colour into the working channels.
static int st_src(pipe_state const *p, int idx, int x, int active,
                  pixv r, pixv g, pixv b, pixv a,
                  pixv dr, pixv dg, pixv db, pixv da) {
    pipe_ctx c = p->prog[idx].ctx;  // checked: idx < nstages
    r = c.color.r; g = c.color.g; b = c.color.b; a = c.color.a;
    PIPE_NEXT(r, g, b, a, dr, dg, db, da);
}

// Scale the working colour by per-pixel coverage (NULL ctx buffer -> coverage 1).
static int st_scale_cov(pipe_state const *p, int idx, int x, int active,
                        pixv r, pixv g, pixv b, pixv a,
                        pixv dr, pixv dg, pixv db, pixv da) {
    pipe_ctx c = p->prog[idx].ctx;
    pixv cov = (_Float16)1.0f;
    if (c.rbuf.p && active == PIXVM_N) {
        u8x8 cv;
        memcpy(&cv, c.rbuf.p + (size_t)x, sizeof cv);  // checked against c.rbuf.len
        cov = pixio_unit(cv);
    } else if (c.rbuf.p) {
        for (int lane = 0; lane < active; lane++) {
            cov[lane] = pixio_from_u8(c.rbuf.p[x + lane]);
        }
    }
    r = r * cov; g = g * cov; b = b * cov; a = a * cov;
    PIPE_NEXT(r, g, b, a, dr, dg, db, da);
}

// Load the backdrop into the dst channels.
static int st_load_dst(pipe_state const *p, int idx, int x, int active,
                       pixv r, pixv g, pixv b, pixv a,
                       pixv dr, pixv dg, pixv db, pixv da) {
    pipe_ctx c = p->prog[idx].ctx;
    if (active == PIXVM_N) {
        u8x32 raw;
        memcpy(&raw, c.wbuf.p + (size_t)x * 4, sizeof raw);  // checked against c.wbuf.len
        pixio_unpack(raw, &dr, &dg, &db, &da);
    } else {
        dr = 0; dg = 0; db = 0; da = 0;
        for (int lane = 0; lane < active; lane++) {
            int q = (x + lane) * 4;
            dr[lane] = pixio_from_u8(c.wbuf.p[q]);
            dg[lane] = pixio_from_u8(c.wbuf.p[q + 1]);
            db[lane] = pixio_from_u8(c.wbuf.p[q + 2]);
            da[lane] = pixio_from_u8(c.wbuf.p[q + 3]);
        }
    }
    PIPE_NEXT(r, g, b, a, dr, dg, db, da);
}

// Premultiplied source-over: out = src + dst * (1 - src.a).
static int st_srcover(pipe_state const *p, int idx, int x, int active,
                      pixv r, pixv g, pixv b, pixv a,
                      pixv dr, pixv dg, pixv db, pixv da) {
    pixv ia = (_Float16)1.0f - a;
    r = r + dr * ia; g = g + dg * ia; b = b + db * ia; a = a + da * ia;
    PIPE_NEXT(r, g, b, a, dr, dg, db, da);
}

// Store the working colour to the destination.
static int st_store(pipe_state const *p, int idx, int x, int active,
                    pixv r, pixv g, pixv b, pixv a,
                    pixv dr, pixv dg, pixv db, pixv da) {
    pipe_ctx c = p->prog[idx].ctx;
    if (active == PIXVM_N) {
        u8x32 out = pixio_pack(r, g, b, a);
        memcpy(c.wbuf.p + (size_t)x * 4, &out, sizeof out);
    } else {
        for (int lane = 0; lane < active; lane++) {
            int q = (x + lane) * 4;
            c.wbuf.p[q]     = pixio_to_u8((float)r[lane]);
            c.wbuf.p[q + 1] = pixio_to_u8((float)g[lane]);
            c.wbuf.p[q + 2] = pixio_to_u8((float)b[lane]);
            c.wbuf.p[q + 3] = pixio_to_u8((float)a[lane]);
        }
    }
    PIPE_NEXT(r, g, b, a, dr, dg, db, da);
}

static int dispatch(pipe_state const *p, int idx, int x, int active,
                    pixv r, pixv g, pixv b, pixv a,
                    pixv dr, pixv dg, pixv db, pixv da) {
    if (idx >= p->nstages) {
        return 0;  // pipeline done for this chunk
    }
    [[clang::musttail]] return p->prog[idx].fn(p, idx, x, active,
                                               r, g, b, a, dr, dg, db, da);
}

void pixvm_run_pipe(uint8_t *__counted_by(n * 4) dst,
                    uint8_t const *__counted_by_or_null(n) cov, int n) {
    pipe_stage const prog[] = {
        { st_src,       { .color = { (_Float16)0.0f, (_Float16)0.5f,
                                     (_Float16)0.0f, (_Float16)0.5f } } },  // premul green
        { st_scale_cov, { .rbuf  = { cov, cov ? n : 0 } } },
        { st_load_dst,  { .wbuf  = { dst, n * 4 } } },
        { st_srcover,   { .color = { 0, 0, 0, 0 } } },
        { st_store,     { .wbuf  = { dst, n * 4 } } },
    };
    pipe_state const p = { .prog = prog, .nstages = (int)(sizeof prog / sizeof prog[0]) };
    for (int x = 0; x < n; x += PIXVM_N) {
        int active = n - x < PIXVM_N ? n - x : PIXVM_N;
        pixv z = (_Float16)0.0f;
        (void)dispatch(&p, 0, x, active, z, z, z, z, z, z, z, z);
    }
}
