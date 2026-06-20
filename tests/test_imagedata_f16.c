#include "canvas.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// fp16 (_Float16) ImageData -- get/put/create -- mirroring the HTML spec's
// rgba-float16 ImageData.  The headline property is EXTENDED RANGE: a LINEAR
// canvas stores HDR (>1) and wide-gamut (negative) colour, and the f16 path
// neither clamps to [0,1] nor quantizes to 8-bit, so those values survive a
// putImageData/getImageData round trip where the byte path collapses them.

// Read one straight-RGBA f16 pixel out of a w-wide element buffer (len elements,
// four per pixel).
struct f16px { float r, g, b, a; };
static struct f16px f16_at(_Float16 const *__counted_by(len) px, int len,
                           int w, int x, int y) {
    (void)len;
    int const o = (y * w + x) * 4;
    return (struct f16px){ (float)px[o], (float)px[o + 1],
                           (float)px[o + 2], (float)px[o + 3] };
}

static bool near_f(float a, float b, float tol) {
    return fabsf(a - b) <= tol;
}

static bool px_near_f16(struct f16px p, float r, float g, float b, float a,
                        float tol) {
    return near_f(p.r, r, tol) && near_f(p.g, g, tol) &&
           near_f(p.b, b, tol) && near_f(p.a, a, tol);
}

// 1. The headline: a LINEAR canvas in LINEAR space round-trips EXTENDED values
//    (HDR > 1 and wide-gamut < 0) within f16 tolerance.  This is the property
//    the byte path cannot hold.
static void extended_roundtrip_linear(void) {
    enum { W = 4, H = 4 };
    int const len = W * H * 4;
    struct canvas *__single cv = canvas(W, H, CANVAS_CS_LINEAR_SRGB);
    _Float16 *__counted_by(len) in = malloc((size_t)len * sizeof *in);
    _Float16 *__counted_by(len) out = malloc((size_t)len * sizeof *out);
    CHECK(cv && in && out);
    if (cv && in && out) {
        // r = 3.5 (HDR), g = -0.12 (wide gamut), b = 1.0, a = 1.0 everywhere.
        for (int i = 0; i < W * H; i++) {
            in[i * 4 + 0] = (_Float16)3.5f;
            in[i * 4 + 1] = (_Float16)(-0.12f);
            in[i * 4 + 2] = (_Float16)1.0f;
            in[i * 4 + 3] = (_Float16)1.0f;
        }
        canvas_put_image_data_f16(cv, CANVAS_CS_LINEAR_SRGB, in, len, W, H, 0, 0);
        canvas_get_image_data_f16(cv, CANVAS_CS_LINEAR_SRGB, 0, 0, W, H, out, len);
        // Extended range preserved: scaled tolerance for the HDR component.
        CHECK(px_near_f16(f16_at(out, len, W, 0, 0), 3.5f, -0.12f, 1.0f, 1.0f, 0.02f));
        CHECK(px_near_f16(f16_at(out, len, W, 3, 3), 3.5f, -0.12f, 1.0f, 1.0f, 0.02f));
        // Prove the values really are extended (not silently clamped to [0,1]).
        CHECK(f16_at(out, len, W, 1, 1).r > 1.5f);
        CHECK(f16_at(out, len, W, 1, 1).g < 0.0f);
    }
    free(in);
    free(out);
    canvas_free(cv);
}

// 2. An sRGB canvas in sRGB space, [0,1] values: tight tolerance round trip
//    (pure unpremultiply, no transfer).
static void roundtrip_srgb(void) {
    enum { W = 3, H = 2 };
    int const len = W * H * 4;
    struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);  // sRGB working space
    _Float16 *__counted_by(len) in = malloc((size_t)len * sizeof *in);
    _Float16 *__counted_by(len) out = malloc((size_t)len * sizeof *out);
    CHECK(cv && in && out);
    if (cv && in && out) {
        float const vals[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
        for (int i = 0; i < W * H; i++) {
            for (int c = 0; c < 4; c++) {
                in[i * 4 + c] = (_Float16)vals[c];
            }
        }
        canvas_put_image_data_f16(cv, CANVAS_CS_SRGB, in, len, W, H, 0, 0);
        canvas_get_image_data_f16(cv, CANVAS_CS_SRGB, 0, 0, W, H, out, len);
        CHECK(px_near_f16(f16_at(out, len, W, 0, 0), 0.25f, 0.5f, 0.75f, 1.0f, 0.01f));
        CHECK(px_near_f16(f16_at(out, len, W, 2, 1), 0.25f, 0.5f, 0.75f, 1.0f, 0.01f));
    }
    free(in);
    free(out);
    canvas_free(cv);
}

