// The CSS filter colour functions: hand-computed expectations for each kernel
// (premultiplied forms, Filter Effects matrices), list order, state plumbing
// (save/restore/reset/set_filter_none), spec clamping, and that the list
// reaches every painted op (fills, images, text) but not put_image_data.

#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <math.h>
#include <stdlib.h>

#define W 16

// Reset bitmap + state is the caller's job (the filter list is part of state);
// this fills the whole canvas with (r,g,b,a) through whatever filters are set
// and reads back the centre pixel, unpremultiplied RGBA8.
static struct px4 fill_and_read(canvas *__single cv, uint8_t *__counted_by(len) px,
                                int len, float r, float g, float b, float a) {
    canvas_set_fill_rgba(cv, r, g, b, a);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    canvas_read_rgba(cv, px, len);
    return pixel_at(px, len, W, 8, 8);
}

int main(void) {
    int const len = W * W * 4;
    uint8_t *__counted_by(len) px = malloc((size_t)len);
    CHECK(px != NULL);
    if (!px) {
        return TEST_REPORT();
    }
    canvas *__single cv = canvas_create(W, W);
    CHECK(cv != NULL);
    if (!cv) {
        free(px);
        return TEST_REPORT();
    }

    // Each function on a solid fill, against hand-computed RGBA8.
    // brightness(0.5): red scales to half.
    canvas_add_filter_brightness(cv, 0.5f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  128, 0, 0, 255, 1));

    // brightness(2): quarter red doubles; full red clamps at the alpha.
    canvas_reset(cv);
    canvas_add_filter_brightness(cv, 2.0f);
    CHECK(px_near(fill_and_read(cv, px, len, 0.25f, 0.0f, 0.0f, 1.0f),
                  128, 0, 0, 255, 1));
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  255, 0, 0, 255, 1));

    // contrast(0.5): c' = 0.5c + 0.25, so red -> (0.75, 0.25, 0.25).
    canvas_reset(cv);
    canvas_add_filter_contrast(cv, 0.5f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  191, 64, 64, 255, 1));

    // grayscale(1): red collapses to its 0.2126 luminance.
    canvas_reset(cv);
    canvas_add_filter_grayscale(cv, 1.0f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  54, 54, 54, 255, 1));

    // saturate(0) is the same luminance projection.
    canvas_reset(cv);
    canvas_add_filter_saturate(cv, 0.0f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  54, 54, 54, 255, 1));

    // saturate(2) pushes a desaturated red away from gray: r' = 0.69685,
    // g' = b' = 0.19685 for (0.5, 0.25, 0.25).
    canvas_reset(cv);
    canvas_add_filter_saturate(cv, 2.0f);
    CHECK(px_near(fill_and_read(cv, px, len, 0.5f, 0.25f, 0.25f, 1.0f),
                  178, 50, 50, 255, 2));

    // sepia(1): red maps through the sepia matrix's first column.
    canvas_reset(cv);
    canvas_add_filter_sepia(cv, 1.0f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  100, 89, 69, 255, 1));

    // hue_rotate(pi): M = 2L - I, so red -> (2*0.2126 - 1, 0.4252, 0.4252),
    // the negative red lane clamping to 0.
    canvas_reset(cv);
    canvas_add_filter_hue_rotate(cv, (float)M_PI);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  0, 108, 108, 255, 2));

    // hue_rotate(pi/2) on red: (0, 0.2126 + 0.1427, negative -> 0).
    canvas_reset(cv);
    canvas_add_filter_hue_rotate(cv, (float)M_PI * 0.5f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  0, 91, 0, 255, 2));

    // invert(1) complements; invert(0.5) lands every colour on mid-gray.
    canvas_reset(cv);
    canvas_add_filter_invert(cv, 1.0f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  0, 255, 255, 255, 1));
    canvas_reset(cv);
    canvas_add_filter_invert(cv, 0.5f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  128, 128, 128, 255, 1));

    // opacity(0.5): colour keeps, alpha halves.
    canvas_reset(cv);
    canvas_add_filter_opacity(cv, 0.5f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  255, 0, 0, 128, 1));

    // Identity amounts: every function at its identity leaves a translucent,
    // non-primary colour untouched.
    canvas_reset(cv);
    struct px4 const base = fill_and_read(cv, px, len, 0.4f, 0.6f, 0.8f, 0.5f);
    struct {
        void (*add)(canvas *__single cv, float amount);
        float identity;
    } const id[8] = {
        { canvas_add_filter_brightness, 1.0f },
        { canvas_add_filter_contrast,   1.0f },
        { canvas_add_filter_grayscale,  0.0f },
        { canvas_add_filter_hue_rotate, 0.0f },
        { canvas_add_filter_invert,     0.0f },
        { canvas_add_filter_opacity,    1.0f },
        { canvas_add_filter_saturate,   1.0f },
        { canvas_add_filter_sepia,      0.0f },
    };
    for (int i = 0; i < 8; i++) {
        canvas_reset(cv);
        id[i].add(cv, id[i].identity);
        CHECK(px_near(fill_and_read(cv, px, len, 0.4f, 0.6f, 0.8f, 0.5f),
                      base.r, base.g, base.b, base.a, 1));
    }

    // The list applies in call order: invert then brightness darkens the
    // complement, while brightness then invert complements the darkened red --
    // different, hand-computed results.
    canvas_reset(cv);
    canvas_add_filter_invert(cv, 1.0f);
    canvas_add_filter_brightness(cv, 0.5f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  0, 128, 128, 255, 1));
    canvas_reset(cv);
    canvas_add_filter_brightness(cv, 0.5f);
    canvas_add_filter_invert(cv, 1.0f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  128, 255, 255, 255, 1));

    // Translucent fills (alpha 0.5): the premultiplied forms.  contrast's
    // offset and invert's flip both scale by alpha -- an unpremultiplied
    // formula misapplied to premultiplied pixels gets these wrong.
    canvas_reset(cv);
    canvas_add_filter_contrast(cv, 0.5f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 0.5f),
                  191, 64, 64, 128, 2));
    canvas_reset(cv);
    canvas_add_filter_invert(cv, 1.0f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 0.5f),
                  0, 255, 255, 128, 2));
    canvas_reset(cv);
    canvas_add_filter_opacity(cv, 0.5f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 0.5f),
                  255, 0, 0, 64, 2));

    // drawImage is filtered: an opaque red sprite grays out.
    canvas_reset(cv);
    canvas_add_filter_grayscale(cv, 1.0f);
    uint8_t img[16] = { 255, 0, 0, 255, 255, 0, 0, 255,
                        255, 0, 0, 255, 255, 0, 0, 255 };
    canvas_draw_image_scaled(cv, img, 2, 2, 4.0f, 4.0f, 8.0f, 8.0f);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, W, 8, 8), 54, 54, 54, 255, 2));

    // put_image_data is NOT filtered (it overwrites; not a painted op).
    canvas_reset(cv);
    canvas_add_filter_invert(cv, 1.0f);
    canvas_put_image_data(cv, img, (int)sizeof img, 2, 2, 7, 7);
    canvas_read_rgba(cv, px, len);
    CHECK(px_near(pixel_at(px, len, W, 8, 8), 255, 0, 0, 255, 1));

    // save/restore brackets the list: the inner invert composes with the outer
    // grayscale (red -> 0.2126 gray -> 0.7874 gray), and restore sheds it.
    canvas_reset(cv);
    canvas_add_filter_grayscale(cv, 1.0f);
    canvas_save(cv);
    canvas_add_filter_invert(cv, 1.0f);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  201, 201, 201, 255, 1));
    canvas_restore(cv);
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  54, 54, 54, 255, 1));

    // set_filter_none clears the list; reset clears it too.
    canvas_set_filter_none(cv);
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)W);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  255, 0, 0, 255, 1));
    canvas_add_filter_grayscale(cv, 1.0f);
    canvas_reset(cv);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  255, 0, 0, 255, 1));

    // Clamping: negative amounts clamp to 0, the capped functions clamp to 1,
    // and non-finite amounts are ignored outright.
    canvas_reset(cv);
    canvas_add_filter_brightness(cv, -2.0f);  // == brightness(0): black
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  0, 0, 0, 255, 1));
    canvas_reset(cv);
    canvas_add_filter_grayscale(cv, 7.0f);    // == grayscale(1)
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  54, 54, 54, 255, 1));
    canvas_reset(cv);
    canvas_add_filter_opacity(cv, 3.0f);      // == opacity(1): identity
    canvas_add_filter_invert(cv, -1.0f);      // == invert(0): identity
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  255, 0, 0, 255, 1));
    canvas_reset(cv);
    canvas_add_filter_brightness(cv, NAN);          // all ignored: still red
    canvas_add_filter_grayscale(cv, INFINITY);
    canvas_add_filter_hue_rotate(cv, -INFINITY);
    CHECK(px_near(fill_and_read(cv, px, len, 1.0f, 0.0f, 0.0f, 1.0f),
                  255, 0, 0, 255, 1));

    canvas_destroy(cv);
    free(px);

    // Text is filtered: red glyphs gray out -- and because the filter runs on
    // the painted tile, even the antialiased edge pixels read as neutral gray
    // (r == g == b at every covered pixel).
    enum { TW = 32 };
    int const tlen = TW * TW * 4;
    uint8_t *__counted_by(tlen) tpx = malloc((size_t)tlen);
    canvas *__single tcv = canvas_create(TW, TW);
    CHECK(tpx != NULL && tcv != NULL);
    if (tpx && tcv) {
        canvas_add_filter_grayscale(tcv, 1.0f);
        canvas_set_fill_rgba(tcv, 1.0f, 0.0f, 0.0f, 1.0f);
        canvas_set_font_size(tcv, 28.0f);
        canvas_fill_text(tcv, "M", 4.0f, 26.0f);
        canvas_read_rgba(tcv, tpx, tlen);
        bool found_solid = false, all_gray = true;
        for (int y = 0; y < TW; y++) {
            for (int x = 0; x < TW; x++) {
                struct px4 p = pixel_at(tpx, tlen, TW, x, y);
                if (p.a > 128 && abs((int)p.r - 54) <= 3) {
                    found_solid = true;
                }
                if (p.a > 32 && (abs((int)p.r - (int)p.g) > 3 ||
                                 abs((int)p.g - (int)p.b) > 3)) {
                    all_gray = false;
                }
            }
        }
        CHECK(found_solid);
        CHECK(all_gray);
    }
    if (tcv) {
        canvas_destroy(tcv);
    }
    free(tpx);
    return TEST_REPORT();
}
