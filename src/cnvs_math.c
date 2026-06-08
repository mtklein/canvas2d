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
