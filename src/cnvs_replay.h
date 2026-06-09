#pragma once

// In-memory core of canvas_replay_from(): parse the text canvas-program in
// data[0,len) and replay it onto `cv` via the public canvas_* API.  The buffer
// need not be NUL-terminated -- parsing is by index against `len`.  Strict: any
// unknown command, bad argument, or over-long line stops and returns false
// (commands before the faulty line have already been applied).  Exposed (beyond
// canvas_replay_from, which just reads a file into a buffer and calls this) for
// tests and fuzzing.

#include "canvas.h"

#include <ptrcheck.h>
#include <stddef.h>

bool cnvs_replay_text(canvas *__single cv, char const *__counted_by(len) data, size_t len);
