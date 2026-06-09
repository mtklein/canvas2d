#!/bin/sh
# Build the canvas2d API fuzz target (Role A).
#
# Discovery build: afl-clang-fast (Homebrew clang) + ASan + UBSan, CPU backend,
# WITHOUT -fbounds-safety (that flag is Apple-clang-only; the bounds annotations
# expand to nothing via the fuzz/shim/ptrcheck.h stub, exactly as in the
# `unsafe` variant).  AFL coverage instrumentation + the sanitizers are the
# oracle: ASan catches the OOB the feature would otherwise trap, UBSan catches
# the integer/float-cast UB that precedes a bad bound.
#
# Then build the seed generator and emit the seed corpus.
#
# Confirmation (separate): replay a crasher under the Apple-clang -fbounds-safety
# build to show the trap -- see fuzz/README.md.

set -eu
cd "$(dirname "$0")/.."
BUILD=build/fuzz
mkdir -p "$BUILD/obj" fuzz/seeds

# CPU backend only (no Metal/ObjC); pixvm_switch.c is excluded (unreferenced,
# and under active development on another branch).
CORE="canvas cnvs_cover cnvs_font_ct cnvs_geom cnvs_gradient cnvs_image \
      cnvs_math cnvs_mem cnvs_path cnvs_png cnvs_stroke compositor_cpu"
FRAMEWORKS="-framework CoreText -framework CoreGraphics -framework CoreFoundation"

CC="${CC:-afl-clang-fast}"
# Homebrew clang doesn't auto-locate the macOS SDK (CoreText etc. for the font
# shim); point it at the active SDK explicitly.
SDKROOT="$(xcrun --show-sdk-path)"
export AFL_USE_ASAN=1 AFL_USE_UBSAN=1 AFL_QUIET=1
CFLAGS="-std=c23 -g -O1 -fno-omit-frame-pointer -isysroot $SDKROOT \
        -Ifuzz/shim -Ifuzz -Iinclude -Isrc \
        -Wall -Wno-unknown-warning-option"

echo "[fuzz] compiling core + harness with $CC (ASan+UBSan, no -fbounds-safety)"
OBJS=""
for s in $CORE; do
    $CC $CFLAGS -c "src/$s.c" -o "$BUILD/obj/$s.o"
    OBJS="$OBJS $BUILD/obj/$s.o"
done
$CC $CFLAGS -c fuzz/fuzz_api.c -o "$BUILD/obj/fuzz_api.o"
$CC $CFLAGS $OBJS "$BUILD/obj/fuzz_api.o" $FRAMEWORKS -o "$BUILD/fuzz_api"

echo "[fuzz] building seed generator + corpus"
cc -std=c23 -O2 -Ifuzz fuzz/seed_gen.c -o "$BUILD/seed_gen"
"$BUILD/seed_gen" fuzz/seeds

echo "[fuzz] built $BUILD/fuzz_api and $(ls fuzz/seeds | wc -l | tr -d ' ') seeds"
echo "[fuzz] fuzz:   afl-fuzz -i fuzz/seeds -o $BUILD/findings -- ./$BUILD/fuzz_api @@"
echo "[fuzz] replay: ./$BUILD/fuzz_api <file>...   (also runs a corpus as a sanitizer check)"
