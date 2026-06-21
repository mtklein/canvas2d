// canvas2d_premultiply / canvas2d_unpremultiply carry EXTENDED-RANGE colour:
// the colour planes are unbounded -- above the [0,1] gamut (HDR, brighter than
// white) and below it (wide gamut, e.g. a Rec.2020 primary's negative sRGB
// component) -- while alpha alone clamps to [0,1].  The range collapses only at
// the output encode+quantize, never in these converters (the same doctrine as
// canvas2d_px8_clamp_premul_lin; docs/decisions/color-axis.md).
#include "canvas2d_color.h"
#include "test_util.h"

#include <math.h>

static bool near(float x, float y) { return fabsf(x - y) < 1e-3f; }  // _Float16 tol

int main(void) {
    // In-gamut: the [0,1] case the old clamp never touched -- round-trips clean.
    {
        canvas2d_premul   const p = canvas2d_premultiply(canvas2d_unpremul_of(0.8f, 0.35f, 0.1f, 0.6f));
        CHECK(near((float)p.r, 0.8f * 0.6f));
        canvas2d_unpremul const u = canvas2d_unpremultiply(p);
        CHECK(near((float)u.r, 0.8f) && near((float)u.g, 0.35f) && near((float)u.b, 0.1f));
        CHECK(near((float)u.a, 0.6f));
    }
    // HDR: a colour brighter than white survives both directions (r*a > 1 and
    // r/a > 1 kept, not clamped to 1).
    {
        canvas2d_premul   const p = canvas2d_premultiply(canvas2d_unpremul_of(4.0f, 0.0f, 0.0f, 0.5f));
        CHECK(near((float)p.r, 2.0f));
        canvas2d_unpremul const u = canvas2d_unpremultiply(p);
        CHECK(near((float)u.r, 4.0f));
    }
    // Wide gamut: a negative component survives (not clamped to 0).
    {
        canvas2d_premul const p = canvas2d_premultiply(canvas2d_unpremul_of(-0.5f, 1.0f, 1.0f, 1.0f));
        CHECK(near((float)p.r, -0.5f));
    }
    // Alpha stays bounded to [0,1] even where colour does not.
    {
        canvas2d_premul const p = canvas2d_premultiply(canvas2d_unpremul_of(2.0f, 0.0f, 0.0f, 2.0f));
        CHECK(near((float)p.a, 1.0f));
    }
    // Fully transparent un-premultiplies to all zero (the a <= 0 guard).
    {
        canvas2d_unpremul const u =
            canvas2d_unpremultiply((canvas2d_premul){ .r = 0, .g = 0, .b = 0, .a = 0 });
        CHECK((float)u.r == 0.0f && (float)u.a == 0.0f);
    }
    return TEST_REPORT();
}
