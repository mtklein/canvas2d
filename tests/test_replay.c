// cnvs_replay_text: a valid program replays and draws; malformed input is
// rejected (returns false) without crashing.  Under the debug variant this runs
// with ASan+UBSan and -fbounds-safety, so a parser memory bug on adversarial
// input would trap here.  See docs/decisions/security-review.md.

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
    struct canvas *__single cv = canvas(64, 48);
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
    canvas_get_image_data(cv, CANVAS_CS_SRGB, 0, 0, 64, 48, px, (int)sizeof px);
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

    // Image blocks: a valid 1x1 block (a 12-byte zlib stream of the 4 RGBA
    // bytes) feeds every op form that references it by id.
    CHECK(REPLAY(cv,
        "image 0 unorm8 unpremul 1 1 12 1\n"
        "bits eJz7z8DwHwAE/wH/\n"
        "draw_image 0 4 4\n"
        "draw_image_scaled 0 8 4 6 6\n"
        "draw_image_subrect 0 0 0 1 1 16 4 6 6\n"
        "put_image_data 0 24 4\n"
        "put_image_data_dirty 0 30 4 0 0 1 1\n"
        "put_image_data 0 -1 -1\n"          // negative placement clips fine
        "set_fill_pattern 0 repeat\n"
        "fill_rect 4 12 8 8\n"
        "set_stroke_pattern 0 no-repeat\n"));
    // The 1x1 red source landed via put_image_data.
    canvas_get_image_data(cv, CANVAS_CS_SRGB, 24, 4, 1, 1, px, 4);
    CHECK(px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255);

    // The scalar ops: conic gradients, per-corner radii, smoothing, the
    // filter list, reset, resize (back and forth, ending at 64x48), and the
    // maxWidth text pair.
    CHECK(REPLAY(cv,
        "set_fill_conic_gradient 0.5 32 24\n"
        "add_fill_color_stop 0 1 0 0 1\n"
        "add_fill_color_stop 1 0 0 1 1\n"
        "set_stroke_conic_gradient 0 32 24\n"
        "add_stroke_color_stop 0 1 1 0 1\n"
        "begin_path\n"
        "round_rect_radii 4 4 40 24 2 3 4 2 0 0 5 5\n"
        "fill nonzero\n"
        "set_image_smoothing_enabled 0\n"
        "set_image_smoothing_quality high\n"
        "set_image_smoothing_enabled true\n"
        "set_filter_none\n"
        "add_filter_brightness 1.25\n"
        "add_filter_contrast 1.5\n"
        "add_filter_grayscale 0.5\n"
        "add_filter_hue_rotate 0.5\n"
        "add_filter_invert 0.25\n"
        "add_filter_opacity 0.75\n"
        "add_filter_saturate 2\n"
        "add_filter_sepia 0.5\n"
        "add_filter_blur 1.5\n"
        "add_filter_drop_shadow 2 2 1 0.25 0.5 0.75 0.5\n"
        "fill_rect 8 8 16 16\n"
        "set_filter_none\n"
        "fill_text_max 2 40 30 squeeze me\n"
        "stroke_text_max 2 44 30 squeeze me\n"
        "resize 32 24\n"
        "fill_rect 0 0 32 24\n"
        "reset\n"
        "resize 64 48\n"));

    // Bad scalar-op arguments are rejected.
    CHECK(!REPLAY(cv, "resize 0 24\n"));                 // zero dimension
    CHECK(!REPLAY(cv, "resize 99999 24\n"));             // past the dim cap
    CHECK(!REPLAY(cv, "resize 32\n"));                   // too few args
    CHECK(!REPLAY(cv, "add_filter_drop_shadow 1 2 3\n"));// too few args
    CHECK(!REPLAY(cv, "set_image_smoothing_quality wiggle\n"));
    CHECK(!REPLAY(cv, "round_rect_radii 1 2 3 4 5\n"));  // too few radii
    CHECK(!REPLAY(cv, "reset 1\n"));                     // trailing junk

    // reset restarts the file-local font-id space along with the text cache
    // it clears: a re-declared id is legal only after one.
    CHECK(REPLAY(cv, "font 0 1.0625 0.25 NoSuchFamily\n"
                     "reset\n"
                     "font 0 1.0625 0.25 NoSuchFamily\n"));
    CHECK(!REPLAY(cv, "font 0 1.0625 0.25 NoSuchFamily\n"
                      "font 0 1.0625 0.25 NoSuchFamily\n"));

    // Path blocks: every command verb, then the three ops that reference the
    // path by id (an empty path is legal and paints nothing).
    CHECK(REPLAY(cv,
        "path 0 10\n"
        "m 4 4\n"
        "l 30 4\n"
        "q 34 12 30 20\n"
        "c 26 24 12 24 8 20\n"
        "t 4 20 4 12 4\n"
        "z\n"
        "a 17 12 5 0 3 1\n"
        "e 17 12 9 4 0.25 0 3 0\n"
        "r 6 6 6 6\n"
        "rr 22 6 8 8 2\n"
        "fill_path 0 evenodd\n"
        "stroke_path 0\n"
        "path 1 0\n"
        "fill_path 1 nonzero\n"
        "save\n"
        "clip_path 0 nonzero\n"
        "fill_rect 0 0 64 48\n"
        "restore\n"));

    // Malformed path blocks and references are all rejected.
    CHECK(!REPLAY(cv, "fill_path 0 nonzero\n"));         // undeclared id
    CHECK(!REPLAY(cv, "path 0 1\n"
                      "fill_rect 0 0 1 1\n"));           // not a path command
    CHECK(!REPLAY(cv, "path 0 1\n"
                      "m 1\n"));                         // short command args
    CHECK(!REPLAY(cv, "path 0 2\n"
                      "m 1 2\n"));                       // truncated block
    CHECK(!REPLAY(cv, "path 0 1\n"
                      "z 9\n"));                         // trailing junk
    CHECK(!REPLAY(cv, "path 0 0\n"
                      "path 0 0\n"));                    // id redeclared
    CHECK(!REPLAY(cv, "path 0 0\n"
                      "fill_path 0 sideways\n"));        // bad fill rule
    CHECK(!REPLAY(cv, "path 0 1\n"
                      "a 1 2 3 4 5 maybe\n"));           // bad winding bool

    // Malformed image blocks and references are all rejected.
    CHECK(!REPLAY(cv, "draw_image 7 0 0\n"));            // undeclared id
    CHECK(!REPLAY(cv, "image 0 unorm8 unpremul 0 1 12 1\n"));            // zero dimension
    CHECK(!REPLAY(cv, "image 0 rgba9 unpremul 1 1 12 1\n"));   // unknown colour type
    CHECK(!REPLAY(cv, "image 0 unorm8 sideways 1 1 12 1\n"));  // unknown alpha type
    CHECK(!REPLAY(cv, "image 0 unorm8 unpremul 1 1 0 1\n"));             // zero-length stream
    CHECK(!REPLAY(cv, "image 0 unorm8 unpremul 1 1 12 999\n"));          // nlines > ceil(zlen/3)
    CHECK(!REPLAY(cv, "image 0 unorm8 unpremul 1 1 12 1\n"
                      "fill_rect 0 0 1 1\n"));           // bits must follow
    CHECK(!REPLAY(cv, "image 0 unorm8 unpremul 1 1 12 1\n"
                      "bits eJz7z8DwHwAE/wH/\n"
                      "image 0 unorm8 unpremul 1 1 12 1\n"
                      "bits eJz7z8DwHwAE/wH/\n"));       // id redeclared
    CHECK(!REPLAY(cv, "image 0 unorm8 unpremul 1 1 4 1\n"
                      "bits AAAAAA==\n"));               // no zlib stream at all
    CHECK(!REPLAY(cv, "image 0 unorm8 unpremul 2 2 12 1\n"
                      "bits eJz7z8DwHwAE/wH/\n"));       // inflates short of w*h*4
    CHECK(!REPLAY(cv, "image 0 unorm8 unpremul 1 1 12 1\n"
                      "bits eJz7z8DwHwAE/wH/\n"
                      "put_image_data 0 1.5 0\n"));      // int arg takes no '.'
    CHECK(!REPLAY(cv, "image 0 unorm8 unpremul 1 1 12 1\n"
                      "bits eJz7z8DwHwAE/wH/\n"
                      "set_fill_pattern 0 sideways\n")); // bad repeat mode

    // Gradient-interpolation lines: both knobs read by name (all three colour
    // spaces are valid interp spaces; alpha is premul/unpremul).  A valid line
    // of each, then every malformed-token rejection.
    CHECK(REPLAY(cv, "set_fill_gradient_interpolation srgb premul\n"));
    CHECK(REPLAY(cv, "set_fill_gradient_interpolation linear unpremul\n"));
    CHECK(REPLAY(cv, "set_fill_gradient_interpolation oklab premul\n"));
    CHECK(REPLAY(cv, "set_stroke_gradient_interpolation oklab unpremul\n"));
    CHECK(!REPLAY(cv, "set_fill_gradient_interpolation gamma premul\n"));    // bad space
    CHECK(!REPLAY(cv, "set_fill_gradient_interpolation srgb sideways\n"));   // bad alpha
    CHECK(!REPLAY(cv, "set_fill_gradient_interpolation srgb\n"));            // missing alpha
    CHECK(!REPLAY(cv, "set_fill_gradient_interpolation srgb premul extra\n")); // trailing junk
    CHECK(!REPLAY(cv, "set_stroke_gradient_interpolation gamma premul\n"));  // bad space
    CHECK(!REPLAY(cv, "set_stroke_gradient_interpolation oklab nope\n"));    // bad alpha

    // working_space: a valid leading line, then every rejection.  It must lead
    // the file (reconfiguring the immutable space before the first op), name one
    // of the two COMPOSITING spaces (oklab is not a working space, only an
    // interpolation one), and carry exactly that one token.
    {
        struct canvas *__single ws = canvas(8, 8);
        CHECK(ws != NULL);
        CHECK(REPLAY(ws, "working_space linear\n"
                         "fill_rect 0 0 8 8\n"));
        canvas_free(ws);
    }
    CHECK(!REPLAY(cv, "fill_rect 0 0 1 1\n"
                      "working_space linear\n"));   // not leading
    CHECK(!REPLAY(cv, "working_space\n"));           // missing name
    CHECK(!REPLAY(cv, "working_space linear junk\n"));  // junk after name
    CHECK(!REPLAY(cv, "working_space gamma\n"));      // unknown name
    CHECK(!REPLAY(cv, "working_space oklab\n"));      // oklab is not a working space

    // An image block's optional trailing colour-space token: a valid tagged
    // block (linear), then an unknown-name rejection.
    CHECK(REPLAY(cv, "image 1 unorm8 unpremul 1 1 12 1 linear\n"
                     "bits eJz7z8DwHwAE/wH/\n"
                     "draw_image 1 0 0\n"));
    CHECK(!REPLAY(cv, "image 2 unorm8 unpremul 1 1 12 1 gamma\n"
                      "bits eJz7z8DwHwAE/wH/\n"));    // unknown trailing cs
    CHECK(!REPLAY(cv, "image 2 unorm8 unpremul 1 1 12 1 linear extra\n"
                      "bits eJz7z8DwHwAE/wH/\n"));    // junk after the cs token

    // image_mips references a declared id; an undeclared one is rejected.
    CHECK(REPLAY(cv, "image 3 unorm8 unpremul 1 1 12 1\n"
                     "bits eJz7z8DwHwAE/wH/\n"
                     "image_mips 3\n"
                     "draw_image_scaled 3 0 0 8 8\n"));
    CHECK(!REPLAY(cv, "image_mips 9\n"));             // undeclared id
    CHECK(!REPLAY(cv, "image 4 unorm8 unpremul 1 1 12 1\n"
                      "bits eJz7z8DwHwAE/wH/\n"
                      "image_mips 4 5\n"));           // trailing junk

    // The single-float add_filter_* ops (only add_filter_drop_shadow had
    // negatives): a non-numeric amount and a missing amount are rejected for
    // each.
    CHECK(!REPLAY(cv, "add_filter_blur abc\n"));
    CHECK(!REPLAY(cv, "add_filter_blur\n"));
    CHECK(!REPLAY(cv, "add_filter_brightness abc\n"));
    CHECK(!REPLAY(cv, "add_filter_brightness\n"));
    CHECK(!REPLAY(cv, "add_filter_contrast x\n"));
    CHECK(!REPLAY(cv, "add_filter_grayscale\n"));
    CHECK(!REPLAY(cv, "add_filter_hue_rotate q\n"));
    CHECK(!REPLAY(cv, "add_filter_invert\n"));
    CHECK(!REPLAY(cv, "add_filter_opacity zz\n"));
    CHECK(!REPLAY(cv, "add_filter_saturate\n"));
    CHECK(!REPLAY(cv, "add_filter_sepia nope\n"));
    CHECK(!REPLAY(cv, "add_filter_blur 1 2\n"));      // too many args
    CHECK(!REPLAY(cv, "add_filter_drop_shadow 1 2 3 4 5 6 7 gamma\n")); // bad trailing cs

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

    canvas_free(cv);
    return TEST_REPORT();
}
