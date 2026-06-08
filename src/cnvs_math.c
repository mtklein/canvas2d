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

cnvs_rgba cnvs_rgba_of(float r, float g, float b, float a) {
    return (cnvs_rgba){ .r = (_Float16)r, .g = (_Float16)g,
                        .b = (_Float16)b, .a = (_Float16)a };
}

float cnvs_srgb_encode(float linear) {
    if (linear <= 0.0f) {
        return 0.0f;
    }
    if (linear >= 1.0f) {
        return 1.0f;
    }
    return linear <= 0.0031308f ? 12.92f * linear
                                : 1.055f * powf(linear, 1.0f / 2.4f) - 0.055f;
}

float cnvs_srgb_decode(float srgb) {
    if (srgb <= 0.0f) {
        return 0.0f;
    }
    if (srgb >= 1.0f) {
        return 1.0f;
    }
    return srgb <= 0.04045f ? srgb / 12.92f
                            : powf((srgb + 0.055f) / 1.055f, 2.4f);
}
