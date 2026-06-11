// Metamorphic / property tests: instead of hand-computed expected pixels (that's
// test_composite.c), assert *relations* that must hold for any input -- order
// independence, transposes, degenerate-case identities, transform invariance.
// These are cheap to state, hard to get wrong by coincidence, and they drive the
// modes/branches the example tests miss (the HSL non-separable blends, the
// Porter-Duff switch, the transform path).  Runs on both backends, which are
// bit-identical, so the render-vs-render relations are checked at tol 0.

#include "canvas.h"
#include "test_pixels.h"
#include "test_util.h"

#include <stdlib.h>
#include <string.h>

enum { W = 16, H = 16, LEN = W * H * 4 };

// Composite layer 1 (r1,g1,b1,a1) over layer 2 (r2,g2,b2,a2): paint layer 2 on a
// cleared canvas, then layer 1 under `op`; return the centre pixel.  Either layer
// may be translucent.
static struct rgba over(struct canvas *__single cv, uint8_t *__counted_by(LEN) px,
                       enum canvas_composite_op op,
                       float r1, float g1, float b1, float a1,
                       float r2, float g2, float b2, float a2) {
    canvas_set_global_composite_operation(cv, CANVAS_OP_SOURCE_OVER);
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_set_fill_rgba(cv, r2, g2, b2, a2);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_set_global_composite_operation(cv, op);
    canvas_set_fill_rgba(cv, r1, g1, b1, a1);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_read_rgba(cv, px, LEN);
    return pixel_at(px, LEN, W, W / 2, H / 2);
}

// Opaque backdrop (br,bg,bb), then (sr,sg,sb,sa) under `op`; centre pixel.
static struct rgba blend(struct canvas *__single cv, uint8_t *__counted_by(LEN) px,
                        enum canvas_composite_op op, float br, float bg, float bb,
                        float sr, float sg, float sb, float sa) {
    return over(cv, px, op, sr, sg, sb, sa, br, bg, bb, 1.0f);
}

