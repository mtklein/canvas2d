#pragma once

// Capacity growth for the project's growable arrays.  Under -fbounds-safety a
// __counted_by count can't be a mutable local, so callers compute the next
// capacity here and store it back beside the data pointer.
int cnvs_grow_cap(int cap, int need);
