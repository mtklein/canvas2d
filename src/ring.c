#include "ring.h"

#include <string.h>

typedef int32_t i32x4 __attribute__((ext_vector_type(4)));

// Sum buf[start .. start+len), start+len <= cap.  The vector load is one bounds
// check per 4 elements (the whole-vector memcpy load), amortized.
static long sum_run(int32_t const *__counted_by(cap) buf, int cap, int start, int len) {
    (void)cap;
    i32x4 a0 = 0, a1 = 0, a2 = 0, a3 = 0;  // four accumulators to hide add latency
    int i = 0;
    for (; i + 16 <= len; i += 16) {
        i32x4 v[4];
        memcpy(v, buf + start + i, sizeof v);  // ONE bounds check per 16 elements
        a0 += v[0]; a1 += v[1]; a2 += v[2]; a3 += v[3];
    }
    i32x4 acc = (a0 + a1) + (a2 + a3);
    long s = (long)acc[0] + acc[1] + acc[2] + acc[3];
    for (; i + 4 <= len; i += 4) {
        i32x4 v;
        memcpy(&v, buf + start + i, sizeof v);
        s += (long)v[0] + v[1] + v[2] + v[3];
    }
    for (; i < len; i++) {
        s += buf[start + i];
    }
    return s;
}

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

long ring_sum_simd(int32_t const *__counted_by(cap) buf, int cap, int tail, int n) {
    if (cap <= 0 || tail < 0 || tail >= cap || n < 0 || n > cap) {
        return 0;
    }
    int first = cap - tail;
    if (first > n) {
        first = n;
    }
    return sum_run(buf, cap, tail, first) + sum_run(buf, cap, 0, n - first);
}
