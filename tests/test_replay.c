// cnvs_replay_text: a valid program replays and draws; malformed input is
// rejected (returns false) without crashing.  Under the debug variant this runs
// with ASan+UBSan and -fbounds-safety, so a parser memory bug on adversarial
// input would trap here.  See secreview/SECURITY-REVIEW.md.

#include "test_util.h"

#include "canvas.h"
#include "cnvs_replay.h"

#include <ptrcheck.h>
#include <stdint.h>
#include <string.h>

// sizeof-1 keeps the literal an array (bounds known), avoiding the
// __null_terminated->__counted_by seam at the call.
#define REPLAY(cv, s) cnvs_replay_text((cv), (s), sizeof(s) - 1)

int main(void) {
    canvas *__single cv = canvas_create(64, 48);
    CHECK(cv != NULL);

    // A valid program: comments, blank lines, no trailing newline on the last line.
    CHECK(REPLAY(cv,
        "# fill the canvas grey\n"
        "set_fill_rgba 0.5 0.5 0.5 1\n"
        "\n"
        "fill_rect 0 0 64 48\n"
        "set_line_join round\n"
        "set_line_dash 4 2\n"
        "begin_path\n"
        "move_to 4 4\n"
        "bezier_curve_to 20 0 40 48 60 24\n"
        "stroke\n"
        "set_global_composite_operation multiply\n"
        "arc 32 24 10 0 6.2831853 1\n"
        "fill_text 2 40 hi"));

    // It actually drew: the grey fill is visible.
    uint8_t px[64 * 48 * 4];
    canvas_get_image_data(cv, 0, 0, 64, 48, px, (int)sizeof px);
    CHECK(px[0] == 128 || px[0] == 127);

    // Malformed inputs are all rejected (and, crucially, don't crash).
    CHECK(!REPLAY(cv, "bogus_command 1 2\n"));        // unknown command
    CHECK(!REPLAY(cv, "move_to 1\n"));                // too few args
    CHECK(!REPLAY(cv, "move_to 1 2 3\n"));            // too many args
    CHECK(!REPLAY(cv, "set_fill_rgba 0.1 abc 0.2 1\n")); // non-numeric arg
    CHECK(!REPLAY(cv, "set_line_join wiggle\n"));     // bad enum
    CHECK(!REPLAY(cv, "arc 1 2 3 4 5 maybe\n"));      // bad bool
    CHECK(!REPLAY(cv, "set_global_composite_operation nope\n"));

    // Numeric edge cases: a huge exponent is a valid number that saturates to
    // inf/0 (like strtof) -- it must parse without tripping signed-overflow UB in
    // the exponent accumulator.  Under the debug variant (UBSan), the pre-clamp
    // code trapped on these; normal exponents still parse.
    CHECK(REPLAY(cv, "move_to 1e1 2e0\n"));               // ordinary exponents -> (10, 2)
    CHECK(REPLAY(cv, "move_to 0 1e99999999999\n"));       // saturates to +inf, no overflow
    CHECK(REPLAY(cv, "line_to 0 1e-99999999999\n"));      // saturates to 0, no overflow
    CHECK(REPLAY(cv, "set_global_alpha 1e2000000000\n")); // huge exp into a clamped setter

    // Empty / comment-only / whitespace programs are valid no-ops.
    CHECK(REPLAY(cv, ""));
    CHECK(REPLAY(cv, "# just a comment\n   \n\t\n"));

    // An over-long line is rejected, not over-read: one 5000-byte token.
    {
        char big[5000];
        memset(big, '1', sizeof big);
        CHECK(!cnvs_replay_text(cv, big, sizeof big));
    }

    // Adversarial bytes (no newline, high bytes, lone multibyte leads) must not
    // crash; result is don't-care.
    {
        char junk[] = { (char)0xF0, (char)0x80, 'm', 'o', 'v', 'e', '_', 't', 'o',
                        ' ', '1', (char)0xFF, 0x7F };
        (void)cnvs_replay_text(cv, junk, sizeof junk);
        CHECK(cv != NULL);
    }

    canvas_destroy(cv);
    return TEST_REPORT();
}
