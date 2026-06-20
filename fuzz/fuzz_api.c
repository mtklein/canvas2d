// canvas2d public-API state-machine fuzzer (Role A).
//
// Maps the fuzz input to a sequence of public canvas_* calls via a *total*
// streaming decoder: any byte string is a valid program (opcodes and counts are
// taken modulo their range, a short read just ends the program), so a coverage
// fuzzer drives deep into the renderer instead of bouncing off input validation.
//
// Build: ninja fuzzers.  Two roles for one source:
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

struct cursor {
    uint8_t const *__counted_by(size) p;
    size_t size;
    size_t at;
    int eof;
};

static uint8_t rd_u8(struct cursor *c) {
    if (c->at >= c->size) {
        c->eof = 1;
        return 0;
    }
    return c->p[c->at++];
}

// Raw 4-byte reinterpret: every float value (incl. NaN/Inf/subnormals/huge) is
// reachable, which is the point for the (int)-cast UB hunt.
static float rd_f32(struct cursor *c) {
    uint8_t b[4] = { rd_u8(c), rd_u8(c), rd_u8(c), rd_u8(c) };
    float f;
    memcpy(&f, b, sizeof f);
    return f;
}

// Inclusive integer range, total (two bytes -> value modulo the span).
static int rd_range(struct cursor *c, int lo, int hi) {
    if (hi <= lo) {
        return lo;
    }
    uint32_t const span = (uint32_t)(hi - lo) + 1u;
    uint32_t const v = (uint32_t)rd_u8(c) << 8 | (uint32_t)rd_u8(c);
    return lo + (int)(v % span);
}

// A fuzzed INPUT colour space: any of the three authoring spaces (sRGB, linear,
// Oklab) -- exercises intern_color / the gradient-stop conversion paths, not just
// the sRGB default.
static enum canvas_color_space rd_cs(struct cursor *c) {
    return (enum canvas_color_space)rd_range(c, 0, 2);
}

static void do_image_get(struct canvas *__single cv, struct cursor *c, int W, int H) {
    int w = rd_range(c, 0, 64), h = rd_range(c, 0, 64);
    int x = rd_range(c, -8, W + 8), y = rd_range(c, -8, H + 8);
    int const len = w * h * 4;                       // w,h <= 64 -> fits int
    uint8_t *__counted_by(len) out = malloc(len > 0 ? (size_t)len : 1);
    if (out) {
        canvas_get_image_data(cv, CANVAS_CS_SRGB, x, y, w, h, out, len);
    }
    free(out);
}

static void do_image_put(struct canvas *__single cv, struct cursor *c) {
    int w = rd_range(c, 0, 64), h = rd_range(c, 0, 64);
    int dx = rd_range(c, -8, 64), dy = rd_range(c, -8, 64);
    int const len = w * h * 4;
    uint8_t *__counted_by(len) data = malloc(len > 0 ? (size_t)len : 1);
    if (data) {
        for (int i = 0; i < len; i++) {
            data[i] = rd_u8(c);
        }
        canvas_put_image_data(cv, CANVAS_CS_SRGB, data, len, w, h, dx, dy);
    }
    free(data);
}

static void do_image_draw(struct canvas *__single cv, struct cursor *c) {
    int sw = rd_range(c, 1, 32), sh = rd_range(c, 1, 32);
    int const slen = sw * sh * 4;
    uint8_t *__counted_by(slen) src = malloc((size_t)slen);
    if (src) {
        for (int i = 0; i < slen; i++) {
            src[i] = rd_u8(c);
        }
        canvas_draw_bitmap_scaled(cv, CANVAS_CS_SRGB, src, sw, sh, rd_f32(c), rd_f32(c),
                                 rd_f32(c), rd_f32(c));
    }
    free(src);
}

// A reified canvas_image in one of the four formats, optionally with mips, drawn
// scaled at a chosen smoothing quality -- so the structured fuzzer reaches the
// trilinear chain, the Catmull-Rom magnifier, and the whole f16 sampler family
// (the bitmap path do_image_draw above never builds an image object).
static void do_image_obj(struct canvas *__single cv, struct cursor *c) {
    int const sw = rd_range(c, 1, 32), sh = rd_range(c, 1, 32);
    int const fmt = rd_range(c, 0, 3);  // 0:u8 unpremul 1:u8 premul 2:f16 unpremul 3:f16 premul
    bool const f16 = fmt >= 2;
    enum canvas_alpha_type const at = (fmt & 1) ? CANVAS_ALPHA_PREMUL
                                                : CANVAS_ALPHA_UNPREMUL;
    int const npx = sw * sh;
    struct canvas_image *__single img = NULL;
    if (f16) {
        int const n = npx * 4;
        _Float16 *__counted_by(n) px = malloc((size_t)n * sizeof *px);
        if (px) {
            for (int i = 0; i < n; i++) {
                // Bytes -> [0,1] f16; the channels need not be valid premul, the
                // sampler must tolerate any data (the total-decoder contract).
                px[i] = (_Float16)((float)rd_u8(c) / 255.0f);
            }
            img = canvas_image_f16(CANVAS_CS_SRGB, px, sw, sh, at);
        }
        free(px);
    } else {
        int const n = npx * 4;
        uint8_t *__counted_by(n) px = malloc((size_t)n);
        if (px) {
            for (int i = 0; i < n; i++) {
                px[i] = rd_u8(c);
            }
            img = canvas_image_unorm8(CANVAS_CS_SRGB, px, sw, sh, at);
        }
        free(px);
    }
    if (!img) {
        return;
    }
    if (rd_u8(c) & 1) {
        (void)canvas_image_build_mips(img);  // half the time: the cached chain
    }
    canvas_set_image_smoothing_enabled(cv, (rd_u8(c) & 1) != 0);
    canvas_set_image_smoothing_quality(cv,
        (enum canvas_image_smoothing_quality)rd_range(c, 0, 2));
    // Pick a draw overload: scaled (minify/magnify chosen by the dims) or subrect.
    if (rd_u8(c) & 1) {
        canvas_draw_image_scaled(cv, img, rd_f32(c), rd_f32(c), rd_f32(c), rd_f32(c));
    } else {
        canvas_draw_image_subrect(cv, img, rd_f32(c), rd_f32(c), rd_f32(c), rd_f32(c),
                                  rd_f32(c), rd_f32(c), rd_f32(c), rd_f32(c));
    }
    canvas_image_free(img);
}

