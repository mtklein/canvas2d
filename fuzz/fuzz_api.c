// canvas2d public-API state-machine fuzzer (Role A).
//
// Maps the fuzz input to a sequence of public canvas_* calls via a *total*
// streaming decoder: any byte string is a valid program (opcodes and counts are
// taken modulo their range, a short read just ends the program), so a coverage
// fuzzer drives deep into the renderer instead of bouncing off input validation.
//
// Build: ninja fuzz.  Two roles for one source:
//   - afl-clang-fast + ASan + UBSan (no -fbounds-safety): the discovery oracle.
//   - Apple clang + -fbounds-safety: replay a crasher to confirm the feature
//     converts the would-be corruption into a trap.
// The harness body is the same; <ptrcheck.h> is real under Apple clang and a
// no-op stub (fuzz/shim) under afl-clang-fast.
//
// Coordinates are read as raw 4-byte floats, so NaN/Inf/huge values flow into
// the path/transform math and the (int) casts in points_bbox -- the float-cast
// UB class UBSan is here to catch.  Text ops are deliberately excluded (Core
// Text shim is a separate target).  Image dimensions are clamped small so the
// fuzzer probes clip/clamp logic without OOMing on multi-GB allocations.

#include "canvas.h"
#include "fuzz_ops.h"

#include <ptrcheck.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t const *__counted_by(size) p;
    size_t size;
    size_t at;
    int eof;
} cursor;

static uint8_t rd_u8(cursor *c) {
    if (c->at >= c->size) {
        c->eof = 1;
        return 0;
    }
    return c->p[c->at++];
}

// Raw 4-byte reinterpret: every float value (incl. NaN/Inf/subnormals/huge) is
// reachable, which is the point for the (int)-cast UB hunt.
static float rd_f32(cursor *c) {
    uint8_t b[4] = { rd_u8(c), rd_u8(c), rd_u8(c), rd_u8(c) };
    float f;
    memcpy(&f, b, sizeof f);
    return f;
}

// Inclusive integer range, total (two bytes -> value modulo the span).
static int rd_range(cursor *c, int lo, int hi) {
    if (hi <= lo) {
        return lo;
    }
    uint32_t span = (uint32_t)(hi - lo) + 1u;
    uint32_t v = (uint32_t)rd_u8(c) << 8 | (uint32_t)rd_u8(c);
    return lo + (int)(v % span);
}

static void do_image_get(canvas *__single cv, cursor *c, int W, int H) {
    int w = rd_range(c, 0, 64), h = rd_range(c, 0, 64);
    int x = rd_range(c, -8, W + 8), y = rd_range(c, -8, H + 8);
    int len = w * h * 4;                       // w,h <= 64 -> fits int
    uint8_t *__counted_by(len) out = malloc(len > 0 ? (size_t)len : 1);
    if (out) {
        canvas_get_image_data(cv, x, y, w, h, out, len);
    }
    free(out);
}

static void do_image_put(canvas *__single cv, cursor *c) {
    int w = rd_range(c, 0, 64), h = rd_range(c, 0, 64);
    int dx = rd_range(c, -8, 64), dy = rd_range(c, -8, 64);
    int len = w * h * 4;
    uint8_t *__counted_by(len) data = malloc(len > 0 ? (size_t)len : 1);
    if (data) {
        for (int i = 0; i < len; i++) {
            data[i] = rd_u8(c);
        }
        canvas_put_image_data(cv, data, len, w, h, dx, dy);
    }
    free(data);
}

static void do_image_draw(canvas *__single cv, cursor *c) {
    int sw = rd_range(c, 1, 32), sh = rd_range(c, 1, 32);
    int slen = sw * sh * 4;
    uint8_t *__counted_by(slen) src = malloc((size_t)slen);
    if (src) {
        for (int i = 0; i < slen; i++) {
            src[i] = rd_u8(c);
        }
        canvas_draw_image_scaled(cv, src, sw, sh, rd_f32(c), rd_f32(c),
                                 rd_f32(c), rd_f32(c));
    }
    free(src);
}

