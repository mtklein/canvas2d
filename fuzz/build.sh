#!/bin/sh
# Build the canvas2d fuzz targets with libFuzzer.
#
# Rootless and in-process: libFuzzer needs no shared memory, fork server, or sudo
# (unlike AFL on macOS).  Built with Homebrew clang -- which ships the libFuzzer
# runtime and SanitizerCoverage -- plus ASan + UBSan, WITHOUT -fbounds-safety
# (Apple-clang only; the annotations vanish via fuzz/shim/ptrcheck.h, as in the
# `unsafe` variant).  ASan is the oracle, including the temporal classes
# (use-after-free / -scope / -return).
#
# Three harnesses:
#   fuzz_api    -- broad public-API state machine (spatial + value classes).
#   fuzz_state  -- focused temporal stress: save/restore/clip/font ownership churn.
#   fuzz_text   -- the unchecked Core Text shim (utf8_next, glyph outlines) on
#                  adversarial UTF-8; ASan is the only net in that TU.
#
# To confirm a crasher traps under the feature, replay it against the Apple-clang
# -fbounds-safety build -- see fuzz/README.md.

set -eu
cd "$(dirname "$0")/.."
BUILD=build/fuzz
mkdir -p "$BUILD/obj" fuzz/seeds

LLVM="$(brew --prefix llvm 2>/dev/null || echo /opt/homebrew/opt/llvm)"
CC="${CC:-$LLVM/bin/clang}"
SDKROOT="$(xcrun --show-sdk-path)"

# fuzzer-no-link instruments every TU for coverage; the libFuzzer driver is pulled
# only at the final link.  The use-after-* flags widen ASan's temporal coverage
# (matching the project's debug variant).  -fno-sanitize-recover=all so a UBSan
# finding aborts and libFuzzer records it.
SAN_COMMON="-fsanitize=address,undefined -fno-sanitize-recover=all \
            -fsanitize-address-use-after-scope -fsanitize-address-use-after-return=always"
COMPILE_SAN="-fsanitize=fuzzer-no-link $SAN_COMMON"
LINK_SAN="-fsanitize=fuzzer $SAN_COMMON"
CFLAGS="-std=c23 -g -O1 -fno-omit-frame-pointer -isysroot $SDKROOT \
        -Ifuzz/shim -Ifuzz -Iinclude -Isrc -Wall -Wno-unknown-warning-option"

# CPU backend only (no Metal/ObjC); pixvm is excluded (unreferenced here, and
# under active development on another branch).
CORE="canvas cnvs_cover cnvs_font_ct cnvs_geom cnvs_gradient cnvs_image \
      cnvs_math cnvs_mem cnvs_path cnvs_stroke cnvs_png compositor_cpu"
FRAMEWORKS="-framework CoreText -framework CoreGraphics -framework CoreFoundation"

echo "[fuzz] compiling core with $CC (libFuzzer + ASan + UBSan, rootless)"
OBJS=""
for s in $CORE; do
    "$CC" $CFLAGS $COMPILE_SAN -c "src/$s.c" -o "$BUILD/obj/$s.o"
    OBJS="$OBJS $BUILD/obj/$s.o"
done

for h in fuzz_api fuzz_state fuzz_text; do
    echo "[fuzz] linking harness $h"
    "$CC" $CFLAGS $COMPILE_SAN -DFUZZ_NO_MAIN -c "fuzz/$h.c" -o "$BUILD/obj/$h.o"
    "$CC" $LINK_SAN -isysroot "$SDKROOT" $OBJS "$BUILD/obj/$h.o" $FRAMEWORKS -o "$BUILD/$h"
done

echo "[fuzz] building seed generator + corpus"
cc -std=c23 -O2 -Ifuzz fuzz/seed_gen.c -o "$BUILD/seed_gen"
"$BUILD/seed_gen" fuzz/seeds

# Scratch corpus dirs (gitignored under build/): libFuzzer writes discoveries to
# its FIRST corpus arg, so point it here and pass the committed seeds read-only --
# keeps fuzz/seeds_text/ and fuzz/seeds/ pristine.
mkdir -p "$BUILD/corpus_text" "$BUILD/corpus"

echo "[fuzz] built fuzz_api, fuzz_state, fuzz_text in $BUILD (libFuzzer, rootless). run e.g.:"
echo "  ./$BUILD/fuzz_text  -max_len=512  $BUILD/corpus_text fuzz/seeds_text  # unchecked Core Text shim"
echo "  ./$BUILD/fuzz_state -max_len=4096 $BUILD/corpus      fuzz/seeds       # temporal stress"
echo "  ./$BUILD/fuzz_api   -max_len=4096 $BUILD/corpus      fuzz/seeds       # broad API; Ctrl-C to stop"
echo "  ./$BUILD/<harness>  <crash-file>                                     # reproduce one crash"
