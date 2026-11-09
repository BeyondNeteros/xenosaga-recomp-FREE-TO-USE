#!/usr/bin/env bash
# Run the installed port unattended, the same way "Launch Xenosaga.bat" does,
# then copy the results into runs/<name>/.
#
#   bash port/tools/run_game.sh <name> <seconds> [--input script.txt] [--record out.txt] [--env NAME=VALUE ...]
#
# --input replays a pad script (format: port/inputs/*.txt, checked with
# console-controller's `padmap.py check --script`); --record writes live port-0
# input as such a script. With <seconds> = 0 the game runs until closed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# Where the game is installed and your own disc image (Windows path):
GAME="${XS_GAME_DIR:?set XS_GAME_DIR to the install folder}"
DISC="${XS_DISC:?set XS_DISC to your disc image, as a Windows path}"
name="$1"; seconds="$2"; shift 2
input=""; record=""; extra_env=()
while [ $# -gt 0 ]; do
    case "$1" in
        --input) input="$(cygpath -w "$(realpath "$2")")"; shift 2 ;;
        --record) record="$(cygpath -w "$(realpath -m "$2")")"; shift 2 ;;
        --env) extra_env+=("$2"); shift 2 ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
done

taskkill //IM XenosagaRecomp.exe //F >/dev/null 2>&1 || true
cd "$GAME"
rm -f vu1_capture_*.bin gs_capture.bin xs_profile.txt xs_trx_*.bin xs_vram_after_*.bin xs_texdump* xs_ram.bin xs_spr.bin
# PS2X_WINDOW_MONITOR=1 (env or --env) opens the window on the second
# monitor, e.g. while Beyond is using the main one. Default: primary.
export XS_NO_SOUND="${XS_NO_SOUND:-0}" XS_DISC_IMAGE="$DISC" PS2X_VU_CAPTURE=1 PS2X_GS_TRXLOG=1
# VU1 blocks without native code yet (input for tools/vu1gen.py).
export PS2X_VU1_BLOCKLOG="$(cygpath -w "$GAME" 2>/dev/null || echo "$GAME")\vu1_blocks.vbl"
# Extra debug env passes through, e.g. PS2X_GS_TRXDUMP=3a80,38b2 bash port/tools/run_game.sh ...
# Debug switches: pass them with --env; variables set in front of the command
# did not always reach the game.
for kv in "${extra_env[@]}"; do export "$kv"; done
[ -n "$input" ] && export PS2X_INPUT_SCRIPT="$input"
[ -n "$record" ] && export PS2X_INPUT_RECORD="$record"
./XenosagaRecomp.exe "$(cygpath -w "$GAME/SLUS_204.69")" > xenosaga.log 2>&1 &
pid=$!
if [ "$seconds" -gt 0 ]; then
    end=$((SECONDS + seconds))
    while [ $SECONDS -lt $end ] && kill -0 $pid 2>/dev/null; do sleep 1; done
    taskkill //IM XenosagaRecomp.exe //F >/dev/null 2>&1 || true
fi
wait $pid 2>/dev/null || true

out="$ROOT/runs/$name"
mkdir -p "$out"
for f in xenosaga.log xs_frame.bmp xs_vram.bin vu1_capture.bin vu1_capture_*.bin gs_capture.bin xs_profile.txt xs_trx_*.bin xs_vram_after_*.bin xs_texdump* xs_ram.bin xs_spr.bin; do
    [ -f "$f" ] && cp -p "$f" "$out/"
done
# XS_FRAME_KEEP=1 frame series: move it into the run folder.
if [ -d xs_frames ]; then
    rm -rf "$out/frames"; mv xs_frames "$out/frames"
fi
echo "run $name done; results in runs/$name"
grep -E "\[pad-(replay|record)\]" xenosaga.log | head -5 || true