// 3. Cross-space: put LINEAR-space f16 onto an sRGB canvas, read it back in the
//    SAME LINEAR space.  The deposit encodes linear->sRGB into the surface and
//    the readback decodes sRGB->linear, so a [0,1] value round-trips
//    consistently (within the 8-bit-ish surface storage's f16 tolerance).  Also
//    an OKLAB-space round trip on a linear canvas.
static void cross_space(void) {
    enum { W = 2, H = 2 };
    int const len = W * H * 4;
    // 3a. LINEAR-space f16 onto an sRGB canvas, read back LINEAR.
    {
        struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);  // sRGB working space
        _Float16 *__counted_by(len) in = malloc((size_t)len * sizeof *in);
        _Float16 *__counted_by(len) out = malloc((size_t)len * sizeof *out);
        CHECK(cv && in && out);
        if (cv && in && out) {
            float const vals[4] = { 0.2f, 0.6f, 0.9f, 1.0f };
            for (int i = 0; i < W * H; i++) {
                for (int c = 0; c < 4; c++) { in[i * 4 + c] = (_Float16)vals[c]; }
            }
            canvas_put_image_data_f16(cv, CANVAS_CS_LINEAR_SRGB, in, len, W, H, 0, 0);
            canvas_get_image_data_f16(cv, CANVAS_CS_LINEAR_SRGB, 0, 0, W, H, out, len);
            // Through the sRGB surface (linear->sRGB->linear): a bit looser.
            CHECK(px_near_f16(f16_at(out, len, W, 0, 0), 0.2f, 0.6f, 0.9f, 1.0f, 0.02f));
        }
        free(in);
        free(out);
        canvas_free(cv);
    }
    // 3b. OKLAB-space f16 onto a LINEAR canvas, read back OKLAB.
    {
        struct canvas *__single cv = canvas(W, H, CANVAS_CS_LINEAR_SRGB);
        _Float16 *__counted_by(len) in = malloc((size_t)len * sizeof *in);
        _Float16 *__counted_by(len) out = malloc((size_t)len * sizeof *out);
        CHECK(cv && in && out);
        if (cv && in && out) {
            // A mid-grey-ish Oklab triple (L, a, b) with full alpha.
            float const vals[4] = { 0.6f, 0.02f, -0.04f, 1.0f };
            for (int i = 0; i < W * H; i++) {
                for (int c = 0; c < 4; c++) { in[i * 4 + c] = (_Float16)vals[c]; }
            }
            canvas_put_image_data_f16(cv, CANVAS_CS_OKLAB, in, len, W, H, 0, 0);
            canvas_get_image_data_f16(cv, CANVAS_CS_OKLAB, 0, 0, W, H, out, len);
            CHECK(px_near_f16(f16_at(out, len, W, 0, 0), 0.6f, 0.02f, -0.04f, 1.0f, 0.02f));
        }
        free(in);
        free(out);
        canvas_free(cv);
    }
}

// 4. create_image_data_f16: correct *len, zeroed; bad dims -> NULL / *len = 0.
static void create_image_data(void) {
    int len = -1;
    _Float16 *buf = canvas_create_image_data_f16(5, 3, &len);
    CHECK(buf != NULL);
    CHECK(len == 5 * 3 * 4);
    if (buf) {
        bool all_zero = true;
        for (int i = 0; i < len; i++) {
            if ((float)buf[i] != 0.0f) { all_zero = false; break; }
        }
        CHECK(all_zero);  // zeroed == transparent black
        free(buf);
    }
    // Bad dims: NULL, *len = 0.
    len = 123;
    CHECK(canvas_create_image_data_f16(0, 4, &len) == NULL);
    CHECK(len == 0);
    len = 123;
    CHECK(canvas_create_image_data_f16(4, -1, &len) == NULL);
    CHECK(len == 0);
}

