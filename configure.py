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

HERE = os.path.dirname(os.path.abspath(__file__))


def rel(p):
    return os.path.relpath(p, HERE)


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
BOUNDARY_C = {"cnvs_font_ct.c"}

_DEBUG = "-O0 -g -fsanitize=address,integer,undefined -fno-sanitize-recover=all"

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
    tests = sorted(rel(p) for p in glob.glob(os.path.join(HERE, "tests", "test_*.c")))
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
    w("rule benchcmp")
    w("  command = $cmd")
    w("  pool = console")
    w("")
    w("rule profile")
    w("  command = sh bench/profile.sh")
    w("  pool = console")
    w("")
    w("rule run_gallery")
    w("  command = $bin")
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
    # benchcmp names a file that is never created, so ninja always reruns it.
    w(f"build benchcmp: benchcmp {' '.join(bench_exes)}")
    w(f"  cmd = {benchcmp_cmd}")
    # `profile` samples the release benches in place (no output file, always reruns).
    release_bench_exes = [f"build/release/{s}" for s in bench_stems]
    w(f"build profile: profile {' '.join(release_bench_exes)}")
    # The default `all` builds every variant's executables -- tests, benches and
    # examples -- runs the whole test suite (`test`), re-renders the gallery PNGs
    # (`images`) so they track the renderer in lockstep, and replays the fuzz
    # corpus (`fuzzcorpus`).  Listing all five variant phonies covers every
    # buildable artifact (release/unsafe carry the benches; release/release-cpu
    # carry the examples).  A passing build is silent, so a green `ninja` shows
    # only its progress -- a rendering change instead surfaces as a dirtied
    # gallery/*.png.  The always-rerun measurement targets (benchcmp, profile)
    # stay opt-in -- not in `all`.
    all_targets = "release debug unsafe release-cpu debug-cpu test images"
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