int LLVMFuzzerTestOneInput(uint8_t const *__counted_by(size) data, size_t size) {
    struct cursor c = { .p = data, .size = size, .at = 0, .eof = 0 };

    int W = rd_range(&c, 1, 256), H = rd_range(&c, 1, 256);
    // Vary the working space: sRGB or extended linear sRGB (Oklab is not a
    // compositing space).  Fuzzes the linear decode/encode + extended-range paths.
    enum canvas_color_space const ws =
        (rd_u8(&c) & 1) ? CANVAS_CS_LINEAR_SRGB : CANVAS_CS_SRGB;
    struct canvas *__single cv = canvas_in_space(W, H, ws);
    if (!cv) {
        return 0;
    }

    float stops[4];  // scratch reused for color components
    enum canvas_fill_rule rule = CANVAS_NONZERO;  // per-call: OP_SET_FILL_RULE
                                                  // picks what OP_FILL/OP_CLIP pass

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
            case OP_CUBIC_TO:  canvas_bezier_curve_to(cv, rd_f32(&c), rd_f32(&c),
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

            case OP_FILL:       canvas_fill(cv, rule); break;
            case OP_STROKE:     canvas_stroke(cv); break;
            case OP_CLIP:       canvas_clip(cv, rule); break;
            case OP_FILL_RECT:  canvas_fill_rect(cv, rd_f32(&c), rd_f32(&c),
                                                 rd_f32(&c), rd_f32(&c)); break;
            case OP_CLEAR_RECT: canvas_clear_rect(cv, rd_f32(&c), rd_f32(&c),
                                                  rd_f32(&c), rd_f32(&c)); break;

            case OP_SET_FILL_RGBA:   canvas_set_fill_rgba(cv, rd_cs(&c), rd_f32(&c), rd_f32(&c),
                                                          rd_f32(&c), rd_f32(&c)); break;
            case OP_SET_STROKE_RGBA: canvas_set_stroke_rgba(cv, rd_cs(&c), rd_f32(&c), rd_f32(&c),
                                                            rd_f32(&c), rd_f32(&c)); break;
            case OP_SET_GLOBAL_ALPHA: canvas_set_global_alpha(cv, rd_f32(&c)); break;
            case OP_SET_LINE_WIDTH:  canvas_set_line_width(cv, rd_f32(&c)); break;
            case OP_SET_LINE_JOIN:   canvas_set_line_join(cv,
                                         (enum canvas_line_join)rd_range(&c, 0, 2)); break;
            case OP_SET_LINE_CAP:    canvas_set_line_cap(cv,
                                         (enum canvas_line_cap)rd_range(&c, 0, 2)); break;
            case OP_SET_MITER_LIMIT: canvas_set_miter_limit(cv, rd_f32(&c)); break;
            case OP_SET_FILL_RULE:   rule =
                                         (enum canvas_fill_rule)rd_range(&c, 0, 1); break;
            case OP_SET_COMPOSITE:   canvas_set_global_composite_operation(cv,
                                         (enum canvas_composite_op)rd_range(&c, 0, 25)); break;
            case OP_SET_LINE_DASH: {
                int const n = rd_range(&c, 0, 8);
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
                canvas_add_fill_color_stop(cv, rd_cs(&c), rd_f32(&c), stops[0], stops[1],
                                           stops[2], stops[3]);
                break;
            case OP_STROKE_LINEAR_GRAD: canvas_set_stroke_linear_gradient(cv, rd_f32(&c),
                                            rd_f32(&c), rd_f32(&c), rd_f32(&c)); break;
            case OP_ADD_STROKE_STOP:
                for (int i = 0; i < 4; i++) { stops[i] = rd_f32(&c); }
                canvas_add_stroke_color_stop(cv, rd_cs(&c), rd_f32(&c), stops[0], stops[1],
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
            case OP_DRAW_IMAGE_OBJ: do_image_obj(cv, &c); break;
            case OP_ENCODE: {  // the BT.2100 16-bit encode path; discard the bytes
                int n = 0;
                free(canvas_encode_png(cv, &n));
            } break;
            default: break;
        }
    }

    canvas_free(cv);
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
        long const n = ftell(f);
        fseek(f, 0, SEEK_SET);
        size_t const cap = n > 0 ? (size_t)n : 1;
        uint8_t *buf = malloc(cap);
        size_t const got = buf ? fread(buf, 1, n > 0 ? (size_t)n : 0, f) : 0;
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