// 5. Dirty-rect put writes only the sub-rect; an out-of-canvas get reads zero
//    outside.
static void dirty_and_subrect(void) {
    enum { W = 4, H = 4 };
    int const len = W * H * 4;
    struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
    _Float16 *__counted_by(len) in = malloc((size_t)len * sizeof *in);
    _Float16 *__counted_by(len) out = malloc((size_t)len * sizeof *out);
    CHECK(cv && in && out);
    if (cv && in && out) {
        // A full opaque-green source; only its [1,1]-1x1 sub-rect is dirty.
        for (int i = 0; i < W * H; i++) {
            in[i * 4 + 0] = (_Float16)0.0f;
            in[i * 4 + 1] = (_Float16)1.0f;
            in[i * 4 + 2] = (_Float16)0.0f;
            in[i * 4 + 3] = (_Float16)1.0f;
        }
        canvas_put_image_data_dirty_f16(cv, CANVAS_CS_SRGB, in, len, W, H, 0, 0,
                                        1, 1, 1, 1);
        canvas_get_image_data_f16(cv, CANVAS_CS_SRGB, 0, 0, W, H, out, len);
        // Only (1,1) painted green; everywhere else stays transparent black.
        CHECK(px_near_f16(f16_at(out, len, W, 1, 1), 0.0f, 1.0f, 0.0f, 1.0f, 0.01f));
        CHECK(px_near_f16(f16_at(out, len, W, 0, 0), 0.0f, 0.0f, 0.0f, 0.0f, 0.01f));
        CHECK(px_near_f16(f16_at(out, len, W, 2, 2), 0.0f, 0.0f, 0.0f, 0.0f, 0.01f));

        // Sub-rect / out-of-canvas get: a 2x2 read straddling the right edge
        // (x = 3) yields the painted column in-canvas and zero out of canvas.
        enum { SW = 2, SH = 2 };
        int const slen = SW * SH * 4;
        _Float16 *__counted_by(slen) sub = malloc((size_t)slen * sizeof *sub);
        CHECK(sub != NULL);
        if (sub) {
            canvas_get_image_data_f16(cv, CANVAS_CS_SRGB, 3, 0, SW, SH, sub, slen);
            // (0,0) maps to canvas (3,0) -> transparent; (1,0) maps to canvas
            // (4,0) which is out of the canvas -> zero.
            CHECK(px_near_f16(f16_at(sub, slen, SW, 0, 0), 0.0f, 0.0f, 0.0f, 0.0f, 0.01f));
            CHECK(px_near_f16(f16_at(sub, slen, SW, 1, 0), 0.0f, 0.0f, 0.0f, 0.0f, 0.01f));
            free(sub);
        }
    }
    free(in);
    free(out);
    canvas_free(cv);
}

