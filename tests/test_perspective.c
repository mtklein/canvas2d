// Projective (perspective) transforms (docs/decisions/perspective.md, P1):
// the cnvs_mat homography math, the quad->homography solve, and projective
// geometry (a fill lands where projection predicts; a fully-behind shape draws
// nothing; affine behaviour is unchanged; record->replay round-trips).

#include "cnvs_math.h"
#include "test_util.h"

#include "canvas.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool near(float x, float y) {
    return fabsf(x - y) < 1e-4f;
}

static bool vnear(cnvs_vec2 p, float x, float y) {
    return near(p.x, x) && near(p.y, y);
}

// Bit-identity of two floats (the affine fast-path proofs need exact, not near).
static bool feq_bits(float a, float b) {
    uint32_t ua, ub;
    memcpy(&ua, &a, sizeof ua);
    memcpy(&ub, &b, sizeof ub);
    return ua == ub;
}

// A representative projective matrix (g,h not both 0): maps (x,y,1) and divides
// by w = 0.1*x + 0.2*y + 1.
static cnvs_mat sample_perspective(void) {
    return (cnvs_mat){ .a = 2.0f, .b = 0.3f, .c = 0.5f, .d = 1.5f,
                       .e = 4.0f, .f = -1.0f, .g = 0.1f, .h = 0.2f, .i = 1.0f };
}

// --- cnvs_mat math ----------------------------------------------------------

static void test_is_affine_and_identity(void) {
    cnvs_mat const id = cnvs_mat_identity();
    CHECK(cnvs_mat_is_affine(id));
    CHECK(vnear(cnvs_mat_apply(id, (cnvs_vec2){ .x = 3.0f, .y = 4.0f }), 3.0f, 4.0f));

    CHECK(cnvs_mat_is_affine(cnvs_mat_translate(2.0f, 3.0f)));
    CHECK(cnvs_mat_is_affine(cnvs_mat_scale(2.0f, 3.0f)));
    CHECK(cnvs_mat_is_affine(cnvs_mat_rotate(0.5f)));

    CHECK(!cnvs_mat_is_affine(sample_perspective()));
}

// An affine apply must be bit-identical to the 2x3 expectation (no divide).
static void test_affine_apply_bit_identical(void) {
    cnvs_mat const m = { .a = 1.5f, .b = -0.25f, .c = 0.75f, .d = 2.0f,
                         .e = 10.0f, .f = -3.0f, .g = 0.0f, .h = 0.0f, .i = 1.0f };
    float const px = 2.0f, py = 5.0f;
    float const want_x = m.a * px + m.c * py + m.e;  // the exact 2x3 expression
    float const want_y = m.b * px + m.d * py + m.f;
    cnvs_vec2 const got = cnvs_mat_apply(m, (cnvs_vec2){ .x = px, .y = py });
    CHECK(feq_bits(got.x, want_x));  // bit-identical, not merely near
    CHECK(feq_bits(got.y, want_y));
}

// A perspective apply divides by w.
static void test_perspective_apply_divides(void) {
    cnvs_mat const m = sample_perspective();
    float const px = 3.0f, py = -2.0f;
    float const w = m.g * px + m.h * py + m.i;
    float const wx = (m.a * px + m.c * py + m.e) / w;
    float const wy = (m.b * px + m.d * py + m.f) / w;
    CHECK(vnear(cnvs_mat_apply(m, (cnvs_vec2){ .x = px, .y = py }), wx, wy));
}

