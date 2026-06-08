// Demonstrates that -fbounds-safety catches an out-of-bounds write at runtime.
//
// A child process performs the violation; the parent asserts the child died
// from a signal (the trap), so the test itself exits 0 when bounds safety
// works.  This passes in BOTH the -Os and sanitizer builds: the annotation is
// enforced regardless of optimisation level.

#include "test_util.h"

#include <ptrcheck.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static void out_of_bounds_write(int idx) {
    const int n = 8;
    int *__counted_by(n) a = calloc((size_t)n, sizeof(int));
    if (!a) {
        _exit(2);
    }
    a[idx] = 123;                  // traps when idx >= n
    volatile int sink = a[idx];    // reached only if no trap fired
    (void)sink;
    free(a);
}

int main(void) {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        // Child: index is read through volatile so the compiler cannot prove
        // the access is out of bounds at compile time (which would be an error
        // rather than the runtime trap we want to exercise).
        volatile int v = 1000;
        out_of_bounds_write(v);
        _exit(0);  // no trap => bug
    }

    int status = 0;
    (void)waitpid(pid, &status, 0);
    bool trapped = WIFSIGNALED(status);
    CHECK(trapped);
    if (!trapped && WIFEXITED(status)) {
        (void)fprintf(stderr, "child exited %d without trapping\n",
                      WEXITSTATUS(status));
    }
    return TEST_REPORT();
}
