#!/usr/bin/env python3
"""Generate build.ninja for the canvas2d project.

Build variants (one source tree):

  release      -Os -fbounds-safety              (shipping build)
  debug        -O0 -g -fbounds-safety -fsanitize=address,integer,undefined
  unsafe       -Os                              (release minus -fbounds-safety)
  release-cpu  release with the software compositor backend
  debug-cpu    debug with the software compositor backend

`release` vs `unsafe` isolates the cost of -fbounds-safety: same sources and
optimisation, only the flag differs. `ninja benchcmp` runs hyperfine over the two.

The C core is built -std=c23 -Werror -Weverything (plus -fbounds-safety for
release/debug); the few disabled warnings below are each justified.

Two source files are platform boundaries, built -Wall -Wextra without
-fbounds-safety (still under the debug sanitizers): the Objective-C Metal shim
(src/*.m), since the flag is C-only, and the Core Text font shim (BOUNDARY_C),
which binds un-annotated system headers. Both sit behind a bounds-safe C ABI; see
docs/bounds-safety.md. The shader is embedded with C23 #embed (--embed-dir=shaders);
-MMD tracks the dependency.
"""

import os
import glob
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))


def rel(p):
    return os.path.relpath(p, HERE)


def homebrew_clang():
    """Resolve Homebrew clang + the macOS SDK for the libFuzzer fuzz targets, or
    None if Homebrew llvm isn't installed.

    The fuzz targets are the one corner of the build that does NOT use Apple clang:
    Apple clang can't link the libFuzzer runtime (it ships only with Homebrew
    clang), and the fuzz build drops -fbounds-safety (Apple-clang-only) the same way
    the `unsafe` variant does -- the annotations vanish via fuzz/shim/ptrcheck.h.
    Homebrew clang needs -isysroot pointed at the SDK explicitly (Apple clang finds
    it implicitly).  When llvm isn't installed we return None and the `fuzz` target
    is simply not emitted, so a bare `ninja` is unaffected (`ninja fuzz` needs
    `brew install llvm`).  $CC overrides the compiler."""
    cc = os.environ.get("CC")
    if not cc:
        try:
            cc = os.path.join(subprocess.run(
                ["brew", "--prefix", "llvm"], capture_output=True, text=True,
                check=True).stdout.strip(), "bin", "clang")
        except (OSError, subprocess.CalledProcessError):
            cc = "/opt/homebrew/opt/llvm/bin/clang"
    if not os.path.exists(cc):
        return None
    try:
        sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True,
                             text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None
    return cc, sdk


CSTD = "-std=c23"
BOUNDS = "-fbounds-safety"

# Disabled warnings.  Keep this list short and justified.
CWARN_DISABLED = [
    # /usr/local/include sits on clang's default search path on this machine;
    # this is a cross-compilation hygiene warning, irrelevant to a native build.
    ("poison-system-directories", "spurious for a native (non-cross) build"),
    # We declare __counted_by locals at their point of use; C89-style
    # "declare everything up top" is antithetical to modern C23.
    ("declaration-after-statement", "we use C23 declare-at-use style"),
    # Struct tail padding is not a correctness signal for this code.
    ("padded", "padding is not a correctness concern here"),
    # We target C23; warning that C23 features are 'incompatible with older C'
    # is backwards for this project.
    ("pre-c23-compat", "we deliberately target C23"),
    # This warning only exists to keep code compilable as C++.  This project is
    # C-only, where implicit void*<->T* conversion is idiomatic (the calloc/
    # realloc idiom); it does NOT weaken -fbounds-safety's runtime size checks.
    ("implicit-void-ptr-cast", "C-only project; idiomatic void* conversion"),
    # We prefer *exhaustive* enum switches with no default: -Wswitch-enum (kept)
    # makes the compiler enforce that every case is handled, so a default is dead
    # weight that -Wcovered-switch-default (kept) would flag anyway.  Requiring one
    # only collides with that.
    ("switch-default", "we write exhaustive enum switches; -Wswitch-enum guards them"),
]

CWARN = "-Werror -Weverything " + " ".join(
    "-Wno-" + name for name, _why in CWARN_DISABLED
)

CINC = "-Iinclude -Isrc"
OBJCWARN = "-Wall -Wextra"

# `ninja analyze` runs the Clang Static Analyzer over the checked C: path-sensitive
# use-after-free / double-free / leak detection (the unix.Malloc checker), the
# nearest thing to *static* temporal-safety checking to complement -fbounds-safety's
# spatial guarantee (Clang has no temporal equivalent for C -- its lifetime analysis
# is C++-only; see docs/bounds-safety.md).  -analyzer-werror gates the build on any
# finding; the dead-store style checker is dropped (not a memory-safety signal).
# In `all` (a bare `ninja` runs everything): the scope is kept to memory-safety
# checkers so the path-sensitive analyzer is unlikely to false-positive and break
# the build; widen it only if that holds up.
ANALYZE = ("--analyze -Xclang -analyzer-output=text -Xclang -analyzer-werror "
           "-Xclang -analyzer-disable-checker -Xclang deadcode.DeadStores")

# Frameworks every variant needs (the Core Text font shim).
BASE_FRAMEWORKS = "-framework CoreText -framework CoreGraphics -framework CoreFoundation"

# Mutually-exclusive compositor backends; a binary links exactly one.  metal is the
# ObjC GPU shim, cpu is the software compositor (no frameworks).  backend -> (source,
# extra link frameworks).
BACKENDS = {
    "metal": ("src/compositor_metal.m", "-framework Metal -framework Foundation"),
    "cpu":   ("src/compositor_cpu.c", ""),
}
BACKEND_SRCS = {os.path.basename(src) for src, _fw in BACKENDS.values()}

# Platform-boundary C sources: built without -fbounds-safety at -Wall -Wextra (like
# the .m shim) because they bind un-annotated system headers, behind a bounds-safe ABI.
BOUNDARY_C = {"cnvs_text_ct.c"}

