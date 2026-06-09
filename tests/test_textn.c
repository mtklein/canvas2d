// canvas_fill_text_n / canvas_stroke_text_n: the length-counted text API renders
// exactly `len` bytes, with no terminator trust.  Rendering a 3-byte slice of a
// larger buffer whose following bytes are non-NUL must match fill_text of the
// 3-char string (and differ from the whole buffer) -- which it can only do if the
// walk stops at text+len rather than at a NUL.  Built under -fbounds-safety.

#include "test_util.h"

#include "canvas.h"

#include <ptrcheck.h>
#include <stdint.h>
#include <string.h>

#define W 64
#define H 32
#define NPX (W * H * 4)

static void setup(canvas *__single cv) {
    canvas_set_font_size(cv, 16.0f);
    canvas_set_fill_rgba(cv, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas_set_stroke_rgba(cv, 0.0f, 0.0f, 0.0f, 1.0f);
}

int main(void) {
    canvas *__single a = canvas_create(W, H);
    canvas *__single b = canvas_create(W, H);
    canvas *__single c = canvas_create(W, H);
    CHECK(a != NULL);
    CHECK(b != NULL);
    CHECK(c != NULL);
    setup(a);
    setup(b);
    setup(c);

    // "ABC" three ways.
    canvas_fill_text(a, "ABC", 4.0f, 20.0f);

    // As a 3-byte slice of "ABCDEF": the bytes after the slice are non-NUL, so a
    // terminator-stopping walk would render "ABCDEF" and diverge.
    char buf[] = "ABCDEF";
    canvas_fill_text_n(b, buf, 3, 4.0f, 20.0f);

    // The whole buffer, for contrast.
    canvas_fill_text(c, "ABCDEF", 4.0f, 20.0f);

    uint8_t pa[NPX], pb[NPX], pc[NPX];
    canvas_read_rgba(a, pa, (int)sizeof pa);
    canvas_read_rgba(b, pb, (int)sizeof pb);
    canvas_read_rgba(c, pc, (int)sizeof pc);

    CHECK(memcmp(pa, pb, sizeof pa) == 0);  // slice of 3 == "ABC"
    CHECK(memcmp(pa, pc, sizeof pa) != 0);  // and "ABC" != "ABCDEF" (non-trivial)

    // stroke_text_n likewise matches stroke_text on the slice.
    canvas *__single d = canvas_create(W, H);
    canvas *__single e = canvas_create(W, H);
    CHECK(d != NULL);
    CHECK(e != NULL);
    setup(d);
    setup(e);
    canvas_stroke_text(d, "ABC", 4.0f, 20.0f);
    canvas_stroke_text_n(e, buf, 3, 4.0f, 20.0f);
    uint8_t pd[NPX], pe[NPX];
    canvas_read_rgba(d, pd, (int)sizeof pd);
    canvas_read_rgba(e, pe, (int)sizeof pe);
    CHECK(memcmp(pd, pe, sizeof pd) == 0);

    // A zero-length slice draws nothing (and must not read text at all).
    canvas *__single f = canvas_create(W, H);
    CHECK(f != NULL);
    setup(f);
    canvas_fill_text_n(f, buf, 0, 4.0f, 20.0f);
    uint8_t pf[NPX];
    canvas_read_rgba(f, pf, (int)sizeof pf);
    bool any = false;
    for (int i = 0; i < NPX; i++) {
        if (pf[i] != 0) { any = true; break; }
    }
    CHECK(!any);

    canvas_destroy(a);
    canvas_destroy(b);
    canvas_destroy(c);
    canvas_destroy(d);
    canvas_destroy(e);
    canvas_destroy(f);
    return TEST_REPORT();
}