int LLVMFuzzerTestOneInput(uint8_t const *__counted_by(size) data, size_t size) {
    cursor c = { .p = data, .size = size, .at = 0, .eof = 0 };

    int W = rd_range(&c, 1, 256), H = rd_range(&c, 1, 256);
    canvas *__single cv = canvas_create(W, H);
    if (!cv) {
        return 0;
    }

    float stops[4];  // scratch reused for color components

    int budget = 0;
    while (!c.eof && budget++ < 2000) {
        switch ((int)((unsigned)rd_u8(&c) % (unsigned)FUZZ_OP_COUNT)) {
            case OP_BEGIN_PATH: canvas_begin_path(cv); break;
            case OP_MOVE_TO:    canvas_move_to(cv, rd_f32(&c), rd_f32(&c)); break;
            case OP_LINE_TO:    canvas_line_to(cv, rd_f32(&c), rd_f32(&c)); break;
            case OP_RECT:       canvas_rect(cv, rd_f32(&c), rd_f32(&c),
                                            rd_f32(&c), rd_f32(&c)); break;
            case OP_QUAD_TO:    canvas_quadratic_curve_to(cv, rd_f32(&c), rd_f32(&c),
                                                          rd_f32(&c), rd_f32(&c)); break;
            case OP_BEZIER_TO:  canvas_bezier_curve_to(cv, rd_f32(&c), rd_f32(&c),
                                                       rd_f32(&c), rd_f32(&c),
                                                       rd_f32(&c), rd_f32(&c)); break;
            case OP_ARC:        canvas_arc(cv, rd_f32(&c), rd_f32(&c), rd_f32(&c),
                                           rd_f32(&c), rd_f32(&c), rd_u8(&c) & 1); break;
            case OP_ELLIPSE:    canvas_ellipse(cv, rd_f32(&c), rd_f32(&c), rd_f32(&c),
                                               rd_f32(&c), rd_f32(&c), rd_f32(&c),
                                               rd_f32(&c), rd_u8(&c) & 1); break;
            case OP_ROUND_RECT: canvas_round_rect(cv, rd_f32(&c), rd_f32(&c),
                                                  rd_f32(&c), rd_f32(&c), rd_f32(&c)); break;
            case OP_ARC_TO:     canvas_arc_to(cv, rd_f32(&c), rd_f32(&c),
                                              rd_f32(&c), rd_f32(&c), rd_f32(&c)); break;
            case OP_CLOSE_PATH: canvas_close_path(cv); break;

            case OP_FILL:       canvas_fill(cv); break;
            case OP_STROKE:     canvas_stroke(cv); break;
            case OP_CLIP:       canvas_clip(cv); break;
            case OP_FILL_RECT:  canvas_fill_rect(cv, rd_f32(&c), rd_f32(&c),
                                                 rd_f32(&c), rd_f32(&c)); break;
            case OP_CLEAR_RECT: canvas_clear_rect(cv, rd_f32(&c), rd_f32(&c),
                                                  rd_f32(&c), rd_f32(&c)); break;

            case OP_SET_FILL_RGBA:   canvas_set_fill_rgba(cv, rd_f32(&c), rd_f32(&c),
                                                          rd_f32(&c), rd_f32(&c)); break;
            case OP_SET_STROKE_RGBA: canvas_set_stroke_rgba(cv, rd_f32(&c), rd_f32(&c),
                                                            rd_f32(&c), rd_f32(&c)); break;
            case OP_SET_GLOBAL_ALPHA: canvas_set_global_alpha(cv, rd_f32(&c)); break;
            case OP_SET_LINE_WIDTH:  canvas_set_line_width(cv, rd_f32(&c)); break;
            case OP_SET_LINE_JOIN:   canvas_set_line_join(cv,
                                         (canvas_line_join)rd_range(&c, 0, 2)); break;
            case OP_SET_LINE_CAP:    canvas_set_line_cap(cv,
                                         (canvas_line_cap)rd_range(&c, 0, 2)); break;
            case OP_SET_MITER_LIMIT: canvas_set_miter_limit(cv, rd_f32(&c)); break;
            case OP_SET_FILL_RULE:   canvas_set_fill_rule(cv,
                                         (canvas_fill_rule)rd_range(&c, 0, 1)); break;
            case OP_SET_COMPOSITE:   canvas_set_global_composite_operation(cv,
                                         (canvas_composite_op)rd_range(&c, 0, 25)); break;
            case OP_SET_LINE_DASH: {
                int n = rd_range(&c, 0, 8);
                float dash[8];
                for (int i = 0; i < n; i++) {
                    dash[i] = rd_f32(&c);
                }
                canvas_set_line_dash(cv, dash, n);
                break;
            }
            case OP_SET_DASH_OFFSET: canvas_set_line_dash_offset(cv, rd_f32(&c)); break;

            case OP_FILL_LINEAR_GRAD: canvas_set_fill_linear_gradient(cv, rd_f32(&c),
                                          rd_f32(&c), rd_f32(&c), rd_f32(&c)); break;
            case OP_FILL_RADIAL_GRAD: canvas_set_fill_radial_gradient(cv, rd_f32(&c),
                                          rd_f32(&c), rd_f32(&c), rd_f32(&c),
                                          rd_f32(&c), rd_f32(&c)); break;
            case OP_ADD_FILL_STOP:
                for (int i = 0; i < 4; i++) { stops[i] = rd_f32(&c); }
                canvas_add_fill_color_stop(cv, rd_f32(&c), stops[0], stops[1],
                                           stops[2], stops[3]);
                break;
            case OP_STROKE_LINEAR_GRAD: canvas_set_stroke_linear_gradient(cv, rd_f32(&c),
                                            rd_f32(&c), rd_f32(&c), rd_f32(&c)); break;
            case OP_ADD_STROKE_STOP:
                for (int i = 0; i < 4; i++) { stops[i] = rd_f32(&c); }
                canvas_add_stroke_color_stop(cv, rd_f32(&c), stops[0], stops[1],
                                             stops[2], stops[3]);
                break;

            case OP_SAVE:    canvas_save(cv); break;
            case OP_RESTORE: canvas_restore(cv); break;
            case OP_TRANSLATE: canvas_translate(cv, rd_f32(&c), rd_f32(&c)); break;
            case OP_SCALE:     canvas_scale(cv, rd_f32(&c), rd_f32(&c)); break;
            case OP_ROTATE:    canvas_rotate(cv, rd_f32(&c)); break;
            case OP_TRANSFORM: canvas_transform(cv, rd_f32(&c), rd_f32(&c), rd_f32(&c),
                                                rd_f32(&c), rd_f32(&c), rd_f32(&c)); break;
            case OP_SET_TRANSFORM: canvas_set_transform(cv, rd_f32(&c), rd_f32(&c),
                                       rd_f32(&c), rd_f32(&c), rd_f32(&c), rd_f32(&c)); break;
            case OP_RESET_TRANSFORM: canvas_reset_transform(cv); break;

            case OP_GET_IMAGE_DATA: do_image_get(cv, &c, W, H); break;
            case OP_PUT_IMAGE_DATA: do_image_put(cv, &c); break;
            case OP_DRAW_IMAGE:     do_image_draw(cv, &c); break;
            default: break;
        }
    }

    canvas_destroy(cv);
    return 0;
}

#ifndef FUZZ_NO_MAIN
// Unified driver: works as an AFL target (`afl-fuzz ... -- ./fuzz_api @@`) and
// as a manual replay tool (`./fuzz_api crash1 crash2 ...`).  Persistent mode is
// a later speed-up; fork-per-exec is fine to get fuzzing in place.
#include <stdio.h>

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            continue;
        }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        size_t cap = n > 0 ? (size_t)n : 1;
        uint8_t *buf = malloc(cap);
        size_t got = buf ? fread(buf, 1, n > 0 ? (size_t)n : 0, f) : 0;
        fclose(f);
        if (buf) {
            LLVMFuzzerTestOneInput(buf, got);
            free(buf);
            (void)fprintf(stderr, "ok: %s (%zu bytes)\n", argv[i], got);
        }
    }
    return 0;
}
#endif
