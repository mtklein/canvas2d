// cnvs_grow_cap: capacity growth must never signed-overflow (the doubling loop
// used to compute n *= 2 unguarded, which is UB once need > INT_MAX/2 -- under
// the debug variant's UBSan this test aborts if that regresses).  It must always
// return a capacity >= need.  See secreview/SECURITY-REVIEW.md, Finding 2.

#include "test_util.h"

#include "cnvs_mem.h"

#include <limits.h>

int main(void) {
    CHECK(cnvs_grow_cap(0, 1) == 8);     // empty -> base capacity
    CHECK(cnvs_grow_cap(0, 8) == 8);
    CHECK(cnvs_grow_cap(8, 9) == 16);    // doubles
    CHECK(cnvs_grow_cap(8, 100) >= 100);
    CHECK(cnvs_grow_cap(64, 64) == 64);  // already big enough (need == cap)

    // Near the top of the range: doubling would overflow, so it must fall back
    // to need exactly -- and never produce a capacity below need.
    CHECK(cnvs_grow_cap(8, INT_MAX - 1) >= INT_MAX - 1);
    CHECK(cnvs_grow_cap(1 << 30, (1 << 30) + 1) == (1 << 30) + 1);
    CHECK(cnvs_grow_cap(INT_MAX / 2 + 1, INT_MAX) == INT_MAX);

    return TEST_REPORT();
}