// Affine mul/invert are bit-identical to the 2x3 result.
static void test_affine_mul_invert_bit_identical(void) {
    cnvs_mat const t = cnvs_mat_translate(10.0f, -5.0f);
    cnvs_mat const s = cnvs_mat_scale(2.0f, 3.0f);
    cnvs_mat const ts = cnvs_mat_mul(t, s);
    // The old 2x3 product expressions, term for term.
    CHECK(feq_bits(ts.a, t.a * s.a + t.c * s.b));
    CHECK(feq_bits(ts.c, t.a * s.c + t.c * s.d));
    CHECK(feq_bits(ts.e, t.a * s.e + t.c * s.f + t.e));
    CHECK(feq_bits(ts.b, t.b * s.a + t.d * s.b));
    CHECK(feq_bits(ts.d, t.b * s.c + t.d * s.d));
    CHECK(feq_bits(ts.f, t.b * s.e + t.d * s.f + t.f));
    CHECK(cnvs_mat_is_affine(ts));

    cnvs_mat const m = { .a = 1.5f, .b = -0.25f, .c = 0.75f, .d = 2.0f,
                         .e = 10.0f, .f = -3.0f, .g = 0.0f, .h = 0.0f, .i = 1.0f };
    cnvs_mat const inv = cnvs_mat_invert(m);
    float const det = m.a * m.d - m.b * m.c;
    float const di = 1.0f / det;
    CHECK(feq_bits(inv.a, m.d * di));
    CHECK(feq_bits(inv.c, -m.c * di));
    CHECK(feq_bits(inv.b, -m.b * di));
    CHECK(feq_bits(inv.d, m.a * di));
    CHECK(cnvs_mat_is_affine(inv));
}

// invert . apply round-trips a point for a perspective matrix.
static void test_perspective_invert_roundtrip(void) {
    cnvs_mat const m = sample_perspective();
    cnvs_mat const inv = cnvs_mat_invert(m);
    cnvs_vec2 const p = { .x = 7.0f, .y = 2.5f };
    cnvs_vec2 const back = cnvs_mat_apply(inv, cnvs_mat_apply(m, p));
    CHECK(vnear(back, p.x, p.y));
}

// --- set_perspective_quad ---------------------------------------------------

// set_perspective_quad maps the source rect's four corners to the destination
// points.  The CTM is set by recording the resolved 3x3 (set_perspective_quad
// emits a `set_transform <9>` line); parse that line to recover the full matrix,
// then apply it to each source corner and compare to the destination point.
static void test_quad_maps_corners(void) {
    float const sx = 0.0f, sy = 0.0f, sw = 4.0f, sh = 4.0f;
    float const dx0 = 50.0f, dy0 = 30.0f;   // TL
    float const dx1 = 150.0f, dy1 = 30.0f;  // TR
    float const dx2 = 190.0f, dy2 = 120.0f; // BR
    float const dx3 = 10.0f, dy3 = 120.0f;  // BL

    char const *__null_terminated path = "build/test_perspective_quad.canvas";
    struct canvas *__single cv = canvas(200, 150, CANVAS_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    CHECK(canvas_record_to(cv, path));
    canvas_set_perspective_quad(cv, sx, sy, sw, sh,
                                dx0, dy0, dx1, dy1, dx2, dy2, dx3, dy3);
    canvas_free(cv);  // flush + close

    // Read back the recorded set_transform line and parse its nine floats.
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    if (!f) {
        return;
    }
    cnvs_mat m = cnvs_mat_identity();
    bool found = false;
    char buf[256];
    for (;;) {
        // fgets returns a __null_terminated view of buf (or NULL at EOF), the form
        // sscanf's __null_terminated parameter wants -- no forge needed.
        char *__null_terminated line = fgets(buf, (int)sizeof buf, f);
        if (!line) {
            break;
        }
        float v[9];
        if (sscanf(line, "set_transform %f %f %f %f %f %f %f %f %f",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8]) == 9) {
            m = (cnvs_mat){ .a = v[0], .b = v[1], .c = v[2], .d = v[3], .e = v[4],
                            .f = v[5], .g = v[6], .h = v[7], .i = v[8] };
            found = true;
            break;
        }
    }
    (void)fclose(f);
    CHECK(found);
    CHECK(!cnvs_mat_is_affine(m));  // a real perspective solve

    // Each source corner maps to its destination point.
    CHECK(vnear(cnvs_mat_apply(m, (cnvs_vec2){ sx, sy }), dx0, dy0));            // TL
    CHECK(vnear(cnvs_mat_apply(m, (cnvs_vec2){ sx + sw, sy }), dx1, dy1));       // TR
    CHECK(vnear(cnvs_mat_apply(m, (cnvs_vec2){ sx + sw, sy + sh }), dx2, dy2));  // BR
    CHECK(vnear(cnvs_mat_apply(m, (cnvs_vec2){ sx, sy + sh }), dx3, dy3));       // BL
}