static bool px_eq(struct rgba a, struct rgba b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// A spread of colours to quantify over (so a relation isn't an accident of one hue).
static float const COL[][3] = {
    { 0.90f, 0.20f, 0.25f }, { 0.20f, 0.75f, 0.40f }, { 0.25f, 0.45f, 0.95f },
    { 0.95f, 0.85f, 0.20f }, { 0.13f, 0.55f, 0.77f }, { 0.50f, 0.50f, 0.50f },
    { 1.00f, 1.00f, 1.00f }, { 0.00f, 0.00f, 0.00f },
};
enum { NCOL = (int)(sizeof COL / sizeof COL[0]) };

int main(void) {
    uint8_t *__counted_by(LEN) px = malloc((size_t)LEN);
    uint8_t *__counted_by(LEN) qx = malloc((size_t)LEN);
    CHECK(px && qx);
    struct canvas *__single cv = canvas(W, H);
    CHECK(cv != NULL);
    if (!px || !qx || !cv) {
        free(px);
        free(qx);
        if (cv) { canvas_free(cv); }
        return TEST_REPORT();
    }

    // 1. Symmetric separable blends are order-independent for ANY two layers, not
    //    just opaque ones.  The premultiplied composite
    //        co = (1-ab)*s + (1-as)*d + as*ab*B(Cb,Cs),  ar = as + ab - as*ab
    //    is invariant under swapping the (colour,alpha) pairs when B(x,y)==B(y,x):
    //    the two cross terms (1-ab)*as*Cs and (1-as)*ab*Cb just trade places, and B
    //    is symmetric.  So layer1-over-layer2 == layer2-over-layer1 bit-exactly,
    //    including when one or both layers are translucent (verified: maxd 0 at
    //    alpha 1.0/0.5/0.25).  The translucent cases are what exercise the full
    //    s*(1-da)+d*(1-sa)+T path rather than the opaque T-only reduction.
    enum canvas_composite_op const SYM[] = {
        CANVAS_OP_MULTIPLY, CANVAS_OP_SCREEN, CANVAS_OP_DARKEN,
        CANVAS_OP_LIGHTEN, CANVAS_OP_DIFFERENCE, CANVAS_OP_EXCLUSION,
    };
    static struct { float c[3]; float a; } const LAYER[] = {
        { { 0.90f, 0.20f, 0.25f }, 1.00f }, { { 0.20f, 0.75f, 0.40f }, 0.50f },
        { { 0.25f, 0.45f, 0.95f }, 1.00f }, { { 0.95f, 0.85f, 0.20f }, 0.25f },
        { { 0.13f, 0.55f, 0.77f }, 0.50f }, { { 0.50f, 0.50f, 0.50f }, 1.00f },
    };
    int const NL = (int)(sizeof LAYER / sizeof LAYER[0]);
    for (int m = 0; m < (int)(sizeof SYM / sizeof SYM[0]); m++) {
        for (int i = 0; i < NL; i++) {
            for (int j = 0; j < NL; j++) {
                struct rgba ij = over(cv, px, SYM[m],
                                     LAYER[i].c[0], LAYER[i].c[1], LAYER[i].c[2], LAYER[i].a,
                                     LAYER[j].c[0], LAYER[j].c[1], LAYER[j].c[2], LAYER[j].a);
                struct rgba ji = over(cv, px, SYM[m],
                                     LAYER[j].c[0], LAYER[j].c[1], LAYER[j].c[2], LAYER[j].a,
                                     LAYER[i].c[0], LAYER[i].c[1], LAYER[i].c[2], LAYER[i].a);
                CHECK(px_eq(ij, ji));
            }
        }
    }

    // 2. overlay and hard-light are transposes: overlay(Cb,Cs) == hard-light(Cs,Cb).
    //    So A-over-B overlay == B-over-A hard-light, exactly.
    for (int i = 0; i < NCOL; i++) {
        for (int j = 0; j < NCOL; j++) {
            struct rgba ov = blend(cv, px, CANVAS_OP_OVERLAY, COL[i][0], COL[i][1], COL[i][2],
                                  COL[j][0], COL[j][1], COL[j][2], 1.0f);
            struct rgba hl = blend(cv, px, CANVAS_OP_HARD_LIGHT, COL[j][0], COL[j][1], COL[j][2],
                                  COL[i][0], COL[i][1], COL[i][2], 1.0f);
            CHECK(px_eq(ov, hl));
        }
    }

    // 3. Non-separable HSL blends are identities when both layers are the same colour:
    //    hue/sat/color/luminosity of C onto C all recover C.  Drives set_lum,
    //    set_sat, clip_color, lum -- the chunk the example tests never reach.  The
    //    float HSL round-trip can land 1/255 off, so allow tol 1.
    enum canvas_composite_op const HSL[] = {
        CANVAS_OP_HUE, CANVAS_OP_SATURATION, CANVAS_OP_COLOR, CANVAS_OP_LUMINOSITY,
    };
    for (int m = 0; m < (int)(sizeof HSL / sizeof HSL[0]); m++) {
        for (int i = 0; i < NCOL; i++) {
            struct rgba self = blend(cv, px, HSL[m], COL[i][0], COL[i][1], COL[i][2],
                                    COL[i][0], COL[i][1], COL[i][2], 1.0f);
            struct rgba want = blend(cv, px, CANVAS_OP_COPY, 0.0f, 0.0f, 0.0f,
                                   COL[i][0], COL[i][1], COL[i][2], 1.0f);
            CHECK(px_near(self, want.r, want.g, want.b, want.a, 1));
        }
    }

    // 4. Porter-Duff reductions over an opaque backdrop, for any opaque source S:
    //    {copy, source-in, source-atop, source-over} all yield S; {dst-over, dst-in,
    //    dst-atop} all leave the backdrop B.  Checks the whole fa/fb switch.
    for (int i = 0; i < NCOL; i++) {
        float br = COL[(i + 3) % NCOL][0], bgc = COL[(i + 3) % NCOL][1], bb = COL[(i + 3) % NCOL][2];
        float sr = COL[i][0], sg = COL[i][1], sb = COL[i][2];
        struct rgba s = blend(cv, px, CANVAS_OP_COPY, br, bgc, bb, sr, sg, sb, 1.0f);  // == S
        struct rgba b = blend(cv, px, CANVAS_OP_COPY, sr, sg, sb, br, bgc, bb, 1.0f);  // == B
        enum canvas_composite_op const TO_S[] = { CANVAS_OP_SOURCE_IN, CANVAS_OP_SOURCE_ATOP,
                                             CANVAS_OP_SOURCE_OVER };
        enum canvas_composite_op const TO_B[] = { CANVAS_OP_DESTINATION_OVER, CANVAS_OP_DESTINATION_IN,
                                             CANVAS_OP_DESTINATION_ATOP };
        for (int k = 0; k < 3; k++) {
            CHECK(px_eq(blend(cv, px, TO_S[k], br, bgc, bb, sr, sg, sb, 1.0f), s));
            CHECK(px_eq(blend(cv, px, TO_B[k], br, bgc, bb, sr, sg, sb, 1.0f), b));
        }
    }

    // 5. multiply over white == source; screen over black == source (B(1,Cs)=Cs,
    //    B(0,Cs)=Cs).  Holds for every colour; exercises the polynomial path.
    for (int i = 0; i < NCOL; i++) {
        float sr = COL[i][0], sg = COL[i][1], sb = COL[i][2];
        struct rgba s = blend(cv, px, CANVAS_OP_COPY, 0, 0, 0, sr, sg, sb, 1.0f);
        CHECK(px_near(blend(cv, px, CANVAS_OP_MULTIPLY, 1, 1, 1, sr, sg, sb, 1.0f),
                      s.r, s.g, s.b, s.a, 1));
        CHECK(px_near(blend(cv, px, CANVAS_OP_SCREEN, 0, 0, 0, sr, sg, sb, 1.0f),
                      s.r, s.g, s.b, s.a, 1));
    }

    // 6. Transform translation invariance: a disc drawn at an integer offset matches
    //    the same disc under canvas_translate of that offset.  Drives the CTM path.
    canvas_set_global_composite_operation(cv, CANVAS_OP_SOURCE_OVER);
    canvas_reset_transform(cv);
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_set_fill_rgba(cv, 0.9f, 0.4f, 0.2f, 1.0f);
    canvas_begin_path(cv);
    canvas_arc(cv, 9.0f, 9.0f, 4.0f, 0.0f, 6.2831853f, false);
    canvas_fill(cv, CANVAS_NONZERO);
    canvas_read_rgba(cv, px, LEN);          // disc at (9,9), direct
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_save(cv);
    canvas_translate(cv, 4.0f, 3.0f);
    canvas_set_fill_rgba(cv, 0.9f, 0.4f, 0.2f, 1.0f);
    canvas_begin_path(cv);
    canvas_arc(cv, 5.0f, 6.0f, 4.0f, 0.0f, 6.2831853f, false);  // 5+4=9, 6+3=9
    canvas_fill(cv, CANVAS_NONZERO);
    canvas_restore(cv);
    canvas_read_rgba(cv, qx, LEN);          // disc at (9,9), via translate
    CHECK(memcmp(px, qx, (size_t)LEN) == 0);

    // 7. fill_rect is equivalent to filling the same rectangle as a path.
    canvas_reset_transform(cv);
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_set_fill_rgba(cv, 0.3f, 0.7f, 0.9f, 1.0f);
    canvas_fill_rect(cv, 2.0f, 3.0f, 9.0f, 7.0f);
    canvas_read_rgba(cv, px, LEN);
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_set_fill_rgba(cv, 0.3f, 0.7f, 0.9f, 1.0f);
    canvas_begin_path(cv);
    canvas_rect(cv, 2.0f, 3.0f, 9.0f, 7.0f);
    canvas_fill(cv, CANVAS_NONZERO);
    canvas_read_rgba(cv, qx, LEN);
    CHECK(memcmp(px, qx, (size_t)LEN) == 0);

    // 8. A gradient whose stops are all one colour IS a solid fill of that
    //    colour, byte for byte: the row evaluator lerps the exact stop colours
    //    (no ramp quantisation, docs/decisions/gradient-eval.md), so equal
    //    stops collapse to the solid paint exactly.
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_set_fill_rgba(cv, 0.4f, 0.6f, 0.85f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_read_rgba(cv, px, LEN);
    canvas_clear_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_set_fill_linear_gradient(cv, 0.0f, 0.0f, (float)W, 0.0f);
    canvas_add_fill_color_stop(cv, 0.0f, 0.4f, 0.6f, 0.85f, 1.0f);
    canvas_add_fill_color_stop(cv, 1.0f, 0.4f, 0.6f, 0.85f, 1.0f);
    canvas_fill_rect(cv, 0.0f, 0.0f, (float)W, (float)H);
    canvas_read_rgba(cv, qx, LEN);
    CHECK(memcmp(px, qx, (size_t)LEN) == 0);

    canvas_free(cv);
    free(px);
    free(qx);
    return TEST_REPORT();
}
