#include "canvas2d_color.h"
#include "test_util.h"

#include <math.h>

static bool near(float x, float y, float tol) {
    return fabsf(x - y) <= tol;
}

static bool finite3(canvas2d_rgb c) {
    return isfinite(c.r) && isfinite(c.g) && isfinite(c.b);
}

static bool finite_lab(canvas2d_oklab c) {
    return isfinite(c.L) && isfinite(c.a) && isfinite(c.b);
}

int main(void) {
    // --- sRGB 8-bit round trip: decode then re-encode every code, exact -------
    // Each of the 256 codes decodes to linear and re-encodes; quantizing back to
    // 8 bits must return the same code (the spec's <= 1/255 promise; in practice
    // exact for all 256).
    for (int code = 0; code < 256; code++) {
        float const c = (float)code / 255.0f;
        float const back = canvas2d_linear_to_srgb(canvas2d_srgb_to_linear(c));
        int const requant = (int)lrintf(back * 255.0f);
        CHECK(requant == code);
        CHECK(near(back, c, 1.0f / 255.0f));
    }

    // A linear value round-trips encode -> decode to tight f32 tolerance.
    for (int i = 0; i <= 100; i++) {
        float const l = (float)i / 100.0f;
        CHECK(near(canvas2d_srgb_to_linear(canvas2d_linear_to_srgb(l)), l, 1e-5f));
    }

    // --- totality: out-of-[0,1] inputs stay finite (the cbrtf/odd-extension
    // guarantee, where powf(x,1/3) or powf(neg,...) would NaN) ----------------
    float const spread[] = { -2.0f, -1.0f, -0.5f, -0.04f, -1e-6f, 0.0f,
                             1e-6f, 0.5f, 1.0f, 1.5f, 2.0f, 4.0f };
    int const N = (int)(sizeof spread / sizeof spread[0]);
    for (int i = 0; i < N; i++) {
        CHECK(isfinite(canvas2d_srgb_to_linear(spread[i])));
        CHECK(isfinite(canvas2d_linear_to_srgb(spread[i])));
    }
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            for (int k = 0; k < N; k++) {
                canvas2d_rgb const lin = { spread[i], spread[j], spread[k] };
                canvas2d_oklab const lab = canvas2d_linear_srgb_to_oklab(lin);
                CHECK(finite_lab(lab));
                CHECK(finite3(canvas2d_oklab_to_linear_srgb(lab)));
            }
        }
    }

    // --- sign symmetry of the extended transfer: f(-x) == -f(x) ---------------
    for (int i = 0; i < N; i++) {
        float const x = spread[i];
        CHECK(near(canvas2d_srgb_to_linear(-x), -canvas2d_srgb_to_linear(x), 1e-7f));
        CHECK(near(canvas2d_linear_to_srgb(-x), -canvas2d_linear_to_srgb(x), 1e-7f));
    }

    // --- Oklab known values (Ottosson's reference numbers) --------------------
    canvas2d_oklab const white = canvas2d_linear_srgb_to_oklab((canvas2d_rgb){ 1.0f, 1.0f, 1.0f });
    CHECK(near(white.L, 1.0f, 1e-4f));
    CHECK(near(white.a, 0.0f, 1e-4f));
    CHECK(near(white.b, 0.0f, 1e-4f));

    canvas2d_oklab const black = canvas2d_linear_srgb_to_oklab((canvas2d_rgb){ 0.0f, 0.0f, 0.0f });
    CHECK(near(black.L, 0.0f, 1e-6f));
    CHECK(near(black.a, 0.0f, 1e-6f));
    CHECK(near(black.b, 0.0f, 1e-6f));

    // Saturated: linear red (1,0,0) -> L=0.627955, a=0.224863, b=0.125846.
    canvas2d_oklab const red = canvas2d_linear_srgb_to_oklab((canvas2d_rgb){ 1.0f, 0.0f, 0.0f });
    CHECK(near(red.L, 0.627955f, 1e-4f));
    CHECK(near(red.a, 0.224863f, 1e-4f));
    CHECK(near(red.b, 0.125846f, 1e-4f));

    // --- Oklab round trip over a grid of in- and out-of-gamut colour ----------
    float const grid[] = { -0.3f, 0.0f, 0.2f, 0.5f, 0.8f, 1.0f, 1.4f };
    int const M = (int)(sizeof grid / sizeof grid[0]);
    for (int i = 0; i < M; i++) {
        for (int j = 0; j < M; j++) {
            for (int k = 0; k < M; k++) {
                canvas2d_rgb const lin = { grid[i], grid[j], grid[k] };
                canvas2d_rgb const rt = canvas2d_oklab_to_linear_srgb(
                                        canvas2d_linear_srgb_to_oklab(lin));
                CHECK(near(rt.r, lin.r, 1e-4f));
                CHECK(near(rt.g, lin.g, 1e-4f));
                CHECK(near(rt.b, lin.b, 1e-4f));
            }
        }
    }

    // The triple wrappers agree with the scalar transfer, lane for lane.
    canvas2d_rgb const enc = { 0.1f, 0.5f, 0.9f };
    canvas2d_rgb const dec = canvas2d_rgb_srgb_to_linear(enc);
    CHECK(near(dec.r, canvas2d_srgb_to_linear(enc.r), 0.0f));
    CHECK(near(dec.g, canvas2d_srgb_to_linear(enc.g), 0.0f));
    CHECK(near(dec.b, canvas2d_srgb_to_linear(enc.b), 0.0f));
    canvas2d_rgb const reenc = canvas2d_rgb_linear_to_srgb(dec);
    CHECK(near(reenc.r, enc.r, 1e-5f));
    CHECK(near(reenc.g, enc.g, 1e-5f));
    CHECK(near(reenc.b, enc.b, 1e-5f));

    return TEST_REPORT();
}
