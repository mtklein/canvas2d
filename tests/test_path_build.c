// Unit tests for struct canvas2d_path's state machine and curve flattening.  test_path.c
// drives the path through the canvas API, which always establishes a current
// point first and flattens at a fixed tolerance -- so the "no current point"
// branches (line_to/quad_to/cubic_to before any move_to), close-on-empty, and the
// flatten flat/recurse/depth-cap branches go untaken (struct canvas2d_path was 89% lines but
// 54% branches).  Drive struct canvas2d_path directly to reach them.  Includes the internal
// header, like test_mem.c.  (The remaining uncovered branches are the realloc
// failure guards in move_to/rect/*_reserve -- unreachable without malloc fault
// injection, so not chased here.)

#include "test_util.h"

#include "canvas2d_path.h"

#include <math.h>

static canvas2d_vec2 v(float x, float y) {
    return (canvas2d_vec2){ .x = x, .y = y };
}

int main(void) {
    struct canvas2d_path p;
    canvas2d_path_init(&p);

    // line_to with no current point starts the subpath (the !has_cur branch).
    CHECK(canvas2d_path_line_to(&p, v(1.0f, 2.0f)));
    CHECK(p.has_cur && p.npts == 1 && p.nsubs == 1);

    // close on an empty path is a no-op; close on a real subpath marks it closed
    // and clears the current point, so the next line_to opens a fresh subpath.
    canvas2d_path_reset(&p);
    CHECK(canvas2d_path_close(&p) && p.nsubs == 0);
    canvas2d_path_move_to(&p, v(0.0f, 0.0f));
    canvas2d_path_line_to(&p, v(4.0f, 0.0f));
    CHECK(canvas2d_path_close(&p));
    CHECK(p.subs[0].closed && !p.has_cur);
    CHECK(canvas2d_path_line_to(&p, v(8.0f, 8.0f)) && p.nsubs == 2);

    // quad_to / cubic_to with no current point move_to the first control point.
    canvas2d_path_reset(&p);
    CHECK(canvas2d_path_quad_to(&p, v(2.0f, 2.0f), v(4.0f, 0.0f), 0.25f) && p.has_cur);
    canvas2d_path_reset(&p);
    CHECK(canvas2d_path_cubic_to(&p, v(1.0f, 3.0f), v(3.0f, 3.0f), v(4.0f, 0.0f), 0.25f) && p.has_cur);

    // Flattening, cubic: a collinear curve is flat at any tolerance -> one
    // segment (the flat branch); a curved one at tol 0 never meets the chord test,
    // so it subdivides to the depth cap (the depth branch) and still terminates.
    canvas2d_path_reset(&p);
    canvas2d_path_move_to(&p, v(0.0f, 0.0f));
    int n0 = p.npts;
    CHECK(canvas2d_path_cubic_to(&p, v(10.0f, 0.0f), v(20.0f, 0.0f), v(30.0f, 0.0f), 1.0f));
    int flat = p.npts - n0;
    CHECK(flat == 1);

    canvas2d_path_reset(&p);
    canvas2d_path_move_to(&p, v(0.0f, 0.0f));
    n0 = p.npts;
    CHECK(canvas2d_path_cubic_to(&p, v(0.0f, 40.0f), v(40.0f, 40.0f), v(40.0f, 0.0f), 0.0f));
    int const deep = p.npts - n0;
    CHECK(deep > flat && deep >= 100);  // subdivided deeply, but bounded + terminated

    // Flattening, quad: same flat-vs-deep pair.
    canvas2d_path_reset(&p);
    canvas2d_path_move_to(&p, v(0.0f, 0.0f));
    n0 = p.npts;
    CHECK(canvas2d_path_quad_to(&p, v(15.0f, 0.0f), v(30.0f, 0.0f), 1.0f));
    CHECK(p.npts - n0 == 1);
    canvas2d_path_reset(&p);
    canvas2d_path_move_to(&p, v(0.0f, 0.0f));
    n0 = p.npts;
    CHECK(canvas2d_path_quad_to(&p, v(0.0f, 40.0f), v(40.0f, 0.0f), 0.0f));
    CHECK(p.npts - n0 >= 50);

    // Flattening, NON-FINITE: an inf/NaN control point makes every flatness
    // comparison false, so the test is written !(error > tol) and the curve
    // counts as flat -- ONE segment, not 2^depth-cap.  Regression for a
    // fuzz_replay OOM: `fill_text 1e9999 0 <text>` put an inf pen under every
    // glyph point, and each poisoned curve emitted 65K-262K points -- a long
    // line of text multiplied that into a multi-GB path.
    float const inf = HUGE_VALF;
    canvas2d_path_reset(&p);
    canvas2d_path_move_to(&p, v(inf, 0.0f));
    n0 = p.npts;
    CHECK(canvas2d_path_quad_to(&p, v(inf, 40.0f), v(inf, 0.0f), 0.0f));
    CHECK(p.npts - n0 == 1);
    canvas2d_path_reset(&p);
    canvas2d_path_move_to(&p, v(0.0f, 0.0f));
    n0 = p.npts;
    CHECK(canvas2d_path_cubic_to(&p, v(0.0f, inf), v(NAN, 40.0f), v(40.0f, 0.0f),
                             0.0f));
    CHECK(p.npts - n0 == 1);

    // rect builds one closed 4-point subpath.
    canvas2d_path_reset(&p);
    CHECK(canvas2d_path_rect(&p, v(0.0f, 0.0f), v(4.0f, 0.0f), v(4.0f, 4.0f), v(0.0f, 4.0f)));
    CHECK(p.nsubs == 1 && p.subs[0].closed && p.subs[0].count == 4);

    canvas2d_path_free(&p);
    return TEST_REPORT();
}
