// The direction attribute (canvas2d_set_direction): defaults ltr, resolves
// textAlign start/end (start == left under ltr, == right under rtl; end the
// opposite), participates in save/restore and reset like textAlign, and
// round-trips through record/replay as a `set_direction` op line.  Alignment
// is pinned by byte-comparing whole renders: a start-aligned draw under one
// direction must equal the explicitly left- or right-aligned draw it resolves
// to -- the same anchor math, so the pixels are identical, not merely close.

#include "canvas2d.h"
#include "canvas2d_replay.h"
#include "test_util.h"

#include <stdint.h>
#include <string.h>

#define W 160
#define H 60
#define LEN (W * H * 4)

// sizeof-1 keeps the literal an array (bounds known), avoiding the
// __null_terminated->__counted_by seam at the call.
#define REPLAY(cv, s) canvas2d_replay_text((cv), (s), sizeof(s) - 1)

// Render `text` with the given direction/align into px: white ground, black
// ink, one fill_text anchored mid-canvas.
static void render(uint8_t *__counted_by(LEN) px, char const *__null_terminated text,
                   enum canvas2d_direction dir, enum canvas2d_text_align align) {
    struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_set_font_size(cv, 24.0f);
    canvas2d_set_direction(cv, dir);
    canvas2d_set_text_align(cv, align);
    canvas2d_fill_text(cv, text, (float)W * 0.5f, 40.0f);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, LEN);
    canvas2d_free(cv);
}

// start/end resolve against direction; left/right/center ignore it.
static void check_resolution(void) {
    static uint8_t a[LEN], b[LEN];
    char const *__null_terminated t = "abc";

    render(a, t, CANVAS2D_DIRECTION_LTR, CANVAS2D_ALIGN_START);
    render(b, t, CANVAS2D_DIRECTION_LTR, CANVAS2D_ALIGN_LEFT);
    CHECK(memcmp(a, b, LEN) == 0);  // ltr: start == left

    render(a, t, CANVAS2D_DIRECTION_LTR, CANVAS2D_ALIGN_END);
    render(b, t, CANVAS2D_DIRECTION_LTR, CANVAS2D_ALIGN_RIGHT);
    CHECK(memcmp(a, b, LEN) == 0);  // ltr: end == right

    render(a, t, CANVAS2D_DIRECTION_RTL, CANVAS2D_ALIGN_START);
    render(b, t, CANVAS2D_DIRECTION_LTR, CANVAS2D_ALIGN_RIGHT);
    CHECK(memcmp(a, b, LEN) == 0);  // rtl: start == right

    render(a, t, CANVAS2D_DIRECTION_RTL, CANVAS2D_ALIGN_END);
    render(b, t, CANVAS2D_DIRECTION_LTR, CANVAS2D_ALIGN_LEFT);
    CHECK(memcmp(a, b, LEN) == 0);  // rtl: end == left

    render(a, t, CANVAS2D_DIRECTION_RTL, CANVAS2D_ALIGN_LEFT);
    render(b, t, CANVAS2D_DIRECTION_LTR, CANVAS2D_ALIGN_LEFT);
    CHECK(memcmp(a, b, LEN) == 0);  // physical left ignores direction

    render(a, t, CANVAS2D_DIRECTION_RTL, CANVAS2D_ALIGN_CENTER);
    render(b, t, CANVAS2D_DIRECTION_LTR, CANVAS2D_ALIGN_CENTER);
    CHECK(memcmp(a, b, LEN) == 0);  // center too

    // start under the two directions genuinely differ (the checks above
    // can't all be vacuously-equal blank renders).
    render(a, t, CANVAS2D_DIRECTION_LTR, CANVAS2D_ALIGN_START);
    render(b, t, CANVAS2D_DIRECTION_RTL, CANVAS2D_ALIGN_START);
    CHECK(memcmp(a, b, LEN) != 0);
}

