# Sourced by the Ghidra scripts: find the external tools. Each tool is searched in this order: its environment
# variable, a local install in tools/external/ (gitignored), then a normal system install.
#   find_ghidra   -> GHIDRA_HOME (the Ghidra install folder with support/analyzeHeadless)
# Java for Ghidra (JDK 21+): JAVA_HOME if it is set, else the java on PATH.

FIND_TOOLS_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXTERNAL="$FIND_TOOLS_ROOT/tools/external"

newest() {
    ls -d "$@" 2>/dev/null | sort -V | tail -1
}

find_ghidra() {
    if [[ -n "${GHIDRA_HOME:-}" ]]; then
        return
    fi
    local candidate
    for candidate in \
        "$(newest "$EXTERNAL"/ghidra_*_PUBLIC)" \
        "$(command -v ghidraRun > /dev/null && dirname "$(readlink -f "$(command -v ghidraRun)")")" \
        /opt/ghidra "$(newest /opt/ghidra_*_PUBLIC)" /usr/share/ghidra /usr/lib/ghidra; do
        if [[ -n "$candidate" && -x "$candidate/support/analyzeHeadless" ]]; then
            GHIDRA_HOME="$candidate"
            return
        fi
    done
    echo "Ghidra not found: install it into tools/external/ghidra_<version>_PUBLIC (or /opt/ghidra), or set" \
        "GHIDRA_HOME." >&2
    exit 1
}