// 6. Recording determinism: drive a recording LINEAR canvas with put_f16
//    (extended values), replay the recording onto a fresh canvas, and confirm
//    the replayed surface matches the directly-drawn one within tolerance.
static void recording_roundtrip(void) {
    enum { W = 4, H = 4 };
    int const len = W * H * 4;
    char const *__null_terminated path = "build/test_imagedata_f16.canvas";
    _Float16 *__counted_by(len) in = malloc((size_t)len * sizeof *in);
    _Float16 *__counted_by(len) direct = malloc((size_t)len * sizeof *direct);
    _Float16 *__counted_by(len) replayed = malloc((size_t)len * sizeof *replayed);
    CHECK(in && direct && replayed);
    if (in && direct && replayed) {
        for (int i = 0; i < W * H; i++) {
            in[i * 4 + 0] = (_Float16)2.0f;     // HDR
            in[i * 4 + 1] = (_Float16)(-0.05f); // wide gamut
            in[i * 4 + 2] = (_Float16)0.5f;
            in[i * 4 + 3] = (_Float16)1.0f;
        }
        // Record a put_f16 to disk, capturing the live surface.
        struct canvas *__single rec = canvas(W, H, CANVAS_CS_LINEAR_SRGB);
        CHECK(rec != NULL);
        if (rec) {
            CHECK(canvas_record_to(rec, path));
            canvas_put_image_data_f16(rec, CANVAS_CS_LINEAR_SRGB, in, len, W, H, 0, 0);
            canvas_get_image_data_f16(rec, CANVAS_CS_LINEAR_SRGB, 0, 0, W, H, direct, len);
            canvas_free(rec);  // flush + close
        }
        // Replay onto a fresh canvas; the surface must match.
        struct canvas *__single rep = canvas(W, H, CANVAS_CS_LINEAR_SRGB);
        CHECK(rep != NULL);
        if (rep) {
            CHECK(canvas_replay_from(rep, path));
            canvas_get_image_data_f16(rep, CANVAS_CS_LINEAR_SRGB, 0, 0, W, H, replayed, len);
            canvas_free(rep);
        }
        for (int i = 0; i < len; i++) {
            CHECK(near_f((float)direct[i], (float)replayed[i], 0.02f));
        }
        // The replayed surface really carries the extended values.
        CHECK(f16_at(replayed, len, W, 0, 0).r > 1.5f);
        CHECK(f16_at(replayed, len, W, 0, 0).g < 0.0f);
    }
    free(in);
    free(direct);
    free(replayed);
}

// 7. Guards: len too small / non-positive dims are no-ops, no crash.
static void guards(void) {
    enum { W = 2, H = 2 };
    int const len = W * H * 4;
    struct canvas *__single cv = canvas(W, H, CANVAS_CS_SRGB);
    _Float16 *__counted_by(len) in = malloc((size_t)len * sizeof *in);
    _Float16 *__counted_by(len) out = malloc((size_t)len * sizeof *out);
    CHECK(cv && in && out);
    if (cv && in && out) {
        for (int i = 0; i < len; i++) { in[i] = (_Float16)0.5f; }
        for (int i = 0; i < len; i++) { out[i] = (_Float16)7.0f; }  // sentinel
        // len too small: no-op (the byte path's contract).
        canvas_put_image_data_f16(cv, CANVAS_CS_SRGB, in, len - 1, W, H, 0, 0);
        // Non-positive dims: no-op.
        canvas_put_image_data_f16(cv, CANVAS_CS_SRGB, in, len, 0, H, 0, 0);
        canvas_put_image_data_f16(cv, CANVAS_CS_SRGB, in, len, W, -1, 0, 0);
        // get with a too-small out buffer leaves it untouched (no write, no crash).
        canvas_get_image_data_f16(cv, CANVAS_CS_SRGB, 0, 0, W, H, out, len - 1);
        CHECK((float)out[0] == 7.0f);  // sentinel intact: get was a no-op
        // get with bad dims: no-op.
        canvas_get_image_data_f16(cv, CANVAS_CS_SRGB, 0, 0, 0, H, out, len);
        CHECK((float)out[0] == 7.0f);
        // The canvas is still drawable: a valid put then get works.
        canvas_put_image_data_f16(cv, CANVAS_CS_SRGB, in, len, W, H, 0, 0);
        canvas_get_image_data_f16(cv, CANVAS_CS_SRGB, 0, 0, W, H, out, len);
        CHECK(px_near_f16(f16_at(out, len, W, 0, 0), 0.5f, 0.5f, 0.5f, 0.5f, 0.01f));
    }
    free(in);
    free(out);
    canvas_free(cv);
}

int main(void) {
    extended_roundtrip_linear();
    roundtrip_srgb();
    cross_space();
    create_image_data();
    dirty_and_subrect();
    recording_roundtrip();
    guards();
    return TEST_REPORT();
}
