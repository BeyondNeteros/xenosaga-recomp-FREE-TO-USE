#!/usr/bin/env bash
# Checks a VU1 fast mode against the accurate interpreter on every capture in
# runs/*/vu1_capture.bin: same checksum (VU memory + registers) and same GIF
# output. Usage: bash port/tools/vu1_check.sh [ENV=VALUE for the fast run ...]
# Reference: PS2X_VU1_FAST=0 PS2X_VU1_EXACT_FLAGS=1 (original paths). Default compares with PS2X_VU1_FAST=1.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
EXE="$ROOT/PS2Recomp/build-runtime/ps2xRuntime/vu1_replay.exe"
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH" # MinGW runtime DLLs
TMP="$ROOT/runs/vu1_check.$$" # under runs/: the Windows exe cannot write to MSYS /tmp
mkdir -p "$TMP"
FAST_ENV=("$@")
[ ${#FAST_ENV[@]} -eq 0 ] && FAST_ENV=(PS2X_VU1_FAST=1)
fail=0
for cap in "$ROOT"/runs/*/vu1_capture.bin; do
    name=$(basename "$(dirname "$cap")")
    ref=$(env PS2X_VU1_FAST=0 PS2X_VU1_EXACT_FLAGS=1 VU_REPLAY_NOGS=1 VU_REPLAY_GIFDUMP="$TMP/ref.bin" "$EXE" "$cap" 1 2>/dev/null)
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
