#include "cnvs_mem.h"

int cnvs_grow_cap(int cap, int need) {
    int n = cap > 0 ? cap : 8;
    while (n < need) {
        n *= 2;
    }
    return n;
}
