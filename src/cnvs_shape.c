#include "cnvs_shape.h"

// Checked-core consumers of a shaped run.  Every glyph and cluster access is
// bounds-checked against the counts the boundary handed over; the cluster value is
// range-checked against the source length before it is trusted as an index.

float cnvs_shaped_width(cnvs_shaped const *__single s) {
    if (!s) {
        return 0.0f;
    }
    float w = 0.0f;
    for (int r = 0; r < s->nruns; r++) {
        cnvs_glyph_run run = s->run[r];
        for (int i = 0; i < run.count; i++) {
            w += run.xadv[i];
        }
    }
    return w;
}

int cnvs_shaped_index_at_x(cnvs_shaped const *__single s, float x) {
    if (!s) {
        return -1;
    }
    float pen = 0.0f;
    for (int r = 0; r < s->nruns; r++) {
        cnvs_glyph_run run = s->run[r];  // glyphs are visual-order, so a single
        for (int i = 0; i < run.count; i++) {  // left-to-right sweep works for RTL too
            if (x < pen + run.xadv[i]) {
                int32_t c = run.cluster[i];
                if (c < 0 || c >= s->text_len) {  // defensive: a bad cluster is not
                    return -1;                    // a valid source index
                }
                return c;
            }
            pen += run.xadv[i];
        }
    }
    return -1;
}
