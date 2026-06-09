#include "pixvm.h"

#include "pixvm_pixio.h"

#include <string.h>

// Design D: design C with the unsafe seams removed -- how much of the register
// pipeline survives strict -fbounds-safety, no __unsafe_indexable, no forge?
//
// Kept from C: the live channels still thread as _Float16x8 *arguments* through
// musttail, so they stay in registers (no register file).  That part needs no
// unsafe anything -- vectors carry no pointers.
//
// Forced changes (see docs/pixel-pipelines.md):
//   * C's raw advancing program pointer is gone.  A __counted_by pointer can't
//     thread through musttail (the B finding), so the program lives in a struct as
//     a __counted_by array and the stages thread an int index -- a checked walk,
//     but it reloads + re-checks per stage (B's tax) instead of C's free raw bump.
//   * C's void* ctx + per-stage forge is gone.  The context is a typed union in the
//     stage struct; buffers keep their __counted_by bound (sibling `len`), so the
//     pixel loads/stores stay checked with no forge.

typedef struct dpipe dpipe;
typedef int (*dstage_fn)(dpipe const *st, int idx, int x, int active,
                         pixv r, pixv g, pixv b, pixv a,
                         pixv dr, pixv dg, pixv db, pixv da);

typedef union {
    struct { _Float16 r, g, b, a; } color;
    struct { uint8_t *__counted_by(len) p; int len; } wbuf;
    struct { uint8_t const *__counted_by_or_null(len) p; int len; } rbuf;
} dctx;

typedef struct {
    dstage_fn fn;
    dctx ctx;
} dstage;

struct dpipe {
    dstage const *__counted_by(nstages) prog;
    int nstages;
};

static int ddispatch(dpipe const *st, int idx, int x, int active,
                     pixv r, pixv g, pixv b, pixv a,
                     pixv dr, pixv dg, pixv db, pixv da);

// Thread the (updated) channels to the next stage; checked index walk, no raw ptr.
#define DPIPE_NEXT(R, G, B, A, DR, DG, DB, DA) \
    [[clang::musttail]] return ddispatch(st, idx + 1, x, active, R, G, B, A, DR, DG, DB, DA)

static int dst_src(dpipe const *st, int idx, int x, int active,
                   pixv r, pixv g, pixv b, pixv a,
                   pixv dr, pixv dg, pixv db, pixv da) {
    dctx c = st->prog[idx].ctx;  // checked: idx < nstages
    r = c.color.r; g = c.color.g; b = c.color.b; a = c.color.a;
    DPIPE_NEXT(r, g, b, a, dr, dg, db, da);
}

static int dst_scale_cov(dpipe const *st, int idx, int x, int active,
                         pixv r, pixv g, pixv b, pixv a,
                         pixv dr, pixv dg, pixv db, pixv da) {
    dctx c = st->prog[idx].ctx;
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
    DPIPE_NEXT(r, g, b, a, dr, dg, db, da);
}

static int dst_load_dst(dpipe const *st, int idx, int x, int active,
                        pixv r, pixv g, pixv b, pixv a,
                        pixv dr, pixv dg, pixv db, pixv da) {
    dctx c = st->prog[idx].ctx;
    if (active == PIXVM_N) {
        u8x32 raw;
        memcpy(&raw, c.wbuf.p + (size_t)x * 4, sizeof raw);  // checked against c.wbuf.len
        pixio_unpack(raw, &dr, &dg, &db, &da);
    } else {
        dr = 0; dg = 0; db = 0; da = 0;
        for (int lane = 0; lane < active; lane++) {
            int p = (x + lane) * 4;
            dr[lane] = pixio_from_u8(c.wbuf.p[p]);
            dg[lane] = pixio_from_u8(c.wbuf.p[p + 1]);
            db[lane] = pixio_from_u8(c.wbuf.p[p + 2]);
            da[lane] = pixio_from_u8(c.wbuf.p[p + 3]);
        }
    }
    DPIPE_NEXT(r, g, b, a, dr, dg, db, da);
}

static int dst_srcover(dpipe const *st, int idx, int x, int active,
                       pixv r, pixv g, pixv b, pixv a,
                       pixv dr, pixv dg, pixv db, pixv da) {
    pixv ia = (_Float16)1.0f - a;
    r = r + dr * ia; g = g + dg * ia; b = b + db * ia; a = a + da * ia;
    DPIPE_NEXT(r, g, b, a, dr, dg, db, da);
}

static int dst_store(dpipe const *st, int idx, int x, int active,
                     pixv r, pixv g, pixv b, pixv a,
                     pixv dr, pixv dg, pixv db, pixv da) {
    dctx c = st->prog[idx].ctx;
    if (active == PIXVM_N) {
        u8x32 out = pixio_pack(r, g, b, a);
        memcpy(c.wbuf.p + (size_t)x * 4, &out, sizeof out);
    } else {
        for (int lane = 0; lane < active; lane++) {
            int p = (x + lane) * 4;
            c.wbuf.p[p]     = pixio_to_u8((float)r[lane]);
            c.wbuf.p[p + 1] = pixio_to_u8((float)g[lane]);
            c.wbuf.p[p + 2] = pixio_to_u8((float)b[lane]);
            c.wbuf.p[p + 3] = pixio_to_u8((float)a[lane]);
        }
    }
    DPIPE_NEXT(r, g, b, a, dr, dg, db, da);
}

static int ddispatch(dpipe const *st, int idx, int x, int active,
                     pixv r, pixv g, pixv b, pixv a,
                     pixv dr, pixv dg, pixv db, pixv da) {
    if (idx >= st->nstages) {
        return 0;
    }
    [[clang::musttail]] return st->prog[idx].fn(st, idx, x, active,
                                                r, g, b, a, dr, dg, db, da);
}

void pixvm_run_pipe_checked(uint8_t *__counted_by(n * 4) dst,
                            uint8_t const *__counted_by_or_null(n) cov, int n) {
    dstage const prog[] = {
        { dst_src,       { .color = { (_Float16)0.0f, (_Float16)0.5f,
                                      (_Float16)0.0f, (_Float16)0.5f } } },
        { dst_scale_cov, { .rbuf  = { cov, cov ? n : 0 } } },
        { dst_load_dst,  { .wbuf  = { dst, n * 4 } } },
        { dst_srcover,   { .color = { 0, 0, 0, 0 } } },
        { dst_store,     { .wbuf  = { dst, n * 4 } } },
    };
    dpipe const st = { .prog = prog, .nstages = (int)(sizeof prog / sizeof prog[0]) };
    for (int x = 0; x < n; x += PIXVM_N) {
        int active = n - x < PIXVM_N ? n - x : PIXVM_N;
        pixv z = (_Float16)0.0f;
        (void)ddispatch(&st, 0, x, active, z, z, z, z, z, z, z, z);
    }
}
