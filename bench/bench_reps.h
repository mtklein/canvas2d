#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// Profiling knob: repeat a benchmark's timed work BENCH_REPS times (default 1, so
// `ninja benchcmp` timings are unchanged).  `ninja profile` raises it so each bench
// runs long enough for `sample` to collect a meaningful self-time profile.
static inline int bench_reps(void) {
    char const *__null_terminated env = getenv("BENCH_REPS");
    int const reps = env ? atoi(env) : 1;
    return reps < 1 ? 1 : reps;
}

// Monotonic wall-clock seconds, for self-timed throughput.
static inline double bench_now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1.0e-9;
}

// Print a size-normalised throughput when BENCH_THROUGHPUT is set: Mpx/s and ns/px
// over `work_px` total pixels processed in `seconds`.  Off by default, so `benchcmp`
// output is unchanged; `ninja throughput` sets it (with a high BENCH_REPS so the cold
// first rep is amortised away).  The metric is comparable across canvas sizes --
// unlike a raw wall-clock that scales with the scene -- so it's the apples-to-apples
// "how many pixels per second" number.
static inline void bench_report_throughput(double seconds, double work_px) {
    if (getenv("BENCH_THROUGHPUT") && seconds > 0.0 && work_px > 0.0) {
        fprintf(stderr, "throughput: %.1f Mpx/s (%.3f ns/px) over %.0f px in %.1f ms\n",
                work_px / seconds / 1.0e6, seconds * 1.0e9 / work_px,
                work_px, seconds * 1.0e3);
    }
}
