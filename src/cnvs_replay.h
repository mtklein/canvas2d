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

bool cnvs_replay_text(struct canvas *__single cv, char const *__counted_by(len) data, size_t len);

// Adopt a malloc'd RGBA8 buffer rebuilt from a replayed `image` block: the
// canvas owns it until canvas_free (implemented in canvas.c).  The canvas
// -- not the parser -- must own these because set_fill_pattern/
// set_stroke_pattern BORROW their source, so a pattern set by a replayed
// program has to stay valid after replay returns (and across reset(), which
// restores drawing state but does not invalidate the program's blocks).
// Returns false, adopting nothing, when the bookkeeping node cannot be
// allocated; the caller still owns (and frees) the buffer.
bool cnvs_canvas_own_image(struct canvas *__single cv,
                           uint8_t *__counted_by(len) px, int len);

// Draw one replayed image block (implemented in canvas.c, the replay-side
// twin of the draw trios): `premul` says how to read the bytes (a `pimage`
// block -- premultiplied, e.g. a recorded snapshot), `mips` whether the
// block's draws carry mip-chain semantics (an `image_mips` line) -- the
// chain rebuilds per draw here, byte-identical to a live cached chain.
// `form` is the op spelling this draw replays (0 = draw_image, 1 =
// draw_image_scaled, 2 = draw_image_subrect), so replaying onto a recording
// canvas re-records the file byte-for-byte (the round-trip test's
// idempotence).
void cnvs_canvas_draw_block(struct canvas *__single cv,
                            uint8_t const *__counted_by(slen) px, int slen,
                            int w, int h, bool premul, bool mips, int form,
                            float sx, float sy, float sww, float shh,
                            float dx, float dy, float dw, float dh);
