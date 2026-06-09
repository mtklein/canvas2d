#include "ring.h"

// Masked index: provably <= cap-1, but the flag doesn't fold it, so each load is
// bounds-checked.
long ring_sum_masked(int32_t const *__counted_by(cap) buf, int cap, int tail, int n) {
    if (cap <= 0 || tail < 0 || tail >= cap || n < 0 || n > cap) {
        return 0;
    }
    long s = 0;
    int m = cap - 1;
    for (int i = 0; i < n; i++) {
        s += buf[(tail + i) & m];
    }
    return s;
}

// Two contiguous runs (tail..cap, then 0..), each bounded by `j < cap` so the loads
// are loop-provably in range and the checks elide.
long ring_sum_runs(int32_t const *__counted_by(cap) buf, int cap, int tail, int n) {
    if (cap <= 0 || tail < 0 || tail >= cap || n < 0 || n > cap) {
        return 0;
    }
    long s = 0;
    int first = cap - tail;        // contiguous run length from tail to end
    if (first > n) {
        first = n;
    }
    for (int i = 0; i < first; i++) {
        s += buf[tail + i];        // tail+i in [tail, cap)
    }
    for (int i = 0; i < n - first; i++) {
        s += buf[i];               // i in [0, n-first) <= [0, cap)
    }
    return s;
}
