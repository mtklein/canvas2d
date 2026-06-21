#include "canvas2d_color.h"
#include "canvas2d_math.h"
#include "canvas2d_matrix.h"

#include <math.h>

canvas2d_mat canvas2d_mat_identity(void) {
    return (canvas2d_mat){ .a = 1.0f, .c = 0.0f, .e = 0.0f,
                       .b = 0.0f, .d = 1.0f, .f = 0.0f,
                       .g = 0.0f, .h = 0.0f, .i = 1.0f };
}

bool canvas2d_mat_is_affine(canvas2d_mat m) {
    return m.g == 0.0f && m.h == 0.0f && m.i == 1.0f;
}

canvas2d_mat canvas2d_mat_mul(canvas2d_mat m, canvas2d_mat n) {
    if (canvas2d_mat_is_affine(m) && canvas2d_mat_is_affine(n)) {
        // Both affine: the old 2x3 product, expression for expression, so an
        // affine chain stays bit-identical to the pre-homography era.
        return (canvas2d_mat){
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
    return (canvas2d_mat){
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

canvas2d_mat canvas2d_mat_translate(float tx, float ty) {
    return (canvas2d_mat){ .a = 1.0f, .c = 0.0f, .e = tx,
                       .b = 0.0f, .d = 1.0f, .f = ty,
                       .g = 0.0f, .h = 0.0f, .i = 1.0f };
}

canvas2d_mat canvas2d_mat_scale(float sx, float sy) {
    return (canvas2d_mat){ .a = sx,   .c = 0.0f, .e = 0.0f,
                       .b = 0.0f, .d = sy,   .f = 0.0f,
                       .g = 0.0f, .h = 0.0f, .i = 1.0f };
}

canvas2d_mat canvas2d_mat_rotate(float radians) {
    float const s = sinf(radians);
    float c = cosf(radians);
    return (canvas2d_mat){ .a = c, .c = -s, .e = 0.0f,
                       .b = s, .d =  c, .f = 0.0f,
                       .g = 0.0f, .h = 0.0f, .i = 1.0f };
}

canvas2d_vec2 canvas2d_mat_apply(canvas2d_mat m, canvas2d_vec2 p) {
    if (canvas2d_mat_is_affine(m)) {
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

canvas2d_mat canvas2d_mat_invert(canvas2d_mat m) {
    if (canvas2d_mat_is_affine(m)) {
        // The old 2x3 inverse, arithmetic unchanged, so an affine matrix
        // inverts bit-identically (the device->user maps the sampler relies on).
        float const det = m.a * m.d - m.b * m.c;
        if (det < 1e-12f && det > -1e-12f) {
            return canvas2d_mat_identity();
        }
        float const inv = 1.0f / det;
        canvas2d_mat r = { .a =  m.d * inv, .c = -m.c * inv,
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
        return canvas2d_mat_identity();
    }
    float const inv = 1.0f / det;
    // Adjugate (transpose of the cofactor matrix) scaled by 1/det.
    return (canvas2d_mat){
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
    // {r*a, g*a, b*a, a}, clamped to [0,1], in _Float16 directly
    // (docs/decisions/color-axis.md).
    half4 const p = { c.r, c.g, c.b, c.a };
    _Float16 a = p[3];
    half4 out = p * (half4){ a, a, a, (_Float16)1.0f };
    out = __builtin_elementwise_min((half4)(_Float16)1.0f,
                                    __builtin_elementwise_max((half4)(_Float16)0.0f, out));
    return (canvas2d_premul){ .r = out[0], .g = out[1], .b = out[2], .a = out[3] };
}

canvas2d_unpremul canvas2d_unpremultiply(canvas2d_premul c) {
    // r/a, g/a, b/a (lane 3 divides to a/a and is overwritten with a),
    // clamped to [0,1].
    _Float16 a = c.a;
    if (a <= (_Float16)0.0f) {  // fully transparent un-premultiplies to all zero
        return (canvas2d_unpremul){ .r = 0, .g = 0, .b = 0, .a = 0 };
    }
    half4 u = (half4){ c.r, c.g, c.b, c.a } / a;
    u[3] = a;
    u = __builtin_elementwise_min((half4)(_Float16)1.0f,
                                  __builtin_elementwise_max((half4)(_Float16)0.0f, u));
    return (canvas2d_unpremul){ .r = u[0], .g = u[1], .b = u[2], .a = u[3] };
}
