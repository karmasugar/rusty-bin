#!/usr/bin/env bash
# Import Rusty programs into a headless Ghidra project: build/ghidra/rusty.gpr, one folder per build.
# Usage: tools/ghidra/import.sh [ja|en] [file ...]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-ja}"
shift || true

source "$ROOT/tools/find_tools.sh"
find_ghidra
HEADLESS="$GHIDRA_HOME/support/analyzeHeadless"
SCRIPTS="$ROOT/tools/ghidra/scripts"
PROJECT_DIR="$ROOT/build/ghidra"
SOURCE="$ROOT/build/unpacked/$BUILD"
PROCESSOR="x86:LE:16:Real Mode"
COM_BASE="1000:0100"
OVERLAY_BASE="2000:0000"

if [[ ! -x "$HEADLESS" ]]; then
    echo "analyzeHeadless not found. Set GHIDRA_HOME (now: '$GHIDRA_HOME')." >&2
    exit 1
fi
if [[ ! -d "$SOURCE" ]]; then
    echo "$SOURCE missing. Run tools/extract.py, tools/lz_unpack.py --copy-plain and tools/exepack.py first." >&2
    exit 1
fi
mkdir -p "$PROJECT_DIR"

run() {
    "$HEADLESS" "$PROJECT_DIR" "rusty/$BUILD" -scriptPath "$SCRIPTS" -overwrite "$@" 2>&1 \
        | grep -E "\.java> |ERROR|Exception" | sed -E 's/^INFO +//; s/ +\((GhidraScript|HeadlessAnalyzer)\).*$//' || true
}

import_mz() {
    run -import "$SOURCE/$1" -postScript Pc98Annotate.java
}

import_com() {
    run -import "$SOURCE/$1" -processor "$PROCESSOR" -loader BinaryLoader -loader-baseAddr "$COM_BASE" \
        -preScript Pc98Annotate.java com -postScript Pc98Annotate.java
}

import_raw_entry() {
    run -import "$SOURCE/$1" -processor "$PROCESSOR" -loader BinaryLoader -loader-baseAddr "$OVERLAY_BASE" \
        -preScript Pc98Annotate.java com -postScript Pc98Annotate.java
}

import_overlay() {
    local file="$1" words="$2" code="$3" imports="${4:-none}"
    run -import "$SOURCE/$file" -processor "$PROCESSOR" -loader BinaryLoader -loader-baseAddr "$OVERLAY_BASE" \
        -preScript RustyOverlayEntries.java "$words" "$code" "$imports" -postScript Pc98Annotate.java
}

import_one() {
    case "$1" in
        JO.EXE) import_mz "$1" ;;
        BOOT.COM | OP.COM | ONGCHK.COM) import_com "$1" ;;
        VISUAL.COM) import_overlay "$1" 7 all ;;
        STORY*.COM) import_overlay "$1" 4 all ;;
        ENEMY*.COM) import_overlay "$1" 11 2-4 5-10 ;;
        GRPGDC.COM | GRPEGC.COM) import_overlay "$1" 38 all ;;
        MAGLOAD.COM | MGXLOAD.BIN) import_raw_entry "$1" ;;
        *) echo "no import rule for $1" >&2 ;;
    esac
}

if [[ $# -gt 0 ]]; then
    FILES=("$@")
else
    FILES=(JO.EXE BOOT.COM OP.COM ONGCHK.COM VISUAL.COM GRPGDC.COM GRPEGC.COM MAGLOAD.COM MGXLOAD.BIN)
    for n in 1 2 3 4 5 6 7 8 9 10; do
        FILES+=("STORY$n.COM" "ENEMY$n.COM")
    done
fi

for file in "${FILES[@]}"; do
    echo "== $BUILD/$file"
    import_one "$file"
done