// Read one device pixel (RGBA, sRGB) from a canvas.
static void pixel_at(struct canvas *__single cv, int w, int h, int x, int y,
                     uint8_t out[4]) {
    int const len = w * h * 4;
    uint8_t *px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
    int const i = (y * w + x) * 4;
    out[0] = px[i + 0];
    out[1] = px[i + 1];
    out[2] = px[i + 2];
    out[3] = px[i + 3];
    free(px);
}

// A perspective fill lands where projection predicts: fill the whole source rect
// and probe a destination point that should be inside the projected quad, plus
// one well outside it.
static void test_fill_lands(void) {
    int const W = 200, H = 150;
    struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas_set_perspective_quad(cv, 0.0f, 0.0f, 4.0f, 4.0f,
                                50.0f, 30.0f, 150.0f, 30.0f,
                                190.0f, 120.0f, 10.0f, 120.0f);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, 4.0f, 4.0f);  // the full source rect

    // The quad's centroid-ish interior point is red.
    uint8_t p[4];
    pixel_at(cv, W, H, 100, 75, p);
    CHECK(p[0] > 200 && p[1] < 50 && p[2] < 50 && p[3] > 200);

    // A corner of the canvas, far outside the trapezoid, is untouched (alpha 0).
    pixel_at(cv, W, H, 2, 2, p);
    CHECK(p[3] == 0);
    canvas_free(cv);
}

// A fully-behind (w <= 0) shape draws nothing and does not crash: a CTM whose
// projection plane puts the whole source rect behind it.
static void test_fully_behind_draws_nothing(void) {
    int const W = 64, H = 64;
    struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    // w = g*x + h*y + i = -1*x + 0*y + (-1) = -(x+1) < 0 for x >= 0.
    canvas_set_transform_3x3(cv, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                             -1.0f, 0.0f, -1.0f);
    canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.0f, 1.0f, 0.0f, 1.0f);
    canvas_fill_rect(cv, 5.0f, 5.0f, 10.0f, 10.0f);  // all corners have w < 0
    canvas_begin_path(cv);
    canvas_move_to(cv, 5.0f, 5.0f);
    canvas_line_to(cv, 15.0f, 5.0f);
    canvas_line_to(cv, 15.0f, 15.0f);
    canvas_close_path(cv);
    canvas_fill(cv, CANVAS_NONZERO);
    canvas_set_line_width(cv, 2.0f);
    canvas_stroke(cv);

    // Nothing was painted.
    int const len = W * H * 4;
    uint8_t *px = malloc((size_t)len);
    CHECK(px != NULL);
    if (px) {
        canvas_read_rgba(cv, CANVAS_CS_SRGB, px, len);
        bool any = false;
        for (int i = 0; i < len; i++) {
            if (px[i] != 0) { any = true; break; }
        }
        CHECK(!any);
        free(px);
    }
    canvas_free(cv);
}

