#!/usr/bin/env bash
# Profile-guided optimisation of the hot VU1/VIF1/GS runtime code.
#   bash port/tools/pgo.sh            # profile + rebuild the game (and vu1_replay)
#
# 1. Builds an instrumented vu1_replay in PS2Recomp/build-pgo (PS2X_PGO_GENERATE).
# 2. Replays every runs/*/vu1_capture.bin through it to record the profile.
# 3. Copies the .gcda profiles next to the matching objects in build-runtime
#    and rebuilds there with PS2X_PGO_USE=ON (game + vu1_replay).
# Same output (check with tools/vu1_check.sh afterwards). Rerun after VU1/GS
# code changes or regenerating the native blocks; stale profiles are ignored
# per function, not an error.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
RECOMP="$ROOT/PS2Recomp"
GEN="$RECOMP/build-pgo"
USE="$RECOMP/build-runtime"
FLAGS="-msse4.2 -mavx2 -Wa,-mbig-obj"

cmake -S "$RECOMP" -B "$GEN" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF \
    -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_DISABLE_IPO=ON -DPS2X_FFMPEG_USE_PKGCONFIG=ON \
    -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_BUILD_VU_REPLAY=ON -DPS2X_PGO_GENERATE=ON -DPS2X_PGO_USE=OFF \
    -DCMAKE_CXX_FLAGS="$FLAGS" > /dev/null
cmake --build "$GEN" --target vu1_replay | tail -1
find "$GEN" -name '*.gcda' -delete # old profiles (build output)

echo "training on captures..."
for cap in "$ROOT"/runs/*/vu1_capture.bin; do
    VU_REPLAY_NOGS=1 "$GEN/ps2xRuntime/vu1_replay.exe" "$cap" 2 > /dev/null 2>&1 || true
done

copied=0
while IFS= read -r gcda; do
    rel="${gcda#$GEN/}"
    case "$rel" in
    */vu1_replay.dir/src/runner/xenosaga_vu1_blocks.cpp.gcda)
        for t in ps2EntryRunner vu1_replay; do
            dst="$USE/ps2xRuntime/CMakeFiles/$t.dir/src/runner/xenosaga_vu1_blocks.cpp.gcda"
            mkdir -p "$(dirname "$dst")"; cp "$gcda" "$dst"; rm -f "${dst%.gcda}.obj"
        done ;;
    */ps2_runtime.dir/*)
        dst="$USE/$rel"; mkdir -p "$(dirname "$dst")"; cp "$gcda" "$dst"; rm -f "${dst%.gcda}.obj" ;;
    *) continue ;;
    esac
    copied=$((copied + 1))
done < <(find "$GEN" -name '*.gcda')
echo "copied $copied profiles"

cmake "$USE" -DPS2X_PGO_USE=ON -DPS2X_PGO_GENERATE=OFF > /dev/null
cmake --build "$USE" --target ps2EntryRunner vu1_replay | tail -1
