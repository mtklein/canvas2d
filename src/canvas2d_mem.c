#include "canvas2d_mem.h"

#include <limits.h>

int canvas2d_grow_cap(int cap, int need) {
    int n = cap > 0 ? cap : 8;
    while (n < need) {
        // Doubling past INT_MAX/2 would overflow (signed overflow is UB); fall
        // back to the exact `need`.  Callers null-check the ensuing realloc, so
        // an unsatisfiably large request fails there rather than wrapping to a
        // small capacity and defeating the __counted_by bound it feeds.
        if (n > INT_MAX / 2) {
            return need;
        }
        n *= 2;
    }
    return n;
}
