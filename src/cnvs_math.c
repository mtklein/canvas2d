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

// Clamp to [0,1], narrow to _Float16.
static _Float16 clamp16(float v) {
    if (v < 0.0f) { v = 0.0f; }
    if (v > 1.0f) { v = 1.0f; }
    return (_Float16)v;
}

cnvs_premul cnvs_premultiply(cnvs_unpremul c) {
    float a = (float)c.a;
    return (cnvs_premul){ .r = clamp16((float)c.r * a), .g = clamp16((float)c.g * a),
                          .b = clamp16((float)c.b * a), .a = clamp16(a) };
}

cnvs_unpremul cnvs_unpremultiply(cnvs_premul c) {
    float a = (float)c.a;
    if (a <= 0.0f) {
        return (cnvs_unpremul){ .r = 0, .g = 0, .b = 0, .a = 0 };
    }
    return (cnvs_unpremul){ .r = clamp16((float)c.r / a), .g = clamp16((float)c.g / a),
                            .b = clamp16((float)c.b / a), .a = clamp16(a) };
}
