#!/usr/bin/env python3
"""Generate build.ninja for the canvas2d project.

Build variants (one source tree):

  release  -Os -g -fbounds-safety           (shipping build)
  debug    -O0 -g -fbounds-safety -fsanitize=address,integer,undefined
  unsafe   -Os -g                           (release minus -fbounds-safety)

`release` vs `unsafe` isolates the cost of -fbounds-safety: same sources and
optimisation, only the flag differs. `ninja benchcmp` runs hyperfine over the two.

The C core is built -std=c23 -Werror -Weverything (plus -fbounds-safety for
release/debug); the few disabled warnings below are each justified.

One source file is a platform boundary, built -Wall -Wextra without
-fbounds-safety (still under the debug sanitizers): the Core Text font shim
(BOUNDARY_C), which binds un-annotated system headers behind a bounds-safe C ABI;
see docs/bounds-safety.md.
"""

import os
import re
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
    is simply not emitted, so a bare `ninja` is unaffected (`ninja fuzzers` needs
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
BOUNDARY_WARN = "-Wall -Wextra"

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

# The compositor backend: the software compositor (src/compositor_cpu.c), built into
# every binary.  It links no extra frameworks beyond BASE_FRAMEWORKS.
COMPOSITOR_SRC = "src/compositor_cpu.c"
BACKEND_SRCS = {os.path.basename(COMPOSITOR_SRC)}

# Platform-boundary C sources: built without -fbounds-safety at -Wall -Wextra
# because they bind un-annotated system headers, behind a bounds-safe ABI.
BOUNDARY_C = {"cnvs_text_ct.c"}

# Test-only system libraries, by source basename.  The differential oracle (H5
# in docs/decisions/codec-outsourcing.md) links the SYSTEM zlib so reference
# deflate/inflate cross-check ours.  Scoped to exactly these binaries' link
# edges (the $libs ninja variable, empty everywhere else): no release/debug/
# unsafe library object links -lz, so the canvas library itself keeps its
# from-scratch no-zlib posture.
EXTRA_LIBS = {
    "test_zlib_oracle.c": "-lz",
    "fuzz_zlib_diff.c": "-lz",
}

# Tests that read gallery/ files from disk.  Their run edges take the
# run_gallery outputs as order-only deps so they never race the re-render
# rewriting those same files mid-read (see the test-loop comment below).
# A new test that opens gallery/ paths belongs in this set.
GALLERY_READERS = {"test_replay_gallery", "test_pngload"}

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

# --- libFuzzer fuzz targets (opt-in `ninja fuzzers`; see homebrew_clang() above) ----
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
# Modules the libFuzzer harnesses do NOT link.  Currently empty: every src/*.c is
# part of the render core a harness can reach.  (It used to hold the pixvm VM, the
# colour LUT, and the ring buffer -- self-contained -fbounds-safety probes no
# harness exercised -- but those have been retired from the tree.)  The fuzz core
# is the whole canvas render core (core_c, globbed) plus the CPU compositor backend,
# so a new cnvs_*.c module is picked up automatically.  Kept as an *exclude* set
# (rather than an include list) because the include list grows with every feature
# and silently drifting out of date is exactly how the old hand-listed CORE rotted;
# an empty exclude needs no maintenance.
FUZZ_CORE_EXCLUDE = set()

# variant -> (opt flags, bounds-safety?, build tests?, build bench?).
# release/unsafe carry -g so Instruments shows source beside the assembly when
# profiling them (the linked binaries keep a debug map into build/*/obj/*.o,
# where the DWARF lives).  Debug info changes no codegen -- the gallery
# byte-diff would catch it if it did.
VARIANTS = {
    "release": ("-Os -g", True,  True,  True),
    "debug":   (_DEBUG, True, True,  False),
    "unsafe":  ("-Os -g", False, False, True),
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
    #
    # This is a STATIC list, deliberately not a glob of gallery/*.png: these files
    # are the run_gallery edge's own outputs, so globbing them made the build's
    # description depend on its outputs existing -- `ninja -t clean` removed them
    # and the next configure emitted an empty `build : run_gallery ...` (invalid).
    # A static list is also the gate's posture (gate.yml names its scenes).  The
    # list must match what examples/gallery.c emits: a name here that gallery.c
    # never writes makes `ninja images` fail (output not produced); a scene
    # gallery.c writes but omitted here lands as an untracked file `git diff`
    # catches -- so a forgotten update fails loudly either way.  One name per
    # gallery scene, sorted.
    gallery_scenes = [
        "affine", "batch", "blend", "clip", "conic", "dashes", "dirtyrect",
        "drawimage", "emoji", "emojiscale", "filters", "gradients", "hittest",
        "imagedata", "joins", "miterdash", "path2d", "paths", "pattern",
        "porterduff", "roundrect", "rtl", "shadows", "shapes", "shaping",
        "smoothing", "strokerect", "subrect", "text", "textgrid",
        "textmaxwidth", "textmetrics", "winding",
    ]
    gallery_pngs = [f"gallery/{name}.png" for name in gallery_scenes]
    # EVERY scene also records a self-contained .canvas program alongside its
    # PNG (examples/gallery.c's record_scene): the serialized font/glyph/
    # bitmap/shape blocks plus the numbered image and path blocks let the
    # program replay on a FONTLESS machine.  Committed build artifacts exactly
    # like the PNGs, and listed STATICALLY for the same reason (a glob of the
    # run_gallery edge's own outputs reintroduces the clean-then-empty-edge
    # circularity ed65b4d killed) -- one .canvas per gallery scene, derived
    # from the same static list.  tests/test_replay_gallery.c replays each and
    # byte-compares to its PNG -- the determinism gate; the CI runner has no
    # Libian TC, so a replay that reproduced a text scene's PNG used the
    # embedded blocks, not host fonts.
    gallery_canvases = [f"gallery/{name}.canvas" for name in gallery_scenes]
    # Committed fuzz regression corpus (distinct from the gitignored fuzz/seeds/
    # scratch).  `ninja` replays every input under the debug sanitizers, so a
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
    w(f"boundarywarn = {BOUNDARY_WARN}")
    w("")
    w("# Rationale for each disabled warning:")
    for name, why in CWARN_DISABLED:
        w(f"#   -Wno-{name}: {why}")
    w("")

    # Self-regeneration: build.ninja depends on this script, so editing
    # configure.py takes effect on the next `ninja` -- no stale-graph builds from
    # forgetting to rerun it by hand.  `generator = 1` marks the output as
    # build-system metadata (ninja won't delete it on interrupt or clean it).
    # The globbed directories (regen_dirs) are implicit inputs: a directory's mtime
    # bumps exactly when an entry is added/removed/renamed (not when a file's
    # contents change), so creating a source file also regenerates the graph, while
    # ordinary edits don't.  Keep regen_dirs in sync with the glob.glob calls
    # (gallery PNGs are rewritten in place by `ninja images`, which leaves the
    # directory mtime alone, so watching gallery/ does not loop).
    #
    # A watched directory name must NOT also be the output of a build edge.  When it
    # is (e.g. a `phony` alias of the same name), ninja resolves the regen input to
    # that target node and brings it up to date as a *prerequisite of regenerating
    # build.ninja* -- so a bare `ninja` drags in whatever the alias points at.  This
    # once shipped: a `fuzz` phony made `ninja` build the entire opt-in libFuzzer
    # suite.  The phony aliases are therefore named off the source-dir names -- test
    # (not tests), images (not gallery), benches (not bench), fuzzers (not fuzz) --
    # and the assert at the end of main() keeps the two namespaces disjoint.
    #
    # DELETING a globbed source still needs a manual `python3 configure.py`:
    # ninja refuses to load a graph whose stale manifest references the missing
    # file ("needed by ... missing and no known rule to make it") before it
    # ever reaches the regen edge -- even when asked for build.ninja alone.
    # Verified empirically when bench_blur_vpf.c was retired.
    regen_dirs = ["src", "tests", "bench", "examples", "gallery", "fuzz", "fuzz/corpus"]
    w("rule configure")
    w("  command = python3 configure.py")
    w("  generator = 1")
    w("")
    w(f"build build.ninja: configure configure.py | {' '.join(regen_dirs)}")
    w("")

    for variant, (opt, bounds, _tests, _bench) in VARIANTS.items():
        bflag = (BOUNDS + " ") if bounds else ""
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
        # debug sanitizers still apply.
        w(f"rule cc_boundary_{variant}")
        w(f"  command = clang {opt} $cstd $boundarywarn $cinc -MMD -MF $out.d -c $in -o $out")
        w("  depfile = $out.d")
        w("  deps = gcc")
        w("")
        # $libs is per-edge (EXTRA_LIBS) and empty for every binary not named there.
        w(f"rule link_{variant}")
        w(f"  command = clang {opt} $in {BASE_FRAMEWORKS} $libs -o $out")
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
    w("  command = $cmd")
    w("  pool = console")
    w("")
    w("rule profile_scene")
    w("  command = $cmd")
    w("  pool = console")
    w("")
    w("rule throughput")
    w("  command = $cmd")
    w("  pool = console")
    w("")
    w("rule run_gallery")
    w("  command = $bin")
    # generator: the gallery PNGs are committed build outputs; this keeps
    # `ninja -t clean` from deleting them -- the same exemption build.ninja
    # itself gets.  (The static gallery_pngs list already keeps configure valid
    # if they go missing; this just spares the working tree the churn.)  They
    # still re-render whenever the gallery binary input changes; only the
    # rebuild-on-command-change and clean-by-default behaviours are dropped, both
    # irrelevant here.  (`ninja -t clean -g` still removes them if you mean it.)
    w("  generator = 1")
    w("")
    w("rule analyze")
    w(f"  command = clang {ANALYZE} $cstd $cinc $in && touch $out")
    w("")
    # `leakcheck` runs the non-ASan release build under the macOS `leaks` tool
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
    # `forgecheck` (in `all`): the mechanical no-escape-hatch gate.  -fbounds-safety's
    # spatial guarantee only holds for code that stays in the checked domain; an
    # `__unsafe_forge*` / `__unsafe_indexable` is the deliberate exit from it.  After
    # the tag-pointer probe (the last legitimate forge user) was retired, the whole
    # checked tree -- src/ include/ tests/ examples/, with NO exemptions (even the one
    # boundary TU, cnvs_text_ct.c, is plain C with zero __unsafe spellings) -- contains
    # no `__unsafe` token.  This greps for one and FAILS the build if it reappears, so a
    # future edit can't silently reintroduce an escape hatch into checked code.  Empty
    # grep -> touch the stamp; any hit (or a grep error, folded into the match text via
    # 2>&1) prints the offending lines and exits nonzero.  Idempotent stamp, so it lives
    # in `all` and runs on a bare `ninja`.  ($$ is a literal $ for ninja.)
    w("rule forgecheck")
    w("  command = hits=$$(grep -rn __unsafe src include tests examples 2>&1) ; "
      'if [ -n "$$hits" ] ; then '
      'echo "forgecheck: __unsafe_ escape hatch in checked code (gate src/ include/ '
      'tests/ examples/ -- no exemptions):" >&2 ; '
      'echo "$$hits" >&2 ; exit 1 ; '
      "else touch $out ; fi")
    w("")

    # Bench stems, e2e "bench" sorted last; variants that build benches.
    bench_stems = sorted((os.path.splitext(os.path.basename(b))[0] for b in benches),
                         key=lambda s: (s == "bench", s))
    bench_variants = [v for v, cfg in VARIANTS.items() if cfg[3]]

    test_stamps = []
    variant_lib_objs = {}
    for variant, (_opt, _bounds, do_tests, do_bench) in VARIANTS.items():
        lib_objs = []
        for c in core_c:
            o = obj(variant, c)
            ccrule = "cc_boundary" if os.path.basename(c) in BOUNDARY_C else "cc"
            w(f"build {o}: {ccrule}_{variant} {c}")
            lib_objs.append(o)
        # The software compositor backend.
        bo = obj(variant, COMPOSITOR_SRC)
        w(f"build {bo}: cc_{variant} {COMPOSITOR_SRC}")
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
                if os.path.basename(t) in EXTRA_LIBS:
                    w(f"  libs = {EXTRA_LIBS[os.path.basename(t)]}")
                # Tests that read gallery/ files from disk race the
                # run_gallery edge that rewrites those same files in the same
                # ninja invocation unless ordered after it.  CI hit exactly
                # this: test_replay_gallery read emojiscale's fresh .canvas
                # against a stale .png mid-re-render, DIVERGED.  test_pngload
                # (decodes every committed PNG) is the same class with a
                # narrower window (a mid-write truncated PNG).  Order-only
                # (||): the test must run after the re-render, but re-rendered
                # pixels don't dirty the test.  In-suite these tests therefore
                # check *this* machine's render; gate.yml's restore step is
                # what proves the *committed* bytes replay on a fontless
                # runner.
                gallery_dep = ""
                if stem in GALLERY_READERS:
                    gallery_dep = " || " + " ".join(gallery_pngs + gallery_canvases)
                w(f"build {stamp}: run {exe}{gallery_dep}")
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
        if variant == "release":
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
    w(f"build benches: phony {' '.join(bench_exes)}")
    # `images` renders straight into the committed gallery/*.png.  The PNGs are
    # outputs gated on the gallery binary, so a bare `ninja` keeps them in
    # lockstep with the renderer -- and dirties the tree the moment a code change
    # moves a pixel.  `ninja images` is the same edge on its own.  (gallery_pngs
    # is a static list now, so this edge is always well-formed even if the PNGs
    # are absent on disk -- the empty-glob failure that needed a guard is gone.)
    # The .canvas programs are outputs of the same edge as the PNGs: one gallery
    # run emits both, so a renderer change re-emits the programs in lockstep with
    # the pixels (and the run_gallery `generator = 1` exempts them from
    # `ninja -t clean` just like the PNGs).
    w(f"build {' '.join(gallery_pngs + gallery_canvases)}: run_gallery build/release/gallery")
    w("  bin = ./build/release/gallery")
    w(f"build images: phony {' '.join(gallery_pngs + gallery_canvases)}")
    # `fuzzcorpus`: replay the committed fuzz corpus through the API harness under
    # the debug sanitizers (ASan/UBSan), turning every seed and reduced crasher into
    # a permanent, libFuzzer-free regression.  fuzz_api.c builds like a boundary file
    # (cc_boundary: no -fbounds-safety, -Wall -Wextra) since it isn't -Weverything
    # clean; it links the bounds-safe debug core, which keeps that core's
    # -fbounds-safety traps under the same replay.
    if fuzz_corpus:
        fuzz_obj = "build/debug/obj/fuzz_api.o"
        fuzz_replay = "build/debug/fuzz_replay"
        fuzz_stamp = "build/debug/fuzz_corpus.runok"
        w(f"build {fuzz_obj}: cc_boundary_debug fuzz/fuzz_api.c")
        w(f"build {fuzz_replay}: link_debug {fuzz_obj} "
          f"{' '.join(variant_lib_objs['debug'])}")
        w(f"build {fuzz_stamp}: corpus_replay {' '.join(fuzz_corpus)} | {fuzz_replay}")
        w(f"  bin = ./{fuzz_replay}")
        w(f"build fuzzcorpus: phony {fuzz_stamp}")

    # `ninja fuzzers`: build the libFuzzer harnesses (opt-in -- needs Homebrew clang;
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
          f"{BASE_FRAMEWORKS} $libs -o $out")
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
        # compositor backend (core_c excludes it).
        fuzz_core_srcs = [c for c in core_c
                          if os.path.basename(c) not in FUZZ_CORE_EXCLUDE]
        fuzz_core_srcs.append(COMPOSITOR_SRC)
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
            if h + ".c" in EXTRA_LIBS:
                w(f"  libs = {EXTRA_LIBS[h + '.c']}")
            fuzz_bins.append(f"build/fuzz/{h}")
        w("build build/fuzz/seed_gen: cc_seedgen fuzz/seed_gen.c")
        w("build build/fuzz/seeds.stamp: gen_seeds build/fuzz/seed_gen")
        w("  bin = ./build/fuzz/seed_gen")
        w(f"build fuzzers: phony {' '.join(fuzz_bins)} build/fuzz/seeds.stamp")

    # benchcmp names a file that is never created, so ninja always reruns it.
    w(f"build benchcmp: benchcmp {' '.join(bench_exes)}")
    w(f"  cmd = {benchcmp_cmd}")
    # `profile` samples the release benches in place (no output file, always reruns).
    release_bench_exes = [f"build/release/{s}" for s in bench_stems]
    w(f"build profile: profile {' '.join(release_bench_exes)}")
    w("  cmd = sh bench/profile.sh build/release")
    # `profile-scene` samples the gallery (the real public-API pipeline across every
    # scene) rather than a single micro-bench.  Loops via GALLERY_REPS so there's
    # enough run time to sample.  No output file, always reruns.
    w("build profile-scene: profile_scene build/release/gallery")
    w("  cmd = sh bench/profile_scene.sh build/release/gallery")
    # `throughput` normalises wall time to pixels: each render bench self-times its rep
    # loop (BENCH_THROUGHPUT) and reports Mpx/s + ns/px over the finished-frame pixels
    # it produced.  Unlike a raw wall-clock (which scales with the scene), a per-pixel
    # rate is comparable across canvas sizes -- the apples-to-apples answer to "how
    # many pixels/second does this pipeline push".  A high BENCH_REPS amortises the
    # cold first rep; both the small and large bench in the per-frame-readback shape
    # (the getImageData/PNG-export workload).
    tp_sm, tp_lg = "BENCH_THROUGHPUT=1 BENCH_REPS=50", "BENCH_THROUGHPUT=1 BENCH_REPS=20"
    sm = "build/release/bench_render"
    lg = "build/release/bench_render_large"
    throughput_cmd = (
        f'echo "# small" ; {tp_sm} ./{sm} ; '
        f'echo "# large" ; {tp_lg} ./{lg}')
    w(f"build throughput: throughput {sm} {lg}")
    w(f"  cmd = {throughput_cmd}")
    # `analyze` runs the static analyzer over the checked C (core + the compositor
    # backend).  One stamp per TU so it's incremental and parallel; gated by
    # -analyzer-werror in the rule.
    analyze_srcs = core_c + [COMPOSITOR_SRC]
    analyze_stamps = []
    for c in analyze_srcs:
        stem = os.path.splitext(os.path.basename(c))[0]
        stamp = os.path.join("build", "analyze", stem + ".stamp")
        w(f"build {stamp}: analyze {c}")
        analyze_stamps.append(stamp)
    w(f"build analyze: phony {' '.join(analyze_stamps)}")
    # leakcheck stamp regenerates when the release test_leak binary or the codesign
    # entitlements change.
    w("build build/release/test_leak.leakok: leakcheck build/release/test_leak"
      " | tests/leaks.entitlements")
    w("  bin = ./build/release/test_leak")
    w("build leakcheck: phony build/release/test_leak.leakok")

    # forgecheck: grep the whole checked tree for `__unsafe`.  Every file under the
    # four gated dirs is an implicit input so an *edit* reruns the gate; the regen edge
    # (watching the src/tests/examples dir mtimes) re-emits this with any *new* file as
    # an input, so an added file is grepped too.  The stamp lives under build/.
    forgecheck_files = sorted(
        rel(p) for d in ("src", "include", "tests", "examples")
        for p in glob.glob(os.path.join(HERE, d, "**", "*"), recursive=True)
        if os.path.isfile(p))
    w(f"build build/forgecheck.stamp: forgecheck | {' '.join(forgecheck_files)}")
    w("build forgecheck: phony build/forgecheck.stamp")

    # `coverage` (opt-in: `ninja coverage`): source-based coverage of the checked
    # C core.  Instrument core + tests with -fprofile-instr-generate
    # -fcoverage-mapping at -O0 (accurate region/line mapping), run every test
    # writing its own .profraw, merge, and print an llvm-cov report over src/.
    # Deterministic, no GPU.  Like benchcmp/profile it's a
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
    w(f"  command = clang $cstd $boundarywarn $cinc {OOM_DEFINES} {COV} -MMD -MF $out.d -c $in -o $out")
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
    w(f"  command = clang $cstd $boundarywarn $cinc -Itests {COV} -MMD -MF $out.d -c $in -o $out")
    w("  depfile = $out.d")
    w("  deps = gcc")
    w("")
    w("rule link_cov")
    w(f"  command = clang {COV} $in {BASE_FRAMEWORKS} $libs -o $out")
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
    # generator: docs/coverage.md is committed (browsable on GitHub), like the
    # gallery PNGs -- keep `ninja -t clean` from deleting it.  `ninja coverage`
    # regenerates it on demand regardless.
    w("  generator = 1")
    w("")
    cov_lib = []
    for c in core_c:
        o = obj("cov", c)
        ccrule = "cc_cov_boundary" if os.path.basename(c) in BOUNDARY_C else "cc_cov"
        w(f"build {o}: {ccrule} {c}")
        cov_lib.append(o)
    cov_bsrc = COMPOSITOR_SRC
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
        if os.path.basename(t) in EXTRA_LIBS:
            w(f"  libs = {EXTRA_LIBS[os.path.basename(t)]}")
        # Same run_gallery ordering as the variant test loop above.
        gallery_dep = ""
        if stem in GALLERY_READERS:
            gallery_dep = " || " + " ".join(gallery_pngs + gallery_canvases)
        w(f"build {raw}: cov_run {exe}{gallery_dep}")
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
    # debug config's -fbounds-safety + ASan/UBSan enforce.  This reaches the
    # `if (!p) return false` OOM guards that coverage flagged as the dominant
    # untaken branch class; the normal suite never fails an allocation.  The macro
    # redefine is invisible to -fbounds-safety: stdlib.h's malloc declaration (with
    # __sized_by_or_null/alloc_size) macro-expands onto cnvs_oom_malloc, so size
    # tracking is preserved.  Folded into `all`.
    w("rule cc_oom")
    w(f"  command = clang $cstd {BOUNDS} {_DEBUG} $cwarn $cinc {OOM_DEFINES} -MMD -MF $out.d -c $in -o $out")
    w("  depfile = $out.d")
    w("  deps = gcc")
    w("")
    w("rule cc_oom_boundary")
    w(f"  command = clang $cstd {_DEBUG} $boundarywarn $cinc {OOM_DEFINES} -MMD -MF $out.d -c $in -o $out")
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
    w(f"  command = clang $cstd {_DEBUG} $boundarywarn $cinc "
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
    oom_bsrc = COMPOSITOR_SRC
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
    # (`fuzzcorpus`), runs the security gates `analyze` (static UAF/double-free/leak),
    # `leakcheck` (the macOS `leaks` tool), and `forgecheck` (no `__unsafe_` escape
    # hatch in checked code), and sweeps allocation failures (`oom`).  A bare `ninja`
    # is meant to do everything, so all of these gate it; the gates are idempotent
    # (stamps), so a clean tree is still "no work to do".  Only the always-rerun
    # measurement targets (benchcmp, profile, coverage) stay opt-in.
    all_targets = ("release debug unsafe test images "
                   "analyze leakcheck forgecheck oom")
    if fuzz_corpus:
        all_targets += " fuzzcorpus"
    w(f"build all: phony {all_targets}")
    w("default all")
    w("")

    # Disjointness guard for the regen edge (see its comment above): a build output
    # that shares a name with a watched directory is built as a prerequisite of
    # regenerating build.ninja, silently pulling that target into a bare `ninja`.
    # This shipped once (a `fuzz` phony dragged in the whole libFuzzer suite); the
    # assert turns any recurrence into a loud configure-time error.  Phony outputs
    # only -- those are the convenience aliases liable to reuse a source-dir name.
    phony_outputs = set()
    for line in n:
        m = re.match(r"build (.+?): phony\b", line)
        if m:
            phony_outputs.update(m.group(1).split())
    clash = set(regen_dirs) & phony_outputs
    if clash:
        raise SystemExit(
            f"configure.py: phony target(s) {sorted(clash)} collide with the regen "
            "edge's watched directories; rename them, or ninja will build the target "
            "as a prerequisite of regenerating build.ninja (dragging it into a bare "
            "`ninja`)")

    with open(os.path.join(HERE, "build.ninja"), "w") as f:
        f.write("\n".join(n))
    print(f"wrote build.ninja: {len(core_c)} core .c, "
          f"{len(tests)} test(s), {len(benches)} bench(es)")


if __name__ == "__main__":
    main()