# Benches that drive the full canvas API through the compositor (not isolated
# kernels), so they're worth building on *both* compositor backends -- `ninja
# rendercmp` compares metal vs cpu end-to-end, and both are the shipping path.
PIPELINE_BENCHES = {"bench_render", "bench_render_large"}

# The two -fsanitize-address-use-after-* flags widen ASan's *temporal* coverage
# (stack use-after-scope and use-after-return) -- the class -fbounds-safety
# doesn't address.  detect_leaks is deliberately NOT enabled: LeakSanitizer is
# broken on Apple-Silicon macOS (libobjc false positives); the macOS `leaks` tool
# covers leaks instead (see the leakcheck test).
_DEBUG = ("-O0 -g -fsanitize=address,integer,undefined -fno-sanitize-recover=all "
          "-fsanitize-address-use-after-scope -fsanitize-address-use-after-return=always")

# Redefine the allocator to the fault injector (tests/oom_alloc.c) for the OOM and
# coverage builds.  Invisible to -fbounds-safety: stdlib.h's annotated malloc
# declaration macro-expands onto the wrapper, so size tracking is preserved.
OOM_DEFINES = ("-Dmalloc=cnvs_oom_malloc -Drealloc=cnvs_oom_realloc "
               "-Dcalloc=cnvs_oom_calloc")

# --- libFuzzer fuzz targets (opt-in `ninja fuzz`; see homebrew_clang() above) ----
# fuzzer-no-link instruments every TU for SanitizerCoverage; the libFuzzer driver
# is pulled in only at the final link.  The use-after-{scope,return} flags match the
# debug variant's temporal ASan; -fno-sanitize-recover=all so a UBSan finding aborts
# and libFuzzer records it.  -Wno-unknown-warning-option tolerates Apple-only flags.
FUZZ_SAN_COMMON = ("-fsanitize=address,undefined -fno-sanitize-recover=all "
                   "-fsanitize-address-use-after-scope "
                   "-fsanitize-address-use-after-return=always")
FUZZ_COMPILE_SAN = "-fsanitize=fuzzer-no-link " + FUZZ_SAN_COMMON
FUZZ_LINK_SAN = "-fsanitize=fuzzer " + FUZZ_SAN_COMMON
FUZZ_CFLAGS = ("-std=c23 -g -O1 -fno-omit-frame-pointer -Ifuzz/shim -Ifuzz "
               "-Iinclude -Isrc -Wall -Wno-unknown-warning-option")
# Modules the libFuzzer harnesses do NOT link: the pixvm VM, the colour LUT, and
# the ring buffer aren't reached by any harness.  The fuzz core is otherwise the
# whole canvas render core (core_c, globbed) plus the CPU compositor backend -- so
# a new cnvs_*.c module is picked up automatically.  Kept as an *exclude* set
# because it's small and stable, where the include list grows with every feature
# (and silently drifting out of date is exactly how the old hand-listed CORE rotted).
FUZZ_CORE_EXCLUDE = {"lut.c", "ring.c", "pixvm_pipe.c", "pixvm_switch.c",
                     "pixvm_thread.c"}

# variant -> (opt flags, bounds-safety?, build tests?, build bench?, backend).
# The -cpu variants run the test suite against the software compositor,
# cross-validating the two backends.  Benchmarks measure backend-agnostic CPU
# kernels, so they stay metal.
VARIANTS = {
    "release":     ("-Os", True,  True,  True,  "metal"),
    "debug":       (_DEBUG, True, True,  False, "metal"),
    "unsafe":      ("-Os", False, False, True,  "metal"),
    "release-cpu": ("-Os", True,  True,  False, "cpu"),
    "debug-cpu":   (_DEBUG, True, True,  False, "cpu"),
}


def obj(variant, src):
    return os.path.join("build", variant, "obj",
                        os.path.splitext(os.path.basename(src))[0] + ".o")


