#!/usr/bin/env bash
# Install a freshly built ps2EntryRunner.exe as XenosagaRecomp.exe, plus the
# MSYS2 MinGW DLLs it needs. The first install keeps the exe that was there
# as XenosagaRecomp.exe.bak-<timestamp> (never touched again); after that the
# previous build is kept as XenosagaRecomp.exe.previous.
#
#   bash port/tools/install.sh [install dir]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="${1:-${XS_GAME_DIR:?give the install folder as an argument or set XS_GAME_DIR}}"
BUILD="${PS2X_BUILD_DIR:-$ROOT/PS2Recomp/build-runtime/ps2xRuntime}"
MINGW_BIN="${MINGW_BIN:-/c/msys64/mingw64/bin}"
export PATH="$MINGW_BIN:$PATH"
EXE="$BUILD/ps2EntryRunner.exe"
[ -f "$EXE" ] || { echo "no build at $EXE"; exit 1; }

if [ -f "$DEST/XenosagaRecomp.exe" ]; then
    if ls "$DEST"/XenosagaRecomp.exe.bak-* >/dev/null 2>&1; then
        bak="$DEST/XenosagaRecomp.exe.previous"   # our own last build
    else
        bak="$DEST/XenosagaRecomp.exe.bak-$(date +%Y%m%d-%H%M%S)"
    fi
    mv -f "$DEST/XenosagaRecomp.exe" "$bak"
    echo "kept old exe as $(basename "$bak")"
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
