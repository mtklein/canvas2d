#pragma once

#include <stdlib.h>

// Profiling knob: repeat a benchmark's timed work BENCH_REPS times (default 1, so
// `ninja benchcmp` timings are unchanged).  `ninja profile` raises it so each bench
// runs long enough for `sample` to collect a meaningful self-time profile.
static inline int bench_reps(void) {
    char const *__null_terminated env = getenv("BENCH_REPS");
    int reps = env ? atoi(env) : 1;
    return reps < 1 ? 1 : reps;
}
