#include "ring.h"
#include "test_util.h"

#include <stdlib.h>

// Independent reference: sum n elements from tail, one wrap (n <= cap).
static long naive(int32_t const *__counted_by(cap) buf, int cap, int tail, int n) {
    long s = 0;
    for (int i = 0; i < n; i++) {
        int idx = tail + i;
        if (idx >= cap) { idx -= cap; }
        s += buf[idx];
    }
    return s;
}

int main(void) {
    int const cap = 16;
    int32_t *__counted_by(cap) buf = malloc((size_t)cap * sizeof(int32_t));
    CHECK(buf != NULL);
    if (buf) {
        for (int i = 0; i < cap; i++) {
            buf[i] = i * 7 + 1;
        }
        int tails[] = { 0, 1, 9, 15 };
        int ns[] = { 0, 1, 7, 16 };
        bool ok = true;
        for (int ti = 0; ti < 4; ti++) {
            for (int ni = 0; ni < 4; ni++) {
                int t = tails[ti], n = ns[ni];
                long want = naive(buf, cap, t, n);
                if (ring_sum_masked(buf, cap, t, n) != want) { ok = false; }
                if (ring_sum_runs(buf, cap, t, n) != want) { ok = false; }
            }
        }
        CHECK(ok);
    }
    free(buf);
    return TEST_REPORT();
}
