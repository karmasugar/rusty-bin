#!/usr/bin/env bash
# Find missing entry points by running the whole game with scripted keys: build, run, and repeat while the run
# meets code that is not recompiled (the interpreter runs it and logs it; the collect-missing target keeps it).
# Usage: tools/recomp/discover.sh <max frames> [RUSTY_KEYS] [RUSTY_ENTER_AT]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
FRAMES="$1"
KEYS="${2:-}"
ENTER="${3:-}"
cmake --preset linux-dev > /dev/null
for round in $(seq 1 40); do
    cmake --build --preset linux-dev > /dev/null
    rm -f build/recomp/missing.txt
    set +e
    env RUSTY_FAST=1 RUSTY_TAIL_SECONDS=0 RUSTY_MAX_FRAMES="$FRAMES" RUSTY_KEYS="$KEYS" RUSTY_ENTER_AT="${ENTER:--1}" \
        build/linux-dev/rusty_headless build/files/ja/RUSTY > /dev/null 2> build/linux-dev/discover.err
    set -e
    stop="$(grep -E "rt fatal|frame limit" build/linux-dev/discover.err | tail -1)"
    found="$(grep -c "interpreted" build/linux-dev/discover.err || true)"
    echo "round $round: $found interpreted addresses; $stop"
    [[ -s build/recomp/missing.txt ]] || break
    cmake --build --preset linux-dev --target collect-missing > /dev/null
done