// Affine behaviour is unchanged: a transform set via set_transform_3x3 with
// (0,0,1) paints the same pixels as the affine set_transform.
static void test_set_transform_3x3_matches_affine(void) {
    int const W = 80, H = 80, NPX = W * H * 4;
    uint8_t a[80 * 80 * 4];
    uint8_t b[80 * 80 * 4];

    struct canvas *__single ca = canvas(W, H, CANVAS_CS_SRGB);
    struct canvas *__single cb = canvas(W, H, CANVAS_CS_SRGB);
    CHECK(ca != NULL && cb != NULL);
    if (!ca || !cb) {
        canvas_free(ca);
        canvas_free(cb);
        return;
    }
    canvas_set_transform(ca, 1.3f, 0.2f, -0.4f, 1.1f, 12.0f, 8.0f);
    canvas_set_transform_3x3(cb, 1.3f, 0.2f, -0.4f, 1.1f, 12.0f, 8.0f,
                             0.0f, 0.0f, 1.0f);
    for (int k = 0; k < 2; k++) {
        struct canvas *__single c = k == 0 ? ca : cb;
        canvas_set_fill_rgba(c, CANVAS_CS_SRGB, 0.2f, 0.6f, 0.9f, 1.0f);
        canvas_fill_rect(c, 4.0f, 4.0f, 20.0f, 16.0f);
        canvas_set_stroke_rgba(c, CANVAS_CS_SRGB, 0.9f, 0.3f, 0.1f, 1.0f);
        canvas_set_line_width(c, 2.0f);
        canvas_begin_path(c);
        canvas_move_to(c, 6.0f, 30.0f);
        canvas_line_to(c, 40.0f, 36.0f);
        canvas_line_to(c, 20.0f, 50.0f);
        canvas_close_path(c);
        canvas_stroke(c);
    }
    canvas_read_rgba(ca, CANVAS_CS_SRGB, a, NPX);
    canvas_read_rgba(cb, CANVAS_CS_SRGB, b, NPX);
    CHECK(memcmp(a, b, (size_t)NPX) == 0);
    canvas_free(ca);
    canvas_free(cb);
}

// In-memory record -> replay round trip with a perspective transform reproduces
// the surface byte for byte.
static void test_record_replay_roundtrip(void) {
    int const W = 200, H = 150, NPX = W * H * 4;
    char const *__null_terminated path = "build/test_perspective.canvas";
    uint8_t recorded[200 * 150 * 4];

    {
        struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas_record_to(cv, path));
        canvas_set_perspective_quad(cv, 0.0f, 0.0f, 4.0f, 4.0f,
                                    50.0f, 30.0f, 150.0f, 30.0f,
                                    190.0f, 120.0f, 10.0f, 120.0f);
        canvas_set_fill_rgba(cv, CANVAS_CS_SRGB, 0.9f, 0.4f, 0.1f, 1.0f);
        canvas_fill_rect(cv, 0.0f, 0.0f, 4.0f, 4.0f);
        canvas_set_stroke_rgba(cv, CANVAS_CS_SRGB, 0.1f, 0.2f, 0.9f, 1.0f);
        canvas_set_line_width(cv, 0.1f);
        canvas_begin_path(cv);
        canvas_move_to(cv, 1.0f, 1.0f);
        canvas_line_to(cv, 3.0f, 1.0f);
        canvas_line_to(cv, 3.0f, 3.0f);
        canvas_close_path(cv);
        canvas_stroke(cv);
        canvas_read_rgba(cv, CANVAS_CS_SRGB, recorded, NPX);
        canvas_free(cv);  // flush + close
    }
    {
        struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas_replay_from(cv, path));
        uint8_t replayed[200 * 150 * 4];
        canvas_read_rgba(cv, CANVAS_CS_SRGB, replayed, NPX);
        CHECK(memcmp(recorded, replayed, (size_t)NPX) == 0);
        canvas_free(cv);
    }
}

int main(void) {
    test_is_affine_and_identity();
    test_affine_apply_bit_identical();
    test_perspective_apply_divides();
    test_affine_mul_invert_bit_identical();
    test_perspective_invert_roundtrip();
    test_quad_maps_corners();
    test_fill_lands();
    test_fully_behind_draws_nothing();
    test_set_transform_3x3_matches_affine();
    test_record_replay_roundtrip();
    return TEST_REPORT();
}
