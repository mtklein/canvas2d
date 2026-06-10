#include "cnvs_math.h"

#include <math.h>

cnvs_mat cnvs_mat_identity(void) {
    return (cnvs_mat){ .a = 1.0f, .b = 0.0f, .c = 0.0f,
                       .d = 1.0f, .e = 0.0f, .f = 0.0f };
}

cnvs_mat cnvs_mat_mul(cnvs_mat m, cnvs_mat n) {
    return (cnvs_mat){
        .a = m.a * n.a + m.c * n.b,
        .b = m.b * n.a + m.d * n.b,
        .c = m.a * n.c + m.c * n.d,
        .d = m.b * n.c + m.d * n.d,
        .e = m.a * n.e + m.c * n.f + m.e,
        .f = m.b * n.e + m.d * n.f + m.f,
    };
}

cnvs_mat cnvs_mat_translate(float tx, float ty) {
    return (cnvs_mat){ .a = 1.0f, .b = 0.0f, .c = 0.0f,
                       .d = 1.0f, .e = tx, .f = ty };
}

cnvs_mat cnvs_mat_scale(float sx, float sy) {
    return (cnvs_mat){ .a = sx, .b = 0.0f, .c = 0.0f,
                       .d = sy, .e = 0.0f, .f = 0.0f };
}

cnvs_mat cnvs_mat_rotate(float radians) {
    float s = sinf(radians);
    float k = cosf(radians);
    return (cnvs_mat){ .a = k, .b = s, .c = -s,
                       .d = k, .e = 0.0f, .f = 0.0f };
}

cnvs_vec2 cnvs_mat_apply(cnvs_mat m, cnvs_vec2 p) {
    return (cnvs_vec2){
        .x = m.a * p.x + m.c * p.y + m.e,
        .y = m.b * p.x + m.d * p.y + m.f,
    };
}

cnvs_mat cnvs_mat_invert(cnvs_mat m) {
    float det = m.a * m.d - m.b * m.c;
    if (det < 1e-12f && det > -1e-12f) {
        return cnvs_mat_identity();
    }
    float inv = 1.0f / det;
    cnvs_mat r = { .a = m.d * inv, .b = -m.b * inv,
                   .c = -m.c * inv, .d = m.a * inv };
    r.e = -(r.a * m.e + r.c * m.f);
    r.f = -(r.b * m.e + r.d * m.f);
    return r;
}

cnvs_unpremul cnvs_unpremul_of(float r, float g, float b, float a) {
    return (cnvs_unpremul){ .r = (_Float16)r, .g = (_Float16)g,
                            .b = (_Float16)b, .a = (_Float16)a };
}

typedef _Float16 premh4 __attribute__((ext_vector_type(4)));

cnvs_premul cnvs_premultiply(cnvs_unpremul c) {
    // {r*a, g*a, b*a, a}, clamped to [0,1] -- the multiply and clamp run in
    // _Float16 directly (no widen to f32, no narrowing convert on the way
    // out), one 4-lane vector op each.  Every 8-bit edge value still
    // round-trips exactly under f16 arithmetic (test_image's exhaustive
    // sweep; docs/decisions/color-axis.md experiment 1).
    premh4 p = { c.r, c.g, c.b, c.a };
    _Float16 a = p[3];
    premh4 out = p * (premh4){ a, a, a, (_Float16)1.0f };
    out = __builtin_elementwise_min((premh4)(_Float16)1.0f,
                                    __builtin_elementwise_max((premh4)(_Float16)0.0f, out));
    return (cnvs_premul){ .r = out[0], .g = out[1], .b = out[2], .a = out[3] };
}

cnvs_unpremul cnvs_unpremultiply(cnvs_premul c) {
    // The inverse divide, also in _Float16: r/a, g/a, b/a (lane 3 divides to
    // a/a and is overwritten with a), clamped to [0,1].
    _Float16 a = c.a;
    if (a <= (_Float16)0.0f) {  // fully transparent un-premultiplies to all zero
        return (cnvs_unpremul){ .r = 0, .g = 0, .b = 0, .a = 0 };
    }
    premh4 u = (premh4){ c.r, c.g, c.b, c.a } / a;
    u[3] = a;
    u = __builtin_elementwise_min((premh4)(_Float16)1.0f,
                                  __builtin_elementwise_max((premh4)(_Float16)0.0f, u));
    return (cnvs_unpremul){ .r = u[0], .g = u[1], .b = u[2], .a = u[3] };
}
