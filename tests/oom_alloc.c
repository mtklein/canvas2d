// Fault-injecting allocator implementation.  Compiled WITHOUT the
// -Dmalloc=cnvs_oom_malloc redefines, so malloc/realloc/calloc below are the real
// libc ones -- and WITHOUT -fbounds-safety: its alloc_size return check traps a
// non-NULL return whose size is zero, but a faithful wrapper must pass libc's
// non-NULL malloc(0)/calloc(0, n) block through verbatim (checked callers still
// get size tracking from oom_alloc.h's alloc_size declarations).  See oom_alloc.h
// and docs/bounds-safety.md.

#include "oom_alloc.h"

#include <stdlib.h>

static int fail_k = 0;  // which allocation to fail (1-indexed); 0 = armed off
static int seen = 0;    // allocations since the last fail_at

void cnvs_oom_fail_at(int k) {
    fail_k = k;
    seen = 0;
}

int cnvs_oom_seen(void) {
    return seen;
}

static int trip(void) {
    seen += 1;
    return fail_k > 0 && seen == fail_k;
}

void *cnvs_oom_malloc(size_t size) {
    return trip() ? NULL : malloc(size);
}

void *cnvs_oom_realloc(void *p, size_t size) {
    // On the injected failure realloc returns NULL and leaves the original block
    // intact -- exactly what the callers' `if (!nd) return false` paths assume.
    return trip() ? NULL : realloc(p, size);
}

void *cnvs_oom_calloc(size_t n, size_t size) {
    return trip() ? NULL : calloc(n, size);
}
