#!/usr/bin/env bash
# Profile-guided optimisation of the hot VU1/VIF1/GS runtime code.
#   bash port/tools/pgo.sh game     # train on a real game run (recommended)
#   bash port/tools/pgo.sh          # train on the VU1 captures only (faster)
#
# game: rebuilds the PGO sources in build-runtime instrumented
#   (PS2X_PGO_GENERATE), installs that exe, plays the tutorial script with
#   nobody at the PC (run_game.sh, XS_PGO_DUMP makes the heartbeat write the
#   profile next to the objects), then rebuilds with PS2X_PGO_USE=ON and
#   installs the optimised exe. ~10 min.
# captures: builds an instrumented vu1_replay in PS2Recomp/build-pgo, replays
#   every runs/*/vu1_capture.bin, copies the .gcda files next to the matching
#   objects in build-runtime and rebuilds there with PS2X_PGO_USE=ON.
# Same output either way (check with tools/vu1_check.sh). Rerun after VU1/GS
# code changes or regenerating the native blocks.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
RECOMP="$ROOT/PS2Recomp"
USE="$RECOMP/build-runtime"
FLAGS="-msse4.2 -mavx2 -Wa,-mbig-obj"
BLOCKS_GCDA="ps2xRuntime/CMakeFiles/%s.dir/src/runner/xenosaga_vu1_blocks.cpp.gcda"

pgo_objects() # objects of the PGO sources in build-runtime
{
    find "$USE/ps2xRuntime/CMakeFiles/ps2_runtime.dir" \( -path '*lib/vu/ps2_vu1_*.obj' -o -name 'ps2_vif1_interpreter.cpp.obj' \
        -o -name 'ps2_memory.cpp.obj' -o -path '*lib/gs/*.obj' \) 2>/dev/null
    for t in ps2EntryRunner vu1_replay; do
        printf "$USE/ps2xRuntime/CMakeFiles/%s.dir/src/runner/xenosaga_vu1_blocks.cpp.obj\n" "$t"
    done
}

if [ "${1:-}" = "game" ]; then
    find "$USE" -name '*.gcda' -delete # old profiles (build output)
    cmake "$USE" -DPS2X_PGO_GENERATE=ON -DPS2X_PGO_USE=OFF > /dev/null
    cmake --build "$USE" --target ps2EntryRunner | tail -1
    bash "$ROOT/port/tools/install.sh" | tail -1
    # PGO_INPUT / PGO_SECONDS / PGO_DUMP pick another script (e.g. a recorded
    # session with battles), how long it runs and when the profile is written.
    echo "training: playing ${PGO_INPUT:-the tutorial script} (instrumented build)..."
    bash "$ROOT/port/tools/run_game.sh" pgo-train "${PGO_SECONDS:-300}" \
        --input "${PGO_INPUT:-$ROOT/port/inputs/newgame-to-tutorial.txt}" \
        --env XS_PGO_DUMP="${PGO_DUMP:-160}" > /dev/null 2>&1 || true
    grep -q "PGO profile written" "$ROOT/runs/pgo-train/xenosaga.log" || { echo "no profile written"; exit 1; }
    src="$USE/$(printf "$BLOCKS_GCDA" ps2EntryRunner)"
    dst="$USE/$(printf "$BLOCKS_GCDA" vu1_replay)"
    [ -f "$src" ] && { mkdir -p "$(dirname "$dst")"; cp "$src" "$dst"; }
else
    GEN="$RECOMP/build-pgo"
    cmake -S "$RECOMP" -B "$GEN" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF \
        -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_DISABLE_IPO=ON -DPS2X_FFMPEG_USE_PKGCONFIG=ON \
        -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_BUILD_VU_REPLAY=ON -DPS2X_PGO_GENERATE=ON -DPS2X_PGO_USE=OFF \
        -DCMAKE_CXX_FLAGS="$FLAGS" > /dev/null
    cmake --build "$GEN" --target vu1_replay | tail -1
    find "$GEN" -name '*.gcda' -delete
    echo "training on captures..."
    for cap in "$ROOT"/runs/*/vu1_capture.bin; do
        VU_REPLAY_NOGS=1 "$GEN/ps2xRuntime/vu1_replay.exe" "$cap" 2 > /dev/null 2>&1 || true
    done
    while IFS= read -r gcda; do
        rel="${gcda#$GEN/}"
        case "$rel" in
        */vu1_replay.dir/src/runner/xenosaga_vu1_blocks.cpp.gcda)
            for t in ps2EntryRunner vu1_replay; do
                dst="$USE/$(printf "$BLOCKS_GCDA" "$t")"; mkdir -p "$(dirname "$dst")"; cp "$gcda" "$dst"
            done ;;
        */ps2_runtime.dir/*)
            dst="$USE/$rel"; mkdir -p "$(dirname "$dst")"; cp "$gcda" "$dst" ;;
        esac
    done < <(find "$GEN" -name '*.gcda')
fi
echo "profiles: $(find "$USE" -name '*.gcda' | wc -l)"

# Ninja does not see .gcda changes: drop the PGO objects so they recompile.
pgo_objects | while IFS= read -r obj; do rm -f "$obj"; done
cmake "$USE" -DPS2X_PGO_USE=ON -DPS2X_PGO_GENERATE=OFF > /dev/null
cmake --build "$USE" --target ps2EntryRunner vu1_replay | tail -1
bash "$ROOT/port/tools/install.sh" | tail -1
