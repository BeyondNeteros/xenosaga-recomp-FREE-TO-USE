#!/usr/bin/env bash
# Checks the current VU1 interpreter against a frozen reference on every
# capture in runs/*/vu1_capture.bin: same checksum (VU memory, VF/VI and
# MAC/status/clip) and same GIF output. Usage:
#   bash port/tools/vu1_check.sh [ENV=VALUE for the run being tested ...]
# The reference is PS2Recomp-ref/ (PS2Recomp e563698 + the runtime patch from
# port commit a9b3cd4, i.e. the interpreter before any speed work, with the
# current tools/vu1_replay.cpp), built with the same flags into
# PS2Recomp-ref/build (target vu1_replay).
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
EXE="$ROOT/PS2Recomp/build-runtime/ps2xRuntime/vu1_replay.exe"
REF="$ROOT/PS2Recomp-ref/build/ps2xRuntime/vu1_replay.exe"
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH" # MinGW runtime DLLs
TMP="$ROOT/runs/vu1_check.$$" # under runs/: the Windows exe cannot write to MSYS /tmp
mkdir -p "$TMP"
FAST_ENV=("$@")
[ ${#FAST_ENV[@]} -eq 0 ] && FAST_ENV=(PS2X_VU1_CHECK=1)
fail=0
for cap in "$ROOT"/runs/*/vu1_capture*.bin; do
    name=$(basename "$(dirname "$cap")")/$(basename "$cap" .bin)
    ref=$(env VU_REPLAY_NOGS=1 VU_REPLAY_GIFDUMP="$TMP/ref.bin" "$REF" "$cap" 1 2>/dev/null)
    new=$(env "${FAST_ENV[@]}" VU_REPLAY_NOGS=1 VU_REPLAY_GIFDUMP="$TMP/new.bin" "$EXE" "$cap" 1 2>/dev/null)
    rc=$(grep checksum <<<"$ref"); nc=$(grep checksum <<<"$new")
    if [ -z "$rc" ]; then echo "skip  $name (old capture format)"; continue; fi
    if [ "$rc" = "$nc" ] && cmp -s "$TMP/ref.bin" "$TMP/new.bin"; then
        echo "ok    $name"
    else
        echo "DIFF  $name ($rc vs $nc)"; fail=1
    fi
done
rm -rf "$TMP"
exit $fail
