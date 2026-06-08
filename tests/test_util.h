#pragma once

#include <stdio.h>

// Minimal test harness: CHECK records failures, TEST_REPORT() returns a process
// exit code (0 = all passed).  Tests are built with the same strict flags as
// the library, so this header must itself be -fbounds-safety/-Weverything clean.

static int g_test_fails = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                          #cond);                                           \
            g_test_fails += 1;                                              \
        }                                                                   \
    } while (0)

#define TEST_REPORT()                                                       \
    (g_test_fails == 0                                                      \
         ? ((void)printf("ok: %s\n", __FILE__), 0)                          \
         : ((void)fprintf(stderr, "%d failure(s): %s\n", g_test_fails,      \
                          __FILE__),                                        \
            1))
