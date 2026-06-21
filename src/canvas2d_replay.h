#pragma once

// In-memory core of canvas2d_replay_from(): parse the text canvas-program in
// data[0,len) and replay it onto `cv` via the public canvas2d_* API.  The buffer
// need not be NUL-terminated -- parsing is by index against `len`.  Strict: any
// unknown command, bad argument, or over-long line stops and returns false
// (commands before the faulty line have already been applied).  Exposed (beyond
// canvas2d_replay_from, which just reads a file into a buffer and calls this) for
// tests and fuzzing.

#include "canvas2d.h"

#include <ptrcheck.h>
#include <stddef.h>

bool canvas2d_replay_text(struct canvas2d_context *__single cv, char const *__counted_by(len) data, size_t len);

// Adopt a malloc'd RGBA8 buffer rebuilt from a replayed `image` block: the
// canvas owns it until canvas2d_free (implemented in canvas.c).  The canvas
// -- not the parser -- must own these because set_fill_pattern/
// set_stroke_pattern BORROW their source, so a pattern set by a replayed
// program has to stay valid after replay returns (and across reset(), which
// restores drawing state but does not invalidate the program's blocks).
// Returns false, adopting nothing, when the bookkeeping node cannot be
// allocated; the caller still owns (and frees) the buffer.
bool canvas2d_canvas_own_image(struct canvas2d_context *__single cv,
                           uint8_t *__counted_by(len) px, int len);

// Reconfigure the canvas's immutable working space (canvas.h) from a replayed
// `working_space` line (implemented in canvas.c).  The space is normally fixed
// at construction; replay is the one caller allowed to set it, and only on the
// leading line of a program, when the canvas is still all-zero transparent --
// identical in either space, so this is exactly choosing the space at creation.
// Returns true (the value the parser propagates); never fails.
bool canvas2d_canvas_set_working_space(struct canvas2d_context *__single cv,
                                   enum canvas2d_color_space space);

// Draw one replayed image block (implemented in canvas.c, the replay-side
// twin of the draw trios): ct/at/cs are the block's format as named on its
// line (cs the colour-space tag -- honoured on draw: the resolved sample
// converts to the working space on deposit), `mips` whether the block's draws carry mip-chain
// semantics (an `image_mips` line) -- the chain rebuilds per draw here,
// byte-identical to a live cached chain.  `form` is the op spelling this draw
// replays (0 = draw_image, 1 = draw_image_scaled, 2 = draw_image_subrect), so
// replaying onto a recording canvas re-records the file byte-for-byte (the
// round-trip test's idempotence, the cs tag carried through).
void canvas2d_canvas_draw_block(struct canvas2d_context *__single cv,
                            uint8_t const *__counted_by(slen) px, int slen,
                            int w, int h, enum canvas2d_color_type ct,
                            enum canvas2d_alpha_type at, enum canvas2d_color_space cs,
                            bool mips, int form,
                            float sx, float sy, float sww, float shh,
                            float dx, float dy, float dw, float dh);
