#!/usr/bin/env bash
# Run the installed port unattended, the same way "Launch Xenosaga.bat" does,
# then copy the results into runs/<name>/.
#
#   bash port/tools/run_game.sh <name> <seconds> [--input script.txt] [--record out.txt]
#
# --input replays a pad script (format: port/inputs/*.txt, checked with
# console-controller's `padmap.py check --script`); --record writes live port-0
# input as such a script. With <seconds> = 0 the game runs until closed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GAME="${XS_GAME_DIR:-<install folder>}"
DISC="${XS_DISC:-<your disc image>}"
name="$1"; seconds="$2"; shift 2
input=""; record=""
while [ $# -gt 0 ]; do
    case "$1" in
        --input) input="$(cygpath -w "$(realpath "$2")")"; shift 2 ;;
        --record) record="$(cygpath -w "$(realpath -m "$2")")"; shift 2 ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
done

taskkill //IM XenosagaRecomp.exe //F >/dev/null 2>&1 || true
cd "$GAME"
rm -f xs_trx_*.bin
export XS_NO_SOUND=1 XS_DISC_IMAGE="$DISC" PS2X_VU_CAPTURE=1 PS2X_GS_TRXLOG=1
# Extra debug env passes through, e.g. PS2X_GS_TRXDUMP=3a80,38b2 bash port/tools/run_game.sh ...
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
for f in xenosaga.log xs_frame.bmp xs_vram.bin vu1_capture.bin xs_trx_*.bin; do
    [ -f "$f" ] && cp -p "$f" "$out/"
done
echo "run $name done; results in runs/$name"
grep -E "\[pad-(replay|record)\]" xenosaga.log | head -5 || true
