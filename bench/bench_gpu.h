#pragma once

#include "canvas.h"

#include <stdio.h>

// Print the backend's coarse GPU profile (total GPU time, dispatch count, and
// us/dispatch) to stderr when CANVAS_GPU_TIMING is set -- the Metal backend's
// programmatic complement to `sample`, which is CPU-only and GPU-blind.  A no-op
// when timing is off or the canvas ran on the CPU backend (which reports 0/0).
// Call after the final readback so all GPU work has completed and been counted.
static inline void bench_report_gpu_timing(canvas *__single cv) {
    double gpu_ns = 0.0;
    long dispatches = 0;
    canvas_gpu_timing(cv, &gpu_ns, &dispatches);
    if (dispatches > 0) {
        fprintf(stderr, "gpu: %.2f ms total over %ld dispatches, %.1f us/dispatch\n",
                gpu_ns / 1.0e6, dispatches, gpu_ns / 1.0e3 / (double)dispatches);
    }
}
