#!/usr/bin/env bash
# Install a freshly built ps2EntryRunner.exe as XenosagaRecomp.exe, plus the
# MSYS2 MinGW DLLs it needs. The previous exe is kept as
# XenosagaRecomp.exe.bak-<timestamp> (never deleted).
#
#   bash port/tools/install.sh [install dir]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="${1:-<install folder>}"
BUILD="${PS2X_BUILD_DIR:-$ROOT/PS2Recomp/build-runtime/ps2xRuntime}"
MINGW_BIN="${MINGW_BIN:-/c/msys64/mingw64/bin}"
export PATH="$MINGW_BIN:$PATH"
EXE="$BUILD/ps2EntryRunner.exe"
[ -f "$EXE" ] || { echo "no build at $EXE"; exit 1; }

if [ -f "$DEST/XenosagaRecomp.exe" ]; then
    bak="$DEST/XenosagaRecomp.exe.bak-$(date +%Y%m%d-%H%M%S)"
    mv "$DEST/XenosagaRecomp.exe" "$bak"
    echo "backed up old exe to $(basename "$bak")"
fi
cp "$EXE" "$DEST/XenosagaRecomp.exe"

# Walk the import tables and copy every DLL that comes from MSYS2.
declare -A seen
queue=("$EXE")
n=0
while [ ${#queue[@]} -gt 0 ]; do
    f="${queue[0]}"; queue=("${queue[@]:1}")
    for d in $(objdump -p "$f" | awk '/DLL Name/{print $3}'); do
        [ -n "${seen[$d]:-}" ] && continue
        seen[$d]=1
        if [ -f "$MINGW_BIN/$d" ]; then
            cp -u "$MINGW_BIN/$d" "$DEST/"
            queue+=("$MINGW_BIN/$d")
            n=$((n + 1))
        fi
    done
done
echo "installed XenosagaRecomp.exe and $n DLLs into $DEST"
