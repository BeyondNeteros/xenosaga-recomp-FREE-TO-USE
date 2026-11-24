#!/usr/bin/env bash
# Configure and incrementally build the assembled runtime in a private workspace.
set -euo pipefail
port=$(cd "$(dirname "$0")/.." && pwd -P)
root=$(dirname "$port")
src=${PS2RECOMP:-$root/PS2Recomp}
build=${BUILD_DIR:-$root/runs/build-wsl-validation}
prefix=$(realpath -e "${FFMPEG_PREFIX:-$root/deps/ffmpeg-win64}")
jobs=${JOBS:-4}
[[ $jobs =~ ^([1-9]|1[0-6])$ ]] || { echo 'JOBS must be between 1 and 16' >&2; exit 2; }
for tool in cmake ninja git pkg-config sha1sum x86_64-w64-mingw32-{gcc-posix,g++-posix,windres,objcopy}; do
    command -v "$tool" >/dev/null || { echo "Missing $tool" >&2; exit 1; }
done
revision=$(git -C "$src" rev-parse --verify HEAD)
if [[ $revision != e5636981a1c9de368f6b71e819c3dc188dfedbf4 ]]; then
    echo 'Expected PS2Recomp revision e563698' >&2; exit 1
fi
if [[ -f "$build/CMakeCache.txt" ]] && [[ $(sed -n 's/^CMAKE_TOOLCHAIN_FILE:[^=]*=//p' "$build/CMakeCache.txt") != "$port/cmake/mingw-w64-x86_64.cmake" ]]; then
    echo 'Use a separate directory for this Windows toolchain' >&2; exit 1
fi
gamefiles=${GAMEFILES:-$root/gamefiles}
(cd "$gamefiles" && echo 'fd206d5715a322830f7fa9285fb4a09276ac2a63  SLUS_204.69' | sha1sum -c -)
for input in OV{01,02,10,11,12}.OVL; do
    [[ -s "$gamefiles/$input" ]] || { echo "Missing $input" >&2; exit 1; }
done
[[ -s "$src/ps2xRuntime/include/ps2_recompiled_functions.h" ]] || { echo 'Run assemble.py first' >&2; exit 1; }
mkdir -p "$build"
df -h "$build"
if [[ ! -f "$build/build.ninja" && $(df -Pk "$build" | awk 'END {print $4}') -lt 1048576 ]]; then
    echo 'Allow at least 1 GiB for a fresh runtime build' >&2; exit 1
fi
export PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
unset PKG_CONFIG_SYSROOT_DIR
pkg-config --modversion libavcodec libavformat libavutil libswresample libswscale
args=()
if [[ -n ${FETCH_SOURCES:-} ]]; then
    for dep in raylib imgui rlimgui; do
        dir=$(realpath -e "$FETCH_SOURCES/$dep-src")
        args+=("-DFETCHCONTENT_SOURCE_DIR_${dep^^}=$dir")
    done
fi
cmake -S "$src" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE:FILEPATH="$port/cmake/mingw-w64-x86_64.cmake" \
    -DCMAKE_FIND_ROOT_PATH="$prefix" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXE_LINKER_FLAGS=-Wl,--no-insert-timestamp \
    -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF \
    -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_RUNTIME=ON -DPS2X_DISABLE_IPO=ON \
    -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_FFMPEG_USE_PKGCONFIG=ON \
    '-DCMAKE_CXX_FLAGS=-msse4.2 -mavx2 -Wa,-mbig-obj' "${args[@]}"
cmake --build "$build" --target ps2EntryRunner --parallel "$jobs"
