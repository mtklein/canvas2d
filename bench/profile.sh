#!/bin/sh
# Per-kernel self-time profiles for the CPU benchmarks, via macOS `sample`.
#
# `ninja benchcmp` answers one question -- what -fbounds-safety costs per phase --
# but can't show where time goes *within* a phase.  This does: it stretches each
# bench with BENCH_REPS so it runs long enough to sample, profiles the running
# process, and prints the "top of stack" (self-time) leaders.  That's how a scalar
# CRC hiding inside "PNG encode" becomes visible.
#
# Usage: bench/profile.sh [bin-dir]   (default build/release)
#   BENCH_REPS   repeat count per bench   (default 80)
#   PROFILE_SECS sampling window seconds   (default 4)
set -eu

DIR="${1:-build/release}"
REPS="${BENCH_REPS:-80}"
SECS="${PROFILE_SECS:-4}"

if ! command -v sample >/dev/null 2>&1; then
    echo "profile: 'sample' not found (macOS developer tool) -- skipping." >&2
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# e2e first, then the isolated phases.
for bin in "$DIR/bench" "$DIR"/bench_*; do
    [ -x "$bin" ] || continue
    name="$(basename "$bin")"
    BENCH_REPS="$REPS" "$bin" >/dev/null 2>&1 &
    pid=$!
    sample "$pid" "$SECS" -mayDie -file "$tmp/$name.txt" >/dev/null 2>&1 || true
    kill "$pid" 2>/dev/null || true   # stop the (still-looping) bench once sampled
    wait "$pid" 2>/dev/null || true
    printf '\n===== %s :: self time (samples) =====\n' "$name"
    awk '/Sort by top of stack/{p=1;next} p&&/^$/{exit} p' "$tmp/$name.txt" | head -12
done
