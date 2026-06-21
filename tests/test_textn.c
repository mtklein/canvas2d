// canvas2d_fill_text_n / canvas2d_stroke_text_n: the length-counted text API renders
// exactly `len` bytes, with no terminator trust.  Rendering a 3-byte slice of a
// larger buffer whose following bytes are non-NUL must match fill_text of the
// 3-char string (and differ from the whole buffer) -- which it can only do if the
// walk stops at text+len rather than at a NUL.  Built under -fbounds-safety.

#include "test_util.h"

#include "canvas2d.h"

#include <ptrcheck.h>
#include <stdint.h>
#include <string.h>

#define W 64
#define H 32
#define NPX (W * H * 4)

static void setup(struct canvas2d_context *__single cv) {
    canvas2d_set_font_size(cv, 16.0f);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_set_stroke_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
}

int main(void) {
    struct canvas2d_context *__single a = canvas2d(W, H, CANVAS2D_CS_SRGB);
    struct canvas2d_context *__single b = canvas2d(W, H, CANVAS2D_CS_SRGB);
    struct canvas2d_context *__single c = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(a != NULL);
    CHECK(b != NULL);
    CHECK(c != NULL);
    setup(a);
    setup(b);
    setup(c);

    // "ABC" three ways.
    canvas2d_fill_text(a, "ABC", 4.0f, 20.0f);

    // As a 3-byte slice of "ABCDEF": the bytes after the slice are non-NUL, so a
    // terminator-stopping walk would render "ABCDEF" and diverge.
    char buf[] = "ABCDEF";
    canvas2d_fill_text_n(b, buf, 3, 4.0f, 20.0f);

    // The whole buffer, for contrast.
    canvas2d_fill_text(c, "ABCDEF", 4.0f, 20.0f);

    uint8_t pa[NPX], pb[NPX], pc[NPX];
    canvas2d_read_rgba(a, CANVAS2D_CS_SRGB, pa, (int)sizeof pa);
    canvas2d_read_rgba(b, CANVAS2D_CS_SRGB, pb, (int)sizeof pb);
    canvas2d_read_rgba(c, CANVAS2D_CS_SRGB, pc, (int)sizeof pc);

    CHECK(memcmp(pa, pb, sizeof pa) == 0);  // slice of 3 == "ABC"
    CHECK(memcmp(pa, pc, sizeof pa) != 0);  // and "ABC" != "ABCDEF" (non-trivial)

    // stroke_text_n likewise matches stroke_text on the slice.
    struct canvas2d_context *__single d = canvas2d(W, H, CANVAS2D_CS_SRGB);
    struct canvas2d_context *__single e = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(d != NULL);
    CHECK(e != NULL);
    setup(d);
    setup(e);
    canvas2d_stroke_text(d, "ABC", 4.0f, 20.0f);
    canvas2d_stroke_text_n(e, buf, 3, 4.0f, 20.0f);
    uint8_t pd[NPX], pe[NPX];
    canvas2d_read_rgba(d, CANVAS2D_CS_SRGB, pd, (int)sizeof pd);
    canvas2d_read_rgba(e, CANVAS2D_CS_SRGB, pe, (int)sizeof pe);
    CHECK(memcmp(pd, pe, sizeof pd) == 0);

    // A zero-length slice draws nothing (and must not read text at all).
    struct canvas2d_context *__single f = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(f != NULL);
    setup(f);
    canvas2d_fill_text_n(f, buf, 0, 4.0f, 20.0f);
    uint8_t pf[NPX];
    canvas2d_read_rgba(f, CANVAS2D_CS_SRGB, pf, (int)sizeof pf);
    bool any = false;
    for (int i = 0; i < NPX; i++) {
        if (pf[i] != 0) { any = true; break; }
    }
    CHECK(!any);

    canvas2d_free(a);
    canvas2d_free(b);
    canvas2d_free(c);
    canvas2d_free(d);
    canvas2d_free(e);
    canvas2d_free(f);
    return TEST_REPORT();
}