// Default ltr; save/restore brackets a direction change; reset restores it.
static void check_state(void) {
    static uint8_t a[LEN], b[LEN];
    char const *__null_terminated t = "abc";

    // Default: never touched == explicitly ltr.
    {
        struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 1.0f, 1.0f, 1.0f);
        canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
        canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
        canvas2d_set_font_size(cv, 24.0f);
        canvas2d_set_text_align(cv, CANVAS2D_ALIGN_START);
        canvas2d_fill_text(cv, t, (float)W * 0.5f, 40.0f);
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, a, LEN);
        canvas2d_free(cv);
    }
    render(b, t, CANVAS2D_DIRECTION_LTR, CANVAS2D_ALIGN_START);
    CHECK(memcmp(a, b, LEN) == 0);

    // save/restore: the inner ltr is undone, the outer rtl comes back.
    {
        struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 1.0f, 1.0f, 1.0f);
        canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
        canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
        canvas2d_set_font_size(cv, 24.0f);
        canvas2d_set_text_align(cv, CANVAS2D_ALIGN_START);
        canvas2d_set_direction(cv, CANVAS2D_DIRECTION_RTL);
        canvas2d_save(cv);
        canvas2d_set_direction(cv, CANVAS2D_DIRECTION_LTR);
        canvas2d_restore(cv);
        canvas2d_fill_text(cv, t, (float)W * 0.5f, 40.0f);
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, a, LEN);

        // reset: back to the ltr default.
        canvas2d_reset(cv);
        canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 1.0f, 1.0f, 1.0f);
        canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
        canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
        canvas2d_set_font_size(cv, 24.0f);
        canvas2d_set_text_align(cv, CANVAS2D_ALIGN_START);
        canvas2d_fill_text(cv, t, (float)W * 0.5f, 40.0f);
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, b, LEN);
        canvas2d_free(cv);
    }
    static uint8_t r[LEN];
    render(r, t, CANVAS2D_DIRECTION_RTL, CANVAS2D_ALIGN_START);
    CHECK(memcmp(a, r, LEN) == 0);   // restored to rtl
    render(r, t, CANVAS2D_DIRECTION_LTR, CANVAS2D_ALIGN_START);
    CHECK(memcmp(b, r, LEN) == 0);   // reset to ltr
}

// The set_direction op line: a replayed program reproduces a direct draw, and
// malformed lines reject without corrupting the canvas.
static void check_replay(void) {
    static uint8_t a[LEN], b[LEN];
    {
        struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(REPLAY(cv,
            "set_fill_rgba 1 1 1 1 srgb\n"
            "fill_rect 0 0 160 60\n"
            "set_fill_rgba 0 0 0 1 srgb\n"
            "set_font_size 24\n"
            "set_direction rtl\n"
            "set_text_align start\n"
            "fill_text 80 40 abc\n"));
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, a, LEN);
        canvas2d_free(cv);
    }
    render(b, "abc", CANVAS2D_DIRECTION_RTL, CANVAS2D_ALIGN_START);
    CHECK(memcmp(a, b, LEN) == 0);

    // Strict parsing.
    struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    CHECK(REPLAY(cv, "set_direction ltr\n"));
    CHECK(REPLAY(cv, "set_direction rtl\n"));
    CHECK(!REPLAY(cv, "set_direction\n"));          // missing argument
    CHECK(!REPLAY(cv, "set_direction up\n"));       // not a direction
    CHECK(!REPLAY(cv, "set_direction rtl junk\n")); // trailing junk
    CHECK(!REPLAY(cv, "set_direction RTL\n"));      // exact spelling only

    // Not corrupted: the canvas still draws.
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    uint8_t px[LEN];
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, LEN);
    CHECK(px[0] == 255);
    canvas2d_free(cv);
}

// Recording writes the op line: a recorded rtl draw replays to identical bytes.
static void check_record(void) {
    char const *__null_terminated path = "build/test_rtl_rec.canvas";
    static uint8_t a[LEN], b[LEN];
    {
        struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas2d_record_to(cv, path));
        canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 1.0f, 1.0f, 1.0f);
        canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
        canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
        canvas2d_set_font_size(cv, 24.0f);
        canvas2d_set_direction(cv, CANVAS2D_DIRECTION_RTL);
        canvas2d_set_text_align(cv, CANVAS2D_ALIGN_START);
        canvas2d_fill_text(cv, "abc", (float)W * 0.5f, 40.0f);
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, a, LEN);
        canvas2d_free(cv);
    }
    {
        struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
        CHECK(cv != NULL);
        if (!cv) {
            return;
        }
        CHECK(canvas2d_replay_from(cv, path));
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, b, LEN);
        canvas2d_free(cv);
    }
    CHECK(memcmp(a, b, LEN) == 0);
}

