# canvas2d fuzzing (Role A: API state-machine fuzzer)

A coverage-guided fuzz target for the public `canvas_*` API. The input is decoded
by a **total** byte→op stream (`fuzz_api.c`): every byte string is a valid
program (opcodes/counts taken modulo their range; a short read ends the program),
so the fuzzer drives deep into the renderer instead of bouncing off validation.

## Engine: libFuzzer (rootless, in-process)

We use **libFuzzer**, not AFL: it runs in-process with no shared memory, fork
server, or `sudo` (AFL on macOS needs `sudo afl-system-config` to raise the SysV
shm limits). libFuzzer's runtime ships with Homebrew clang.

`-fbounds-safety` is Apple-clang-only, and Homebrew clang doesn't have it, so the
duties split:

- **Discovery** — Homebrew clang + libFuzzer + ASan + UBSan, CPU backend,
  *without* `-fbounds-safety` (the annotations vanish via `shim/ptrcheck.h`, as in
  the `unsafe` variant). ASan catches the OOB the feature would trap; UBSan
  catches the integer/float-cast UB that precedes a bad bound. Diagnostics print
  directly (file:line) — no trap-mode triage needed.
- **Confirmation** — replay a crasher under the Apple-clang `-fbounds-safety`
  build to show a would-be OOB write becomes a deterministic trap.

Coordinates decode as raw 4-byte floats, so NaN/Inf/huge values reach the path
math and the `(int)` casts — the float-cast UB class. Text ops are excluded (Core
Text shim is a separate target). Image dimensions are clamped small so the fuzzer
probes clip/clamp logic without OOMing on multi-GB allocations.

## Build

```sh
sh fuzz/build.sh         # -> build/fuzz/fuzz_api (libFuzzer), seeds in fuzz/seeds
```

Requires `brew install llvm` (provides clang + the libFuzzer runtime). No root.

## Run

```sh
./build/fuzz/fuzz_api -max_len=4096 fuzz/seeds                # fuzz; Ctrl-C to stop
./build/fuzz/fuzz_api -runs=500000 -max_len=4096 fuzz/seeds   # bounded run
./build/fuzz/fuzz_api <crash-file>                            # reproduce one crash
./build/fuzz/fuzz_api -merge=1 fuzz/seeds build/fuzz/cmin     # corpus minimization
```

A crash is written as `crash-<hash>`; ASan/UBSan print the file:line and stack
inline (diagnostic mode). Minimize with `-minimize_crash=1 <crash-file>`.

## Confirm the feature traps (differential)

Replay the same crasher against an Apple-clang `-fbounds-safety` build to confirm
the spatial guarantee converts a would-be OOB into a deterministic abort. The
harness's `main` (compiled *without* `-DFUZZ_NO_MAIN`) replays files:

```sh
python3 configure.py && ninja build/release-cpu/test_png   # builds release-cpu core objects
cc -std=c23 -Os -Iinclude -Isrc -c fuzz/fuzz_api.c -o /tmp/h.o      # Apple clang, no -fbounds-safety
cc -Os /tmp/h.o \
   build/release-cpu/obj/{canvas,cnvs_cover,cnvs_font_ct,cnvs_geom,cnvs_gradient,cnvs_image,cnvs_math,cnvs_mem,cnvs_path,cnvs_png,cnvs_stroke,compositor_cpu}.o \
   -framework CoreText -framework CoreGraphics -framework CoreFoundation -o /tmp/fuzz_replay
./tmp/fuzz_replay <crash-file>     # OOB-write classes -> exit 133 (SIGTRAP)
```

Note: float-cast UB (Finding 4) is *not* spatial — it shows up as UBSan in the
diagnostic build and is unaffected by `-fbounds-safety`; the trap differential is
for the OOB-write classes.

## Harnesses

- **`fuzz_api.c`** — broad public-API state machine (spatial + value classes);
  also the binary the in-`all` `fuzzcorpus` gate replays the committed corpus
  through under ASan.
- **`fuzz_state.c`** — focused *temporal* stress: a small op set that hammers the
  ownership transfers (deep save/restore nesting, `clip()` mask alloc/copy/free,
  the font-cache destroy-then-recreate, image-data buffers), so a coverage fuzzer
  reaches the interprocedural lifetime paths the static analyzer can't follow.
  ASan use-after-free / -scope / -return is the oracle.

The fuzz build enables `-fsanitize-address-use-after-scope` and
`-fsanitize-address-use-after-return=always`, matching the debug variant.

## Files

- `fuzz_ops.h` — opcode enum, shared by `fuzz_api` and the seed generator.
- `fuzz_api.c`, `fuzz_state.c` — the harnesses (`LLVMFuzzerTestOneInput` + a
  file-replay `main` behind `#ifndef FUZZ_NO_MAIN`).
- `seed_gen.c` — emits a seed corpus of real drawing programs.
- `shim/ptrcheck.h` — no-op bounds-safety macros for the non-Apple-clang build.
- `build.sh` — builds both libFuzzer targets + seeds.

Not yet done: PNG-encoder and Core-Text harnesses; Role B (a strict on-disk
format with a bounds-safe validating parser, a fuzz target in its own right);
adding `fuzz_state` to the committed-corpus replay gate (coordinate with
`fuzzcorpus`, which currently replays `fuzz_api` only).