def main():
    core_c = sorted(rel(p) for p in glob.glob(os.path.join(HERE, "src", "*.c"))
                    if os.path.basename(p) not in BACKEND_SRCS)
    # test_oom.c is built only by the `oom` target (it needs the fault-injecting
    # allocator and the malloc redefines), not the normal suite.
    tests = sorted(rel(p) for p in glob.glob(os.path.join(HERE, "tests", "test_*.c"))
                   if os.path.basename(p) != "test_oom.c")
    benches = sorted(rel(p) for p in glob.glob(os.path.join(HERE, "bench", "*.c")))
    examples = sorted(rel(p) for p in glob.glob(os.path.join(HERE, "examples", "*.c")))
    # The gallery PNGs are committed build artifacts.  A bare `ninja` re-renders
    # them straight into the tree whenever the gallery binary changes, so a
    # rendering change surfaces as a git diff in lockstep -- review and commit the
    # new PNGs alongside the code.  Declaring them as outputs (input = the binary)
    # makes ninja re-render exactly when the renderer changes, no more.
    gallery_pngs = sorted(rel(p) for p in glob.glob(os.path.join(HERE, "gallery", "*.png")))
    # Committed fuzz regression corpus (distinct from the gitignored fuzz/seeds/
    # scratch).  `ninja` replays every input under the debug-cpu sanitizers, so a
    # crasher -- once reduced and dropped in here -- stays a permanent regression.
    fuzz_corpus = sorted(rel(p) for p in glob.glob(os.path.join(HERE, "fuzz", "corpus", "*.bin")))

    n = []
    w = n.append

    w("# Generated by configure.py -- do not edit by hand.")
    w("ninja_required_version = 1.10")
    w("")
    w(f"cstd = {CSTD}")
    w(f"cwarn = {CWARN}")
    w(f"cinc = {CINC}")
    w(f"objcwarn = {OBJCWARN}")
    w("")
    w("# Rationale for each disabled warning:")
    for name, why in CWARN_DISABLED:
        w(f"#   -Wno-{name}: {why}")
    w("")

    # Self-regeneration: build.ninja depends on this script, so editing
    # configure.py takes effect on the next `ninja` -- no stale-graph builds from
    # forgetting to rerun it by hand.  `generator = 1` marks the output as
    # build-system metadata (ninja won't delete it on interrupt or clean it).
    # New/removed source files still need a manual rerun: the file lists are
    # globbed, and ninja only watches declared inputs.
    w("rule configure")
    w("  command = python3 configure.py")
    w("  generator = 1")
    w("")
    w("build build.ninja: configure configure.py")
    w("")

    for variant, (opt, bounds, _tests, _bench, backend) in VARIANTS.items():
        bflag = (BOUNDS + " ") if bounds else ""
        frameworks = (BASE_FRAMEWORKS + " " + BACKENDS[backend][1]).strip()
        # Flag order targets ninja's status line, which elides the middle: the
        # variant-distinguishing flags lead (clang -fbounds-safety -Os ...), the
        # source/output trail (... -c $in -o $out), and the std/warning/include/
        # depfile boilerplate sits in the elided middle.
        w(f"rule cc_{variant}")
        w(f"  command = clang {bflag}{opt} $cstd $cwarn $cinc -MMD -MF $out.d -c $in -o $out")
        w("  depfile = $out.d")
        w("  deps = gcc")
        w("")
        # Boundary C: no -fbounds-safety, -Wall -Wextra (system-FFI seam), but the
        # debug sanitizers still apply (unlike the ObjC shim).
        w(f"rule cc_boundary_{variant}")
        w(f"  command = clang {opt} $cstd $objcwarn $cinc -MMD -MF $out.d -c $in -o $out")
        w("  depfile = $out.d")
        w("  deps = gcc")
        w("")
        w(f"rule objc_{variant}")
        w(f"  command = clang -fobjc-arc {opt} $cstd $objcwarn $cinc --embed-dir=shaders -MMD -MF $out.d -c $in -o $out")
        w("  depfile = $out.d")
        w("  deps = gcc")
        w("")
        w(f"rule link_{variant}")
        w(f"  command = clang {opt} $in {frameworks} -o $out")
        w("")

    w("rule run")
    w("  command = $bin && touch $out")
    w("")
    # Replay the fuzz corpus through the harness.  Unlike `run`, the replay binary
    # is chatty (one `ok:` per input), so capture its output and surface it only on
    # a finding -- a clean replay stays silent, a crash dumps the ASan/UBSan report.
    w("rule corpus_replay")
    w("  command = $bin $in >$out.log 2>&1 && { touch $out ; rm -f $out.log ; } "
      "|| { cat $out.log >&2 ; rm -f $out.log ; exit 1 ; }")
    w("")
    # Backend differential: render the diff scenes into a scratch dir, then diff
    # the two backends' dumps.  render_diff wipes the dir so a removed scene can't
    # linger; diff_compare is silent within tolerance and prints the per-scene
    # divergence report only when it's exceeded (run it by hand to see the report).
    w("rule render_diff")
    w("  command = rm -rf $dir && mkdir -p $dir && $bin $dir && touch $out")
    w("")
    w("rule diff_compare")
    w("  command = $bin $a $b $tol >$out.log 2>&1 && { touch $out ; rm -f $out.log ; } "
      "|| { cat $out.log >&2 ; rm -f $out.log ; exit 1 ; }")
    w("")
    w("rule benchcmp")
    w("  command = $cmd")
    w("  pool = console")
    w("")
    w("rule profile")
    w("  command = $cmd")
    w("  pool = console")
    w("")
    w("rule profile_scene")
    w("  command = $cmd")
    w("  pool = console")
    w("  description = sample(1): self-time across the whole gallery (real scenes)")
    w("")
    w("rule rendercmp")
    w("  command = $cmd")
    w("  pool = console")
    w("  description = real-pipeline render: metal vs cpu compositor (both shipping)")
    w("")
    w("rule gputime")
    w("  command = $cmd")
    w("  pool = console")
    w("  description = Metal GPU execution time (ns total, us/dispatch)")
    w("")
    w("rule throughput")
    w("  command = $cmd")
    w("  pool = console")
    w("  description = size-normalised render throughput (Mpx/s, ns/px)")
    w("")
    w("rule run_gallery")
    w("  command = $bin")
    w("")
    w("rule analyze")
    w(f"  command = clang {ANALYZE} $cstd $cinc $in && touch $out")
    w("  description = ANALYZE $in")
    w("")
    # `leakcheck` runs the non-ASan release-cpu build under the macOS `leaks` tool
    # (LeakSanitizer is broken on Apple-Silicon macOS).  `leaks` exits non-zero if
    # any allocation is unreachable at exit, so it gates.  The stamp makes it
    # idempotent -- it reruns only when its binary changes -- so it can live in `all`.
    #
    # `leaks` needs task_for_pid to inspect the heap; without the get-task-allow
    # entitlement it can only read read-only memory, prints a "Process is not
    # debuggable" warning, AND misses leaks.  So ad-hoc-codesign a *copy* of the
    # binary (a copy, to leave the build output's mtime untouched) with
    # tests/leaks.entitlements first.  Then capture the report and surface it only on
    # a finding -- a clean run is silent (no leaks summary spam), a real leak prints
    # the full report and fails.
    w("rule leakcheck")
    w("  command = cp $bin $bin.signed && "
      "codesign -s - -f --entitlements tests/leaks.entitlements $bin.signed 2>/dev/null && "
      "leaks --atExit -- $bin.signed >$out.log 2>&1 && { touch $out ; rm -f $out.log $bin.signed ; } "
      "|| { cat $out.log >&2 ; rm -f $out.log $bin.signed ; exit 1 ; }")
    w("")

    # Bench stems, e2e "bench" sorted last; variants that build benches.
    bench_stems = sorted((os.path.splitext(os.path.basename(b))[0] for b in benches),
                         key=lambda s: (s == "bench", s))
    bench_variants = [v for v, cfg in VARIANTS.items() if cfg[3]]

    test_stamps = []
    variant_lib_objs = {}
    for variant, (_opt, _bounds, do_tests, do_bench, backend) in VARIANTS.items():
        lib_objs = []
        for c in core_c:
            o = obj(variant, c)
            ccrule = "cc_boundary" if os.path.basename(c) in BOUNDARY_C else "cc"
            w(f"build {o}: {ccrule}_{variant} {c}")
            lib_objs.append(o)
        # The chosen compositor backend: the ObjC Metal shim or the software compositor.
        bsrc = BACKENDS[backend][0]
        bo = obj(variant, bsrc)
        w(f"build {bo}: {'objc' if bsrc.endswith('.m') else 'cc'}_{variant} {bsrc}")
        lib_objs.append(bo)
        variant_lib_objs[variant] = list(lib_objs)
        w("")

        produced = []
        if do_tests:
            for t in tests:
                stem = os.path.splitext(os.path.basename(t))[0]
                o = obj(variant, t)
                exe = os.path.join("build", variant, stem)
                stamp = exe + ".runok"
                w(f"build {o}: cc_{variant} {t}")
                w(f"build {exe}: link_{variant} {o} {' '.join(lib_objs)}")
                w(f"build {stamp}: run {exe}")
                w(f"  bin = {exe}")
                produced.append(exe)
                test_stamps.append(stamp)
        if do_bench:
            for b in benches:
                stem = os.path.splitext(os.path.basename(b))[0]
                o = obj(variant, b)
                exe = os.path.join("build", variant, stem)
                w(f"build {o}: cc_{variant} {b}")
                w(f"build {exe}: link_{variant} {o} {' '.join(lib_objs)}")
                produced.append(exe)
        elif variant == "release-cpu":
            # Pipeline benches build on the optimized cpu backend too, so rendercmp
            # can pit the two shipping compositors against each other end to end.
            for b in benches:
                stem = os.path.splitext(os.path.basename(b))[0]
                if stem not in PIPELINE_BENCHES:
                    continue
                o = obj(variant, b)
                exe = os.path.join("build", variant, stem)
                w(f"build {o}: cc_{variant} {b}")
                w(f"build {exe}: link_{variant} {o} {' '.join(lib_objs)}")
                produced.append(exe)
        if variant in ("release", "release-cpu"):
            for e in examples:
                stem = os.path.splitext(os.path.basename(e))[0]
                o = obj(variant, e)
                exe = os.path.join("build", variant, stem)
                w(f"build {o}: cc_{variant} {e}")
                w(f"build {exe}: link_{variant} {o} {' '.join(lib_objs)}")
                produced.append(exe)
        w("")
        w(f"build {variant}: phony {' '.join(produced)}")
        w("")

    # All bench executables (every stem in every bench-building variant).
    bench_exes = [f"build/{v}/{s}" for v in bench_variants for s in bench_stems]

    # One hyperfine invocation per phase, comparing variants side by side.
    calls = []
    for s in bench_stems:
        args = " ".join(f'-n "{s} {v}" ./build/{v}/{s}' for v in bench_variants)
        calls.append(f"hyperfine --warmup 3 -N {args}")
    benchcmp_cmd = " ; ".join(calls)

    w(f"build test: phony {' '.join(test_stamps)}")
    w(f"build bench: phony {' '.join(bench_exes)}")
    # `images` renders straight into the committed gallery/*.png.  The PNGs are
    # outputs gated on the gallery binary, so a bare `ninja` keeps them in
    # lockstep with the renderer -- and dirties the tree the moment a code change
    # moves a pixel.  `ninja images` is the same edge on its own.
    w(f"build {' '.join(gallery_pngs)}: run_gallery build/release/gallery")
    w("  bin = ./build/release/gallery")
    w(f"build images: phony {' '.join(gallery_pngs)}")
    # `fuzzcorpus`: replay the committed fuzz corpus through the API harness under
    # the debug-cpu sanitizers (ASan/UBSan, software backend), turning every seed
    # and reduced crasher into a permanent, libFuzzer-free regression.  fuzz_api.c
    # builds like a boundary file (cc_boundary: no -fbounds-safety, -Wall -Wextra)
    # since it isn't -Weverything clean; it links the bounds-safe debug-cpu core,
    # which keeps that core's -fbounds-safety traps under the same replay.
    if fuzz_corpus:
        fuzz_obj = "build/debug-cpu/obj/fuzz_api.o"
        fuzz_replay = "build/debug-cpu/fuzz_replay"
        fuzz_stamp = "build/debug-cpu/fuzz_corpus.runok"
        w(f"build {fuzz_obj}: cc_boundary_debug-cpu fuzz/fuzz_api.c")
        w(f"build {fuzz_replay}: link_debug-cpu {fuzz_obj} "
          f"{' '.join(variant_lib_objs['debug-cpu'])}")
        w(f"build {fuzz_stamp}: corpus_replay {' '.join(fuzz_corpus)} | {fuzz_replay}")
        w(f"  bin = ./{fuzz_replay}")
        w(f"build fuzzcorpus: phony {fuzz_stamp}")

    # `ninja fuzz`: build the libFuzzer harnesses (opt-in -- needs Homebrew clang;
    # see homebrew_clang()).  This is the whole fuzz build -- previously a
    # standalone shell script, now folded in so `ninja` is the single entry point:
    # native per-TU edges (real incremental + parallel builds, header deps tracked
    # via -MMD), not a serial shell loop.  Harnesses are globbed (fuzz/fuzz_*.c), so
    # adding one needs no edit here.  Each links the fuzz core + libFuzzer + ASan/
    # UBSan (see FUZZ_CORE_EXCLUDE / FUZZ_*_SAN above); the
    # seed generator runs into the gitignored fuzz/seeds/.  Stays opt-in (not in
    # `all`): it's a campaign-prep step needing `brew install llvm`, not a gate --
    # the in-`all` regression is `fuzzcorpus`, which replays the committed corpus
    # under Apple clang above.
    hb = homebrew_clang()
    fuzz_harnesses = sorted(os.path.splitext(os.path.basename(p))[0]
                            for p in glob.glob(os.path.join(HERE, "fuzz", "fuzz_*.c")))
    if hb and fuzz_harnesses:
        fuzz_cc, fuzz_sdk = hb
        w("")
        w(f"fuzzcc = {fuzz_cc}")
        w(f"fuzzsdk = {fuzz_sdk}")
        w("")
        # One compile rule; $fuzzdef is empty for core TUs, -DFUZZ_NO_MAIN for the
        # harnesses (so libFuzzer supplies main, not their file-replay main()).
        w("rule cc_fuzz")
        w(f"  command = $fuzzcc {FUZZ_CFLAGS} {FUZZ_COMPILE_SAN} -isysroot $fuzzsdk "
          "$fuzzdef -MMD -MF $out.d -c $in -o $out")
        w("  depfile = $out.d")
        w("  deps = gcc")
        w("")
        w("rule link_fuzz")
        w(f"  command = $fuzzcc {FUZZ_LINK_SAN} -isysroot $fuzzsdk $in "
          f"{BASE_FRAMEWORKS} -o $out")
        w("")
        # seed_gen is a plain host tool (no sanitizers); it writes seeds into the
        # gitignored fuzz/seeds/, which it does not create -- so mkdir first.
        w("rule cc_seedgen")
        w("  command = cc -std=c23 -O2 -Ifuzz $in -o $out")
        w("")
        w("rule gen_seeds")
        w("  command = mkdir -p fuzz/seeds && $bin fuzz/seeds && touch $out")
        w("")
        # The canvas render core (core_c) minus the unreached subsystems, plus the
        # CPU compositor backend (core_c excludes both backends).
        fuzz_core_srcs = [c for c in core_c
                          if os.path.basename(c) not in FUZZ_CORE_EXCLUDE]
        fuzz_core_srcs.append(BACKENDS["cpu"][0])
        fuzz_core_objs = []
        for c in fuzz_core_srcs:
            stem = os.path.splitext(os.path.basename(c))[0]
            o = f"build/fuzz/obj/{stem}.o"
            w(f"build {o}: cc_fuzz {c}")
            fuzz_core_objs.append(o)
        core_args = " ".join(fuzz_core_objs)
        fuzz_bins = []
        for h in fuzz_harnesses:
            ho = f"build/fuzz/obj/{h}.o"
            w(f"build {ho}: cc_fuzz fuzz/{h}.c")
            w("  fuzzdef = -DFUZZ_NO_MAIN")
            w(f"build build/fuzz/{h}: link_fuzz {core_args} {ho}")
            fuzz_bins.append(f"build/fuzz/{h}")
        w("build build/fuzz/seed_gen: cc_seedgen fuzz/seed_gen.c")
        w("build build/fuzz/seeds.stamp: gen_seeds build/fuzz/seed_gen")
        w("  bin = ./build/fuzz/seed_gen")
        w(f"build fuzz: phony {' '.join(fuzz_bins)} build/fuzz/seeds.stamp")

    # `backenddiff`: render the diff scenes on both backends (release = Metal,
    # release-cpu = software) and assert they agree per channel within a tolerance
    # ratchet.  Geometry/AA/gradient/unpremultiply are shared CPU code, so any delta
    # isolates the compositor (blend math + float->_Float16 rounding).  They now
    # agree *bit-for-bit*: compositor_cpu.c rounds its half stores toward zero to
    # mirror Metal's RGBA16Float store (see to_half_rtz there), so the gate is 0 --
    # any divergence (a regression, or a GPU whose store rounds differently) fails
    # the build.  diff_render/diff_compare build boundary-style (no -fbounds-safety,
    # -Wall -Wextra); the comparator links no core (it only reads the dumps), so
    # link_release just supplies the C runtime.
    BACKEND_DIFF_TOL = 0
    dr_obj = lambda v: f"build/{v}/obj/diff_render.o"
    dr_bin = lambda v: f"build/{v}/diff_render"
    dump = lambda v: f"build/{v}/diffdump"
    dump_stamp = lambda v: f"build/{v}/diffdump.stamp"
    for v in ("release", "release-cpu"):
        w(f"build {dr_obj(v)}: cc_boundary_{v} diff/diff_render.c")
        w(f"build {dr_bin(v)}: link_{v} {dr_obj(v)} {' '.join(variant_lib_objs[v])}")
        w(f"build {dump_stamp(v)}: render_diff | {dr_bin(v)}")
        w(f"  bin = ./{dr_bin(v)}")
        w(f"  dir = {dump(v)}")
    w("build build/diff/obj/diff_compare.o: cc_boundary_release diff/diff_compare.c")
    w("build build/diff/diff_compare: link_release build/diff/obj/diff_compare.o")
    w(f"build build/backend_diff.runok: diff_compare {dump_stamp('release')} "
      f"{dump_stamp('release-cpu')} | build/diff/diff_compare")
    w("  bin = ./build/diff/diff_compare")
    w(f"  a = {dump('release')}")
    w(f"  b = {dump('release-cpu')}")
    w(f"  tol = {BACKEND_DIFF_TOL}")
    w("build backenddiff: phony build/backend_diff.runok")
    # benchcmp names a file that is never created, so ninja always reruns it.
    w(f"build benchcmp: benchcmp {' '.join(bench_exes)}")
    w(f"  cmd = {benchcmp_cmd}")
    # `profile` samples the release benches (metal), then the cpu pipeline bench, in
    # place (no output file, always reruns).
    release_bench_exes = [f"build/release/{s}" for s in bench_stems]
    cpu_pipeline_exes = [f"build/release-cpu/{s}" for s in sorted(PIPELINE_BENCHES)]
    w(f"build profile: profile {' '.join(release_bench_exes + cpu_pipeline_exes)}")
    w("  cmd = sh bench/profile.sh build/release ; sh bench/profile.sh build/release-cpu")
    # `profile-scene` samples the gallery (the real public-API pipeline across every
    # scene) rather than a single micro-bench -- the cpu build, since `sample` is
    # CPU-only.  Loops via GALLERY_REPS so there's enough run time to sample.  No output
    # file, always reruns.
    w("build profile-scene: profile_scene build/release-cpu/gallery")
    w("  cmd = sh bench/profile_scene.sh build/release-cpu/gallery")
    # `rendercmp` pits the two shipping compositor backends against each other on the
    # real-pipeline bench (metal vs cpu); names no output file, so it always reruns.
    # Two shapes: per-frame readback (the getImageData/PNG-export workload, a sync per
    # frame) and one-readback-at-the-end (BENCH_READBACK=end -- the GPU pipelines
    # frames, Metal's strength).  The env-prefixed run drops -N so a shell expands it.
    #
    # The GPU path's per-run time is noisy (scheduler jitter, shared-machine
    # contention), so a low run count gives an unreliable mean -- a single short run
    # once even reported a real speedup as a slowdown.  hyperfine has no run
    # interleaving (it runs all of one command, then the other), so the mitigation is
    # a high fixed run count + extra warmup: enough samples that the mean + its
    # confidence interval are trustworthy.  (benchcmp stays at the default low count:
    # those are CPU kernels with tight variance.)
    # Small canvas / many tiny fills (per-draw overhead dominates -> cpu wins) and a
    # large canvas / few full-canvas fills (millions of pixels per draw -> the GPU's
    # parallelism is meant to cross over ahead of the per-pixel software blend).  Each
    # in both readback shapes.  rc: high run count for the noisy small bench; rcl: the
    # large bench is heavier per run and steadier, so fewer runs keep rendercmp's
    # wall time sane.
    rc = "--warmup 8 --runs 50"
    rcl = "--warmup 3 --runs 20"
    sm, smc = "build/release/bench_render", "build/release-cpu/bench_render"
    lg, lgc = "build/release/bench_render_large", "build/release-cpu/bench_render_large"
    rendercmp_cmd = (
        f'hyperfine {rc} -N -n "small metal (per-frame readback)" ./{sm} '
        f'-n "small cpu (per-frame readback)" ./{smc} ; '
        f'hyperfine {rc} -n "small metal (1 readback)" "BENCH_READBACK=end ./{sm}" '
        f'-n "small cpu (1 readback)" "BENCH_READBACK=end ./{smc}" ; '
        f'hyperfine {rcl} -N -n "large metal (per-frame readback)" ./{lg} '
        f'-n "large cpu (per-frame readback)" ./{lgc} ; '
        f'hyperfine {rcl} -n "large metal (1 readback)" "BENCH_READBACK=end ./{lg}" '
        f'-n "large cpu (1 readback)" "BENCH_READBACK=end ./{lgc}"')
    w(f"build rendercmp: rendercmp {sm} {smc} {lg} {lgc}")
    w(f"  cmd = {rendercmp_cmd}")
    # `gputime` reports the Metal backend's own GPU execution time -- `sample` only
    # sees CPU PCs and is blind to the GPU, so this is the complementary view.  Each
    # render bench under CANVAS_GPU_TIMING prints ns total / dispatch count /
    # us-per-dispatch; we run both the small and large bench in both readback shapes.
    # Per-frame readback gives one dispatch per frame (the GPU per-draw cost); the
    # one-readback-at-the-end shape collapses every frame into a single batched
    # dispatch (maximally pipelined).  Metal-only; the cpu backend reports nothing.
    gputime_cmd = (
        f'echo "# small, per-frame readback" ; CANVAS_GPU_TIMING=1 ./{sm} ; '
        f'echo "# small, 1 readback (batched)" ; CANVAS_GPU_TIMING=1 BENCH_READBACK=end ./{sm} ; '
        f'echo "# large, per-frame readback" ; CANVAS_GPU_TIMING=1 ./{lg} ; '
        f'echo "# large, 1 readback (batched)" ; CANVAS_GPU_TIMING=1 BENCH_READBACK=end ./{lg}')
    w(f"build gputime: gputime {sm} {lg}")
    w(f"  cmd = {gputime_cmd}")
    # `throughput` normalises wall time to pixels: each render bench self-times its rep
    # loop (BENCH_THROUGHPUT) and reports Mpx/s + ns/px over the finished-frame pixels
    # it produced.  Unlike rendercmp's raw wall-clock (which scales with the scene), a
    # per-pixel rate is comparable across canvas sizes and between backends -- the
    # apples-to-apples answer to "how many pixels/second does this pipeline push".  A
    # high BENCH_REPS amortises the cold first rep; both benches, both backends, in the
    # per-frame-readback shape (the getImageData/PNG-export workload).  hyperfine still
    # owns the rigorous wall-clock A/B in rendercmp; this is the normalised companion.
    tp_sm, tp_lg = "BENCH_THROUGHPUT=1 BENCH_REPS=50", "BENCH_THROUGHPUT=1 BENCH_REPS=20"
    throughput_cmd = (
        f'echo "# small metal" ; {tp_sm} ./{sm} ; '
        f'echo "# small cpu"   ; {tp_sm} ./{smc} ; '
        f'echo "# large metal" ; {tp_lg} ./{lg} ; '
        f'echo "# large cpu"   ; {tp_lg} ./{lgc}')
    w(f"build throughput: throughput {sm} {smc} {lg} {lgc}")
    w(f"  cmd = {throughput_cmd}")
    # `analyze` runs the static analyzer over the checked C (core + the cpu backend;
    # the ObjC Metal shim is out of scope).  One stamp per TU so it's incremental
    # and parallel; gated by -analyzer-werror in the rule.
    analyze_srcs = core_c + [BACKENDS["cpu"][0]]
    analyze_stamps = []
    for c in analyze_srcs:
        stem = os.path.splitext(os.path.basename(c))[0]
        stamp = os.path.join("build", "analyze", stem + ".stamp")
        w(f"build {stamp}: analyze {c}")
        analyze_stamps.append(stamp)
    w(f"build analyze: phony {' '.join(analyze_stamps)}")
    # leakcheck stamp regenerates when the release-cpu test_leak binary or the
    # codesign entitlements change.
    w("build build/release-cpu/test_leak.leakok: leakcheck build/release-cpu/test_leak"
      " | tests/leaks.entitlements")
    w("  bin = ./build/release-cpu/test_leak")
    w("build leakcheck: phony build/release-cpu/test_leak.leakok")

    # `coverage` (opt-in: `ninja coverage`): source-based coverage of the checked
    # C core.  Instrument core + tests with -fprofile-instr-generate
    # -fcoverage-mapping at -O0 (accurate region/line mapping), run every test
    # writing its own .profraw, merge, and print an llvm-cov report over src/.
    # cpu backend (GPU-free, deterministic).  Like benchcmp/profile it's a
    # measurement, not a gate -- always reruns, console output, NOT in `all`.
    # -fcoverage-compilation-dir=. records the compilation directory in the
    # coverage mapping as "." rather than the absolute CWD, so the metadata is
    # checkout-relative: llvm-cov resolves paths against wherever it runs (ninja
    # runs it from the repo root).  Without it, moving or renaming the checkout
    # strands every already-built object's records at the old absolute path, and
    # the `src` filter below silently drops those files from the report.
    COV = "-O0 -fprofile-instr-generate -fcoverage-mapping -fcoverage-compilation-dir=."
    # The coverage core routes through the fault injector ({OOM_DEFINES}), so adding
    # test_oom to the suite below merges its armed-allocation-failure run into the
    # report -- making the realloc-failure guards show as covered instead of dead.
    # With the injector disarmed (every other test), the wrapper is just malloc.
    # -Itests so test_oom finds oom_alloc.h.
    w("rule cc_cov")
    w(f"  command = clang $cstd {BOUNDS} $cwarn $cinc -Itests {OOM_DEFINES} {COV} -MMD -MF $out.d -c $in -o $out")
    w("  depfile = $out.d")
    w("  deps = gcc")
    w("")
    w("rule cc_cov_boundary")
    w(f"  command = clang $cstd $objcwarn $cinc {OOM_DEFINES} {COV} -MMD -MF $out.d -c $in -o $out")
    w("  depfile = $out.d")
    w("  deps = gcc")
    w("")
    # The fault injector itself: instrumented, NOT redefined, NOT checked.  It must
    # hand back exactly what libc hands it, and -fbounds-safety's alloc_size return
    # check traps a non-NULL return whose size is zero -- which is precisely what
    # macOS malloc(0)/calloc(0, n) produce (cnvs_shape callocs 0 runs for an empty
    # string).  Compiled unchecked like the other boundary shims; checked callers
    # still get size tracking from oom_alloc.h's alloc_size declarations.
    w("rule cc_cov_shim")
    w(f"  command = clang $cstd $objcwarn $cinc -Itests {COV} -MMD -MF $out.d -c $in -o $out")
    w("  depfile = $out.d")
    w("  deps = gcc")
    w("")
    w("rule link_cov")
    w(f"  command = clang {COV} $in {BASE_FRAMEWORKS} -o $out")
    w("")
    w("rule cov_run")  # run a test, writing its coverage profile to $out
    w("  command = LLVM_PROFILE_FILE=$out $bin")
    w("")
    # Merge the profiles, write the checked-in Markdown report (docs/coverage.md,
    # browsable on GitHub), and print the human-readable table to the console.
    # The report is scoped to src/ by *excluding* tests/ (-ignore-filename-regex
    # matches the path string as recorded in the coverage mapping), not by the
    # positional source-path filter: that filter resolves to absolute paths, so it
    # cannot match the checkout-relative records -fcoverage-compilation-dir=.
    # writes, and silently reports everything instead of src/.
    cov_scope = "-ignore-filename-regex='(^|/)tests/'"
    w("rule coverage")
    w("  command = xcrun llvm-profdata merge -sparse $in -o $profdata && "
      f"xcrun llvm-cov export -summary-only $mainbin $objargs -instr-profile=$profdata {cov_scope} "
      "| python3 tools/cov_report.py > $out && "
      f"xcrun llvm-cov report $mainbin $objargs -instr-profile=$profdata {cov_scope}")
    w("  pool = console")
    w("")
    cov_lib = []
    for c in core_c:
        o = obj("cov", c)
        ccrule = "cc_cov_boundary" if os.path.basename(c) in BOUNDARY_C else "cc_cov"
        w(f"build {o}: {ccrule} {c}")
        cov_lib.append(o)
    cov_bsrc = BACKENDS["cpu"][0]
    cov_bo = obj("cov", cov_bsrc)
    w(f"build {cov_bo}: cc_cov {cov_bsrc}")
    cov_lib.append(cov_bo)
    # The fault injector is linked into every coverage binary (the redefined core
    # calls it); only test_oom arms it.
    cov_oom_alloc = "build/cov/obj/oom_alloc.o"
    w(f"build {cov_oom_alloc}: cc_cov_shim tests/oom_alloc.c")
    cov_lib.append(cov_oom_alloc)
    cov_raws, cov_exes = [], []
    for t in tests + [os.path.join("tests", "test_oom.c")]:
        stem = os.path.splitext(os.path.basename(t))[0]
        o = obj("cov", t)
        exe = os.path.join("build", "cov", stem)
        raw = os.path.join("build", "cov", "raw", stem + ".profraw")
        w(f"build {o}: cc_cov {t}")
        w(f"build {exe}: link_cov {o} {' '.join(cov_lib)}")
        w(f"build {raw}: cov_run {exe}")
        w(f"  bin = ./{exe}")
        cov_raws.append(raw)
        cov_exes.append(exe)
    # docs/coverage.md is the committed report (the rule's $out); regenerated from
    # the test profiles, so `ninja coverage` refreshes it and a git diff shows any
    # coverage change.  cov_report.py is an input so edits to it rebuild the report.
    w(f"build docs/coverage.md: coverage {' '.join(cov_raws)} "
      f"| {' '.join(cov_exes)} tools/cov_report.py")
    w("  profdata = build/cov/coverage.profdata")
    w(f"  mainbin = {cov_exes[0]}")
    w(f"  objargs = {' '.join('-object ' + e for e in cov_exes[1:])}")
    w("build coverage: phony docs/coverage.md")
    w("")

    # `oom` (fault-injection gate): recompile the core with malloc/realloc/calloc
    # redefined to a fault injector (tests/oom_alloc.c), then test_oom.c sweeps each
    # allocation of a set of canvas ops failing in turn.  Every allocation-failure
    # cleanup path must degrade gracefully -- no crash, no corruption -- which the
    # debug-cpu config's -fbounds-safety + ASan/UBSan enforce.  This reaches the
    # `if (!p) return false` OOM guards that coverage flagged as the dominant
    # untaken branch class; the normal suite never fails an allocation.  The macro
    # redefine is invisible to -fbounds-safety: stdlib.h's malloc declaration (with
    # __sized_by_or_null/alloc_size) macro-expands onto cnvs_oom_malloc, so size
    # tracking is preserved.  cpu backend; folded into `all`.
    w("rule cc_oom")
    w(f"  command = clang $cstd {BOUNDS} {_DEBUG} $cwarn $cinc {OOM_DEFINES} -MMD -MF $out.d -c $in -o $out")
    w("  depfile = $out.d")
    w("  deps = gcc")
    w("")
    w("rule cc_oom_boundary")
    w(f"  command = clang $cstd {_DEBUG} $objcwarn $cinc {OOM_DEFINES} -MMD -MF $out.d -c $in -o $out")
    w("  depfile = $out.d")
    w("  deps = gcc")
    w("")
    # The injector and the test are NOT redefined (they reach the real allocator),
    # and need tests/ on the include path for oom_alloc.h.  test_oom.c stays
    # checked; the injector itself compiles unchecked (still sanitized) because
    # -fbounds-safety's alloc_size return check traps a non-NULL return whose size
    # is zero, and a faithful libc wrapper must pass malloc(0)'s non-NULL block
    # through verbatim (see cc_cov_shim).
    w("rule cc_oom_harness")
    w(f"  command = clang $cstd {BOUNDS} {_DEBUG} $cwarn $cinc "
      "-Itests -MMD -MF $out.d -c $in -o $out")
    w("  depfile = $out.d")
    w("  deps = gcc")
    w("")
    w("rule cc_oom_shim")
    w(f"  command = clang $cstd {_DEBUG} $objcwarn $cinc "
      "-Itests -MMD -MF $out.d -c $in -o $out")
    w("  depfile = $out.d")
    w("  deps = gcc")
    w("")
    w("rule link_oom")
    w(f"  command = clang {_DEBUG} $in {BASE_FRAMEWORKS} -o $out")
    w("")
    oom_objs = []
    for c in core_c:
        o = obj("oom", c)
        ccrule = "cc_oom_boundary" if os.path.basename(c) in BOUNDARY_C else "cc_oom"
        w(f"build {o}: {ccrule} {c}")
        oom_objs.append(o)
    oom_bsrc = BACKENDS["cpu"][0]
    oom_bo = obj("oom", oom_bsrc)
    w(f"build {oom_bo}: cc_oom {oom_bsrc}")
    oom_objs.append(oom_bo)
    w("build build/oom/obj/oom_alloc.o: cc_oom_shim tests/oom_alloc.c")
    w("build build/oom/obj/test_oom.o: cc_oom_harness tests/test_oom.c")
    w(f"build build/oom/test_oom: link_oom build/oom/obj/test_oom.o "
      f"build/oom/obj/oom_alloc.o {' '.join(oom_objs)}")
    w("build build/oom/test_oom.runok: run build/oom/test_oom")
    w("  bin = build/oom/test_oom")
    w("build oom: phony build/oom/test_oom.runok")

    # The default `all` builds every variant's executables -- tests, benches and
    # examples -- runs the whole test suite (`test`), re-renders the gallery PNGs
    # (`images`) so they track the renderer in lockstep, replays the fuzz corpus
    # (`fuzzcorpus`), checks the two compositor backends agree (`backenddiff`), runs
    # the security gates `analyze` (static UAF/double-free/leak) and `leakcheck`
    # (the macOS `leaks` tool), and sweeps allocation failures (`oom`).  A bare
    # `ninja` is meant to do everything, so all of these gate it; the gates are
    # idempotent (stamps), so a clean tree is still "no work to do".  Only the
    # always-rerun measurement targets (benchcmp, profile, coverage) stay opt-in.
    all_targets = ("release debug unsafe release-cpu debug-cpu test images "
                   "analyze leakcheck backenddiff oom")
    if fuzz_corpus:
        all_targets += " fuzzcorpus"
    w(f"build all: phony {all_targets}")
    w("default all")
    w("")

    with open(os.path.join(HERE, "build.ninja"), "w") as f:
        f.write("\n".join(n))
    print(f"wrote build.ninja: {len(core_c)} core .c, {len(BACKENDS)} backends, "
          f"{len(tests)} test(s), {len(benches)} bench(es)")


if __name__ == "__main__":
    main()
