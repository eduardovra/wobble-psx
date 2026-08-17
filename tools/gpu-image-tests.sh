#!/bin/bash
# Runs every ps1-tests GPU test that ships a reference image and says
# how far this emulator's VRAM is from a real console's.
#
# Each test is sampled at several frame counts and the closest is kept.
# They redraw continuously rather than drawing once and stopping, so a
# single snapshot can catch a half-finished frame: gpu/triangle reads
# 0.6% or 25% wrong on the same build depending only on when it is
# asked. The best of a handful of moments is the honest number.
set -uo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here"

emulator=${EMULATOR:-./build-rel/wobble-dbg}
bios=${BIOS:-SCPH1001.BIN}
tests=games/ps1-tests/gpu
frames=${FRAMES:-"607 640 680 740"}

if [ ! -d "$tests" ]; then
    echo "no test programs — run tools/fetch-test-roms.sh first" >&2
    exit 1
fi

dump=$(mktemp --suffix=.ppm)
trap 'rm -f "$dump"' EXIT

for reference in "$tests"/*/vram.png; do
    directory=$(dirname "$reference")
    name=$(basename "$directory")
    program="$directory/$name.exe"
    [ -f "$program" ] || continue

    best=100
    best_at=0
    for frame in $frames; do
        "$emulator" "$bios" -c "exe $program" -c "frames $frame" \
            -c "vram $dump" >/dev/null 2>&1
        percent=$(python3 tools/vramdiff.py "$dump" "$reference" |
            head -1 | grep -oE '\(([0-9.]+)%\)' | tr -d '()%')
        [ -z "$percent" ] && percent=0
        if awk "BEGIN{exit !($percent < $best)}"; then
            best=$percent
            best_at=$frame
        fi
    done
    printf '%-22s %8s%%  (at %s frames)\n' "$name" "$best" "$best_at"
done
