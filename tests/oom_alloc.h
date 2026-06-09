#pragma once

// Fault-injecting allocator for the out-of-memory test build.  The core sources
// are compiled with -Dmalloc=cnvs_oom_malloc (and realloc/calloc), so every
// allocation routes here; this file is compiled WITHOUT those defines so it can
// reach the real allocator.  The alloc_size attributes mirror the libc ones so
// -fbounds-safety still tracks allocation sizes for __counted_by pointers.

#include <stddef.h>

void *cnvs_oom_malloc(size_t size) __attribute__((alloc_size(1)));
void *cnvs_oom_realloc(void *p, size_t size) __attribute__((alloc_size(2)));
void *cnvs_oom_calloc(size_t n, size_t size) __attribute__((alloc_size(1, 2)));

// Arm the injector: the k-th allocation (1-indexed) after this call returns NULL;
// all others succeed.  k <= 0 disarms.  Resets the internal counter each call.
void cnvs_oom_fail_at(int k);

// Allocations seen since the last cnvs_oom_fail_at (for assertions/debugging).
int cnvs_oom_seen(void);
