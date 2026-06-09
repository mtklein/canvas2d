# canvas2d fuzzing (Role A: API state-machine fuzzer)

A coverage-guided fuzz target for the public `canvas_*` API. The input is decoded
by a **total** byte→op stream (`fuzz_api.c`): every byte string is a valid
program (opcodes/counts taken modulo their range; a short read ends the program),
so the fuzzer drives deep into the renderer instead of bouncing off validation.

## Why this shape

`-fbounds-safety` is Apple-clang-only; AFL++'s compiler is Homebrew clang and
rejects the flag. So the duties split:

- **Discovery** — `afl-clang-fast` + ASan + UBSan, CPU backend, *without*
  `-fbounds-safety` (the annotations vanish via `shim/ptrcheck.h`, as in the
  `unsafe` variant). ASan catches the OOB the feature would trap; UBSan catches
  the integer/float-cast UB that precedes a bad bound.
- **Confirmation** — replay a crasher under the Apple-clang `-fbounds-safety`
  build to show the would-be corruption becomes a trap.

Coordinates are decoded as raw 4-byte floats, so NaN/Inf/huge values reach the
path math and the `(int)` casts — the float-cast UB class. Text ops are excluded
(Core Text shim is a separate target). Image dimensions are clamped small so the
fuzzer probes clip/clamp logic without OOMing on multi-GB allocations.

## Build

```sh
sh fuzz/build.sh         # builds build/fuzz/fuzz_api (+ seeds in fuzz/seeds)
```

## Run

```sh
afl-fuzz -i fuzz/seeds -o build/fuzz/findings -m none -- ./build/fuzz/fuzz_api @@
```

`-m none` is required (ASan reserves huge virtual memory).

### macOS prerequisite (one-time, needs sudo)

macOS ships tiny System V shared-memory limits, so AFL's coverage map fails
`shmget()` ("SYSTEM ERROR : shmget() failed"). Run once:

```sh
sudo afl-system-config        # raises kern.sysv.shm* (and disables some throttles)
```

Without it, fall back to **coverage-less** fuzzing under the same oracle — feed
random/mutated inputs straight to the harness (this is how Finding 3 surfaced):

```sh
for i in $(seq 1 1000); do head -c $((RANDOM%400+16)) /dev/urandom > /tmp/in$i; done
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    ./build/fuzz/fuzz_api /tmp/in*
```

## Triage

`afl-clang-fast` compiles UBSan in **trap mode** (a crasher is just `SIGTRAP`,
exit 133, no message). To get the file:line, replay the crasher against the
Apple-clang `debug-cpu` objects (full diagnostic ASan+UBSan):

```sh
python3 configure.py && ninja build/debug-cpu/test_png   # builds debug-cpu core objects
cc -std=c23 -O0 -g -fsanitize=address,integer,undefined -fno-sanitize-recover=all \
   -Iinclude -Isrc -c fuzz/fuzz_api.c -o /tmp/h.o
cc -fsanitize=address,integer,undefined -fno-sanitize-recover=all /tmp/h.o \
   build/debug-cpu/obj/{canvas,cnvs_cover,cnvs_font_ct,cnvs_geom,cnvs_gradient,cnvs_image,cnvs_math,cnvs_mem,cnvs_path,cnvs_png,cnvs_stroke,compositor_cpu}.o \
   -framework CoreText -framework CoreGraphics -framework CoreFoundation -o /tmp/fuzz_diag
UBSAN_OPTIONS=print_stacktrace=1 ./tmp/fuzz_diag <crasher>
```

## Confirm the feature traps (differential)

Replay the same crasher against an Apple-clang `-fbounds-safety` build to confirm
the spatial guarantee converts it to a deterministic abort. (Float-cast UB like
Finding 3 is *not* spatial — it shows up as UBSan in debug, and is unaffected by
`-fbounds-safety` in release; the differential matters for the OOB-write classes.)

## Files

- `fuzz_ops.h` — opcode enum, shared by harness and seed generator.
- `fuzz_api.c` — the harness (`LLVMFuzzerTestOneInput`) + a file-replay `main`.
- `seed_gen.c` — emits a seed corpus of real drawing programs.
- `shim/ptrcheck.h` — no-op bounds-safety macros, fuzz build only.
- `build.sh` — builds the discovery target + seeds.

Not yet done: persistent mode (`__AFL_LOOP`) for speed; PNG-encoder and Core-Text
harnesses; Role B (a strict on-disk format with a bounds-safe validating parser).
