#!/bin/sh
# Build the canvas2d API fuzz target (Role A) with libFuzzer.
#
# Rootless and in-process: libFuzzer needs no shared memory, fork server, or
# sudo (unlike AFL on macOS).  Built with Homebrew clang -- which ships the
# libFuzzer runtime and SanitizerCoverage -- plus ASan + UBSan in *diagnostic*
# mode, on the CPU backend, WITHOUT -fbounds-safety (that flag is Apple-clang
# only; the bounds annotations vanish via fuzz/shim/ptrcheck.h, as in the
# `unsafe` variant).  The sanitizers are the oracle: ASan catches the OOB the
# feature would otherwise trap, UBSan catches the integer/float-cast UB that
# precedes a bad bound.
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

# Standard libFuzzer split: instrument every TU for coverage (fuzzer-no-link),
# pull the libFuzzer driver only at the final link (fuzzer).  -fno-sanitize-
# recover=all makes a UBSan finding abort so libFuzzer records it as a crash.
COMPILE_SAN="-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all"
LINK_SAN="-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all"
CFLAGS="-std=c23 -g -O1 -fno-omit-frame-pointer -isysroot $SDKROOT \
        -Ifuzz/shim -Ifuzz -Iinclude -Isrc -Wall -Wno-unknown-warning-option"

# CPU backend only (no Metal/ObjC); pixvm_switch.c is excluded (unreferenced,
# and under active development on another branch).
CORE="canvas cnvs_cover cnvs_font_ct cnvs_geom cnvs_gradient cnvs_image \
      cnvs_math cnvs_mem cnvs_path cnvs_stroke cnvs_png compositor_cpu"
FRAMEWORKS="-framework CoreText -framework CoreGraphics -framework CoreFoundation"

echo "[fuzz] compiling core + harness with $CC (libFuzzer + ASan + UBSan, rootless)"
OBJS=""
for s in $CORE; do
    "$CC" $CFLAGS $COMPILE_SAN -c "src/$s.c" -o "$BUILD/obj/$s.o"
    OBJS="$OBJS $BUILD/obj/$s.o"
done
# FUZZ_NO_MAIN: libFuzzer supplies main(); the harness's file-replay main is for
# the Apple-clang replay build only.
"$CC" $CFLAGS $COMPILE_SAN -DFUZZ_NO_MAIN -c fuzz/fuzz_api.c -o "$BUILD/obj/fuzz_api.o"
"$CC" $LINK_SAN -isysroot "$SDKROOT" $OBJS "$BUILD/obj/fuzz_api.o" $FRAMEWORKS -o "$BUILD/fuzz_api"

echo "[fuzz] building seed generator + corpus"
cc -std=c23 -O2 -Ifuzz fuzz/seed_gen.c -o "$BUILD/seed_gen"
"$BUILD/seed_gen" fuzz/seeds

echo "[fuzz] built $BUILD/fuzz_api (libFuzzer, rootless). run:"
echo "  ./$BUILD/fuzz_api -max_len=4096 fuzz/seeds               # fuzz (Ctrl-C to stop)"
echo "  ./$BUILD/fuzz_api -runs=500000 -max_len=4096 fuzz/seeds  # bounded run"
echo "  ./$BUILD/fuzz_api <crash-file>                           # reproduce one crash"