// Ink extent of a render: the min/max x of any non-white pixel.  Returns the
// ink pixel count (0 = blank).
static long ink_x(uint8_t const *__counted_by(LEN) px, int *__single xmin,
                  int *__single xmax) {
    *xmin = W;
    *xmax = -1;
    long n = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (px[(y * W + x) * 4] < 250) {
                n++;
                if (x < *xmin) { *xmin = x; }
                if (x > *xmax) { *xmax = x; }
            }
        }
    }
    return n;
}

// A string measures the way it draws, under rtl too: start-aligned RTL text
// hangs its whole advance to the LEFT of the anchor, within the measured width
// (plus a couple of px of ink overhang/antialiasing slop).
static void check_draw_measure(void) {
    char const *__null_terminated heb = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D";  // שלום
    float const x = 120.0f;

    struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas2d_set_font_size(cv, 24.0f);
    canvas2d_set_direction(cv, CANVAS2D_DIRECTION_RTL);
    float const w = canvas2d_measure_text(cv, heb);
    CHECK(w > 0.0f);

    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_set_text_align(cv, CANVAS2D_ALIGN_START);
    canvas2d_fill_text(cv, heb, x, 40.0f);

    static uint8_t px[LEN];
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, LEN);
    int xmin = 0, xmax = 0;
    CHECK(ink_x(px, &xmin, &xmax) > 0);
    CHECK((float)xmax <= x + 2.0f);      // nothing right of the anchor
    CHECK((float)xmin >= x - w - 2.0f);  // nothing left of anchor - advance

    // maxWidth under rtl: condensing anchors at the same (right) edge, so the
    // ink stays right-pinned and shrinks into half the measured width.
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 1.0f, 1.0f, 1.0f, 1.0f);
    canvas2d_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas2d_set_fill_rgba(cv, CANVAS2D_CS_SRGB, 0.0f, 0.0f, 0.0f, 1.0f);
    canvas2d_fill_text_max(cv, heb, x, 40.0f, w * 0.5f);
    canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, LEN);
    CHECK(ink_x(px, &xmin, &xmax) > 0);
    CHECK((float)xmax <= x + 2.0f);
    CHECK((float)xmin >= x - w * 0.5f - 2.0f);

    canvas2d_free(cv);
}

// Mixed-direction text really reorders: the same bytes, same anchor, same
// alignment draw differently under ltr and rtl paragraphs (bidi run order and
// neutral resolution change), while the advance -- and so measureText --
// agrees to a hair.
static void check_mixed_reorders(void) {
    char const *__null_terminated mixed = "\xD7\x90\xD7\x91 ab!";  // "אב ab!"
    static uint8_t a[LEN], b[LEN];
    render(a, mixed, CANVAS2D_DIRECTION_LTR, CANVAS2D_ALIGN_LEFT);
    render(b, mixed, CANVAS2D_DIRECTION_RTL, CANVAS2D_ALIGN_LEFT);
    CHECK(memcmp(a, b, LEN) != 0);  // physical alignment, so only the
                                    // reordering can differ -- and it does
    struct canvas2d_context *__single cv = canvas2d(W, H, CANVAS2D_CS_SRGB);
    CHECK(cv != NULL);
    if (!cv) {
        return;
    }
    canvas2d_set_font_size(cv, 24.0f);
    float const wl = canvas2d_measure_text(cv, mixed);
    canvas2d_set_direction(cv, CANVAS2D_DIRECTION_RTL);
    float const wr = canvas2d_measure_text(cv, mixed);
    CHECK(wl > 0.0f);
    float const d = wl - wr;  // same advances, summed in another order
    CHECK(d < 0.01f && d > -0.01f);
    canvas2d_free(cv);
}

int main(void) {
    check_resolution();
    check_state();
    check_replay();
    check_record();
    check_draw_measure();
    check_mixed_reorders();
    return TEST_REPORT();
}
