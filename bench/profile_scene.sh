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
# Uses the release gallery: `sample` sees only CPU program counters, and the whole
# pipeline (including the software compositor) runs on the CPU.
#
# Usage: bench/profile_scene.sh [gallery-binary]   (default build/release/gallery)
#   GALLERY_REPS  repeat count        (default 200)
#   PROFILE_SECS  sampling window s   (default 4)
set -eu

BIN="${1:-build/release/gallery}"
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
# `sample` profiles every thread, and the system frameworks we link (Core Text /
# Core Graphics) park a libdispatch worker pool whether or not anyone dispatches
# work -- canvas2d itself is entirely single-threaded -- so an idle
# __workq_kernreturn pile shows up that is waiting, not compute.  Drop it and the
# loader / kernel-trap frames so the leaderboard is actual self-time in our code.
printf '\n===== gallery (all scenes) :: compute self-time (samples) =====\n'
awk '/Sort by top of stack/{p=1;next} p&&/^$/{exit} p' "$tmp/gallery.txt" \
  | grep -vE 'libsystem_kernel|libsystem_pthread|libdispatch|_dyld_start|dyld`|mach_msg|__workq_kernreturn|madvise|libsystem_platform' \
  | head -15
