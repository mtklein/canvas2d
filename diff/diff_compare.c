// Backend differential comparator.  Reads the raw RGBA8 dumps written by
// diff_render for two backends and reports the per-channel divergence; exits
// non-zero if the worst delta exceeds the tolerance.  Backend-agnostic -- it
// touches no canvas code, just the dump files.
//
// Each dump is [int32 w][int32 h][w*h*4 bytes RGBA8].  The report prints, per
// scene, the max |delta| over all channels, how many channels differ at all, and
// the worst pixel.  For the "modes" montage a worst pixel at (x,y) lives in grid
// cell (x/56, y/56) -> globalCompositeOperation (row*6 + col).
//
// Usage: diff_compare <dirA> <dirB> [tolerance]   (tolerance defaults to 0 = exact)

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long read_file(char const *path, uint8_t **out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
    long got = buf ? (long)fread(buf, 1, (size_t)n, f) : -1;
    fclose(f);
    *out = buf;
    return got;
}

static int32_t rd_i32(uint8_t const *p) {
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: diff_compare <dirA> <dirB> [tolerance]\n");
        return 2;
    }
    char const *da = argv[1], *db = argv[2];
    int tol = argc > 3 ? atoi(argv[3]) : 0;

    DIR *d = opendir(da);
    if (!d) {
        fprintf(stderr, "diff_compare: cannot open %s\n", da);
        return 2;
    }

    int global_max = 0, hard_fail = 0, scenes = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char const *name = e->d_name;
        size_t nl = strlen(name);
        if (nl < 6 || strcmp(name + nl - 5, ".rgba") != 0) {
            continue;
        }
        char pa[1024], pb[1024];
        (void)snprintf(pa, sizeof pa, "%s/%s", da, name);
        (void)snprintf(pb, sizeof pb, "%s/%s", db, name);

        uint8_t *ba = NULL, *bb = NULL;
        long na = read_file(pa, &ba), nb = read_file(pb, &bb);
        if (na < 8 || nb < 8) {
            printf("  %-12s READ FAILED\n", name);
            hard_fail = 1;
            free(ba);
            free(bb);
            continue;
        }
        int32_t w = rd_i32(ba), ha = rd_i32(ba + 4);
        int32_t wb = rd_i32(bb), hb = rd_i32(bb + 4);
        long bytes = (long)w * ha * 4;
        if (w != wb || ha != hb || na < 8 + bytes || nb < 8 + bytes) {
            printf("  %-12s DIMENSION/SIZE MISMATCH (%dx%d vs %dx%d)\n",
                   name, w, ha, wb, hb);
            hard_fail = 1;
            free(ba);
            free(bb);
            continue;
        }

        uint8_t const *xa = ba + 8, *xb = bb + 8;
        int smax = 0, wx = 0, wy = 0, wc = 0;
        long ndiff = 0;
        for (long i = 0; i < bytes; i++) {
            int dd = xa[i] - xb[i];
            if (dd < 0) {
                dd = -dd;
            }
            if (dd > 0) {
                ndiff++;
            }
            if (dd > smax) {
                smax = dd;
                long pix = i / 4;
                wx = (int)(pix % w);
                wy = (int)(pix / w);
                wc = (int)(i % 4);
            }
        }
        char const *chan = "RGBA";
        printf("  %-10s %3dx%-3d  maxd=%d  channels_differing=%ld/%ld  worst=(%d,%d %c)\n",
               name, w, ha, smax, ndiff, bytes, wx, wy, chan[wc]);
        if (smax > global_max) {
            global_max = smax;
        }
        scenes++;
        free(ba);
        free(bb);
    }
    closedir(d);

    printf("global maxd = %d over %d scene(s)  (tolerance %d)\n", global_max, scenes, tol);
    if (hard_fail || global_max > tol) {
        printf("FAIL: software backend diverges from Metal beyond tolerance\n");
        return 1;
    }
    return 0;
}
