#!/usr/bin/env bash
# Compare register/memory checksums and GIF dumps against a reference replay.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd -P)
exe=${VU_REPLAY_EXE:-$root/PS2Recomp/build-runtime/ps2xRuntime/vu1_replay.exe}
ref=${VU_REPLAY_REF:-$root/PS2Recomp-ref/build/ps2xRuntime/vu1_replay.exe}
runs=${VU_CAPTURE_ROOT:-$root/runs}
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
shopt -s nullglob
captures=("$runs"/*/vu1_capture*.bin)
if (( ${#captures[@]} == 0 )); then
    echo "No VU1 captures under $runs" >&2
    exit 1
fi
for tool in "$ref" "$exe"; do
    [[ -x "$tool" ]] || { echo "Missing executable $tool" >&2; exit 1; }
done
work=$(mktemp -d "$runs/vu1-check.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
options=("$@")
if (( ${#options[@]} == 0 )); then options=(PS2X_VU1_CHECK=1); fi
failed=0
for i in "${!captures[@]}"; do
    cap=${captures[i]}
    if ! reference=$(env VU_REPLAY_NOGS=1 VU_REPLAY_GIFDUMP="$work/ref.$i" "$ref" "$cap" 1) ||
       ! current=$(env "${options[@]}" VU_REPLAY_NOGS=1 VU_REPLAY_GIFDUMP="$work/new.$i" "$exe" "$cap" 1); then
        echo "Replay failed for $cap" >&2
        failed=1
        continue
    fi
    if expected=$(grep checksum <<<"$reference") && actual=$(grep checksum <<<"$current") &&
       [[ "$expected" == "$actual" ]] && cmp -s "$work/ref.$i" "$work/new.$i"; then
        echo "ok $cap"
    else
        echo "Missing or different replay output for $cap" >&2
        failed=1
    fi
done
exit "$failed"
