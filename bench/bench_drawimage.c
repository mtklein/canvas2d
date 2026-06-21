// Isolated benchmark: bilinear image sampling -- the SAMP_BILINEAR u8 path
// (sample_src / draw_image_quad's 8-wide fold), the rasterization.md #1 pole that
// no other bench reached.  A non-flat source bitmap is drawn many times per frame
// at non-integer scale and offset, so every output pixel is a true four-tap
// bilinear blend (not an aligned passthrough), at the default smoothing quality
// (bilinear, no mips -- the minify-trilinear/magnify-cubic tiers need MEDIUM/HIGH).
// Profile with `sample`; price with `ninja benchcmp` (release vs unsafe).
#include "bench_reps.h"

#include "canvas2d.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define DIM 512
#define SRC 64
#define DRAWS 40
#define FRAMES 20

int main(void) {
    struct canvas2d_context *__single cv = canvas2d(DIM, DIM, CANVAS2D_CS_SRGB);
    int const slen = SRC * SRC * 4;
    uint8_t *src = malloc((size_t)slen);
    int const len = DIM * DIM * 4;
    uint8_t *px = malloc((size_t)len);
    if (!cv || !src || !px) {
        free(px);
        free(src);
        canvas2d_free(cv);
        return 1;
    }
    // A varied source so adjacent taps differ (otherwise bilinear is a copy).
    for (int y = 0; y < SRC; y++) {
        for (int x = 0; x < SRC; x++) {
            int const o = (y * SRC + x) * 4;
            src[o + 0] = (uint8_t)(x * 4);
            src[o + 1] = (uint8_t)(y * 4);
            src[o + 2] = (uint8_t)(((unsigned)x ^ (unsigned)y) * 3u);
            src[o + 3] = 255;
        }
    }

    double sink = 0.0;
    int const reps = bench_reps();
    double const t0 = bench_now_s();
    for (int rep = 0; rep < reps; rep++) {
        for (int f = 0; f < FRAMES; f++) {
            for (int i = 0; i < DRAWS; i++) {
                float const s = 0.9f + 0.02f * (float)((i + f) % 16);   // ~0.9..1.2x
                float const dx = (float)((i * 37 + f * 11) % (DIM - 80));
                float const dy = (float)((i * 53 + f * 7) % (DIM - 80));
                canvas2d_draw_bitmap_scaled(cv, CANVAS2D_CS_SRGB, src, SRC, SRC,
                                          dx + 0.3f, dy + 0.7f,    // non-integer offset
                                          (float)SRC * s, (float)SRC * s);
            }
        }
        canvas2d_read_rgba(cv, CANVAS2D_CS_SRGB, px, len);
        sink += (double)px[(DIM / 2 * DIM + DIM / 2) * 4];
    }
    double const secs = bench_now_s() - t0;

    // Output pixels sampled: roughly the source area per draw (scale ~1).
    bench_report_throughput(secs, (double)SRC * (double)SRC * (double)DRAWS *
                                      (double)FRAMES * (double)reps);
    free(px);
    free(src);
    canvas2d_free(cv);
    fprintf(stderr, "sink=%.0f\n", sink);
    return 0;
}
