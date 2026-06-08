#pragma once

// Capacity math kept free of any pointer: under -fbounds-safety a __counted_by
// count can't be a mutable local, so growable arrays compute capacity here and
// assign it back to a struct's count field alongside its data pointer.
static inline int cnvs_grow_cap(int cap, int need) {
    int n = cap > 0 ? cap : 8;
    while (n < need) {
        n *= 2;
    }
    return n;
}
