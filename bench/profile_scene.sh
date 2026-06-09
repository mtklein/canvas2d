#!/bin/sh
# Per-function self-time profile of the *whole rendering pipeline* driven by real
# scenes, via macOS `sample`.
#
# `ninja profile` samples the micro-benches (one kernel each); this samples the
# gallery -- every public-API scene (fills, gradients, strokes, clips, text, blend
# modes, shadows) run back to back -- so the profile shows where time goes across a
# realistic mix, not within one isolated kernel.  The gallery renders in ~0.2 s, too
# brief to sample, so GALLERY_REPS loops it (writing PNGs only on the final rep, which
# the sample window never reaches, so disk I/O stays out of the profile).
#
# Uses the release-cpu gallery: `sample` sees only CPU program counters, and the cpu
# backend keeps the whole pipeline on the CPU (the Metal backend would just show
# CPU<->GPU sync stalls -- see `ninja gputime` for the GPU side).
#
# Usage: bench/profile_scene.sh [gallery-binary]   (default build/release-cpu/gallery)
#   GALLERY_REPS  repeat count        (default 200)
#   PROFILE_SECS  sampling window s   (default 4)
set -eu

BIN="${1:-build/release-cpu/gallery}"
REPS="${GALLERY_REPS:-200}"
SECS="${PROFILE_SECS:-4}"

if ! command -v sample >/dev/null 2>&1; then
    echo "profile-scene: 'sample' not found (macOS developer tool) -- skipping." >&2
    exit 0
fi
if [ ! -x "$BIN" ]; then
    echo "profile-scene: $BIN not built -- skipping." >&2
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

GALLERY_REPS="$REPS" "$BIN" >/dev/null 2>&1 &
pid=$!
sample "$pid" "$SECS" -mayDie -file "$tmp/gallery.txt" >/dev/null 2>&1 || true
kill "$pid" 2>/dev/null || true   # stop the (still-looping) gallery once sampled
wait "$pid" 2>/dev/null || true
# `sample` profiles every thread, so the blur's parked GCD worker pool shows up as a
# big __workq_kernreturn pile -- idle waiting, not compute.  Drop those and the loader
# / kernel-trap frames so the leaderboard is actual self-time in our code.
printf '\n===== gallery (all scenes) :: compute self-time (samples) =====\n'
awk '/Sort by top of stack/{p=1;next} p&&/^$/{exit} p' "$tmp/gallery.txt" \
  | grep -vE 'libsystem_kernel|libsystem_pthread|libdispatch|_dyld_start|dyld`|mach_msg|__workq_kernreturn|madvise|libsystem_platform' \
  | head -15
