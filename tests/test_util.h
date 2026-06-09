#pragma once

#include <stdio.h>

// TEST_REPORT() returns 0 if every CHECK passed, else 1 -- use as main's return.
// Success is silent on purpose: `ninja` builds and runs the whole suite, so a
// passing test must print nothing (only ninja's progress shows).  Failures go to
// stderr, which ninja surfaces along with the failed command.

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
         ? 0                                                                 \
         : ((void)fprintf(stderr, "%d failure(s): %s\n", g_test_fails,      \
                          __FILE__),                                        \
            1))
