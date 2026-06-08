#pragma once

// Memory helpers shared by the core.
//
// -fbounds-safety places strict rules on __counted_by pointer/count pairs:
// a mutable counted *local* is rejected, and any change to a count must be
// paired with a change to its pointer.  The practical consequences, used
// throughout this codebase, are:
//
//   * Growable arrays live as a (pointer, count) pair inside a struct, and the
//     two fields are assigned together after a realloc.
//   * Capacity arithmetic is done on a plain int with no pointer attached, so
//     it composes freely; that is what this header provides.

// New capacity that is at least `need`, grown by doubling from `cap`.
static inline int cnvs_grow_cap(int cap, int need) {
    int n = cap > 0 ? cap : 8;
    while (n < need) {
        n *= 2;
    }
    return n;
}
