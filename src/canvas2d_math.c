#include "canvas2d_color.h"
#include "canvas2d_math.h"
#include "canvas2d_matrix.h"

#include <math.h>

canvas2d_matrix canvas2d_matrix_identity(void) {
    return (canvas2d_matrix){ .a = 1.0f, .c = 0.0f, .e = 0.0f,
                       .b = 0.0f, .d = 1.0f, .f = 0.0f,
                       .g = 0.0f, .h = 0.0f, .i = 1.0f };
}

bool canvas2d_matrix_is_affine(canvas2d_matrix m) {
    return m.g == 0.0f && m.h == 0.0f && m.i == 1.0f;
}

canvas2d_matrix canvas2d_matrix_mul(canvas2d_matrix m, canvas2d_matrix n) {
    if (canvas2d_matrix_is_affine(m) && canvas2d_matrix_is_affine(n)) {
        // Both affine: the old 2x3 product, expression for expression, so an
        // affine chain stays bit-identical to the pre-homography era.
        return (canvas2d_matrix){
            .a = m.a * n.a + m.c * n.b,
            .c = m.a * n.c + m.c * n.d,
            .e = m.a * n.e + m.c * n.f + m.e,
            .b = m.b * n.a + m.d * n.b,
            .d = m.b * n.c + m.d * n.d,
            .f = m.b * n.e + m.d * n.f + m.f,
            .g = 0.0f, .h = 0.0f, .i = 1.0f,
        };
    }
    // Full 3x3 product (column vectors: the (a,b,g) etc. layout).
    return (canvas2d_matrix){
        .a = m.a * n.a + m.c * n.b + m.e * n.g,
        .b = m.b * n.a + m.d * n.b + m.f * n.g,
        .g = m.g * n.a + m.h * n.b + m.i * n.g,
        .c = m.a * n.c + m.c * n.d + m.e * n.h,
        .d = m.b * n.c + m.d * n.d + m.f * n.h,
        .h = m.g * n.c + m.h * n.d + m.i * n.h,
        .e = m.a * n.e + m.c * n.f + m.e * n.i,
        .f = m.b * n.e + m.d * n.f + m.f * n.i,
        .i = m.g * n.e + m.h * n.f + m.i * n.i,
    };
}

canvas2d_matrix canvas2d_matrix_translate(float tx, float ty) {
    return (canvas2d_matrix){ .a = 1.0f, .c = 0.0f, .e = tx,
                       .b = 0.0f, .d = 1.0f, .f = ty,
                       .g = 0.0f, .h = 0.0f, .i = 1.0f };
}

canvas2d_matrix canvas2d_matrix_scale(float sx, float sy) {
    return (canvas2d_matrix){ .a = sx,   .c = 0.0f, .e = 0.0f,
                       .b = 0.0f, .d = sy,   .f = 0.0f,
                       .g = 0.0f, .h = 0.0f, .i = 1.0f };
}

canvas2d_matrix canvas2d_matrix_rotate(float radians) {
    float const s = sinf(radians);
    float c = cosf(radians);
    return (canvas2d_matrix){ .a = c, .c = -s, .e = 0.0f,
                       .b = s, .d =  c, .f = 0.0f,
                       .g = 0.0f, .h = 0.0f, .i = 1.0f };
}

canvas2d_vec2 canvas2d_matrix_apply(canvas2d_matrix m, canvas2d_vec2 p) {
    if (canvas2d_matrix_is_affine(m)) {
        // No divide: identical to the 2x3 apply (w == 1 exactly).
        return (canvas2d_vec2){
            .x = m.a * p.x + m.c * p.y + m.e,
            .y = m.b * p.x + m.d * p.y + m.f,
        };
    }
    float const w = m.g * p.x + m.h * p.y + m.i;
    float const inv = 1.0f / w;
    return (canvas2d_vec2){
        .x = (m.a * p.x + m.c * p.y + m.e) * inv,
        .y = (m.b * p.x + m.d * p.y + m.f) * inv,
    };
}

canvas2d_matrix canvas2d_matrix_invert(canvas2d_matrix m) {
    if (canvas2d_matrix_is_affine(m)) {
        // The old 2x3 inverse, arithmetic unchanged, so an affine matrix
        // inverts bit-identically (the device->user maps the sampler relies on).
        float const det = m.a * m.d - m.b * m.c;
        if (det < 1e-12f && det > -1e-12f) {
            return canvas2d_matrix_identity();
        }
        float const inv = 1.0f / det;
        canvas2d_matrix r = { .a =  m.d * inv, .c = -m.c * inv,
                       .b = -m.b * inv, .d =  m.a * inv,
                       .g = 0.0f, .h = 0.0f, .i = 1.0f };
        r.e = -(r.a * m.e + r.c * m.f);
        r.f = -(r.b * m.e + r.d * m.f);
        return r;
    }
    // General 3x3 inverse via the adjugate / determinant.
    float const A =  (m.d * m.i - m.f * m.h);
    float const B = -(m.b * m.i - m.f * m.g);
    float const C =  (m.b * m.h - m.d * m.g);
    float const det = m.a * A + m.c * B + m.e * C;
    if (det < 1e-12f && det > -1e-12f) {
        return canvas2d_matrix_identity();
    }
    float const inv = 1.0f / det;
    // Adjugate (transpose of the cofactor matrix) scaled by 1/det.
    return (canvas2d_matrix){
        .a = A * inv,
        .b = B * inv,
        .g = C * inv,
        .c = -(m.c * m.i - m.e * m.h) * inv,
        .d =  (m.a * m.i - m.e * m.g) * inv,
        .h = -(m.a * m.h - m.c * m.g) * inv,
        .e =  (m.c * m.f - m.e * m.d) * inv,
        .f = -(m.a * m.f - m.e * m.b) * inv,
        .i =  (m.a * m.d - m.c * m.b) * inv,
    };
}

canvas2d_unpremul canvas2d_unpremul_of(float r, float g, float b, float a) {
    return (canvas2d_unpremul){ .r = (_Float16)r, .g = (_Float16)g,
                            .b = (_Float16)b, .a = (_Float16)a };
}

canvas2d_premul canvas2d_premultiply(canvas2d_unpremul c) {
    // {r*a, g*a, b*a, a} in _Float16 (docs/decisions/color-axis.md).  Alpha
    // clamps to [0,1]; the colour planes keep NO bound -- extended linear sRGB
    // carries colour both above the [0,1] gamut (HDR) and below it (wide gamut),
    // and the only place either collapses is the output encode+quantize (the
    // same doctrine as canvas2d_px8_clamp_premul_lin).
    _Float16 const a = __builtin_elementwise_max((_Float16)0.0f,
                       __builtin_elementwise_min((_Float16)1.0f, c.a));
    return (canvas2d_premul){ .r = c.r * a, .g = c.g * a, .b = c.b * a, .a = a };
}

canvas2d_unpremul canvas2d_unpremultiply(canvas2d_premul c) {
    // r/a, g/a, b/a; alpha clamps to [0,1].  The colour planes keep NO bound
    // (extended linear sRGB -- HDR above the gamut, wide gamut below); only the
    // output encode+quantize collapses the range.
    _Float16 const a = c.a;
    if (a <= (_Float16)0.0f) {  // fully transparent un-premultiplies to all zero
        return (canvas2d_unpremul){ .r = 0, .g = 0, .b = 0, .a = 0 };
    }
    _Float16 const ca = __builtin_elementwise_min((_Float16)1.0f, a);
    return (canvas2d_unpremul){ .r = c.r / a, .g = c.g / a, .b = c.b / a, .a = ca };
}
