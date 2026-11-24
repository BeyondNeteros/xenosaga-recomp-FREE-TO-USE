#!/usr/bin/env bash
# Build shared Windows x64 FFmpeg libraries from a clean FFmpeg 7.1 source tree.
set -euo pipefail

if [[ $# != 2 ]]; then
    printf 'Usage: %s <clean FFmpeg 7.1 source directory> <install prefix>\n' "$0" >&2
    exit 2
fi

source_dir=$(realpath -e -- "$1")
prefix=$(realpath -m -- "$2")
if [[ ! -x "$source_dir/configure" || ! -f "$source_dir/RELEASE" ]] ||
   [[ $(<"$source_dir/RELEASE") != 7.1 || -f "$source_dir/config.h" ]]; then
    printf 'Use a clean FFmpeg 7.1 source tree without an in-tree build.\n' >&2
    exit 2
fi

build_dir=$(mktemp -d "${TMPDIR:-/tmp}/xenosaga-ffmpeg.XXXXXX")
printf 'FFmpeg build directory (retained for inspection): %s\n' "$build_dir"
cd "$build_dir"

"$source_dir/configure" \
    --prefix="$prefix" \
    --target-os=mingw32 --arch=x86_64 --enable-cross-compile \
    --cross-prefix=x86_64-w64-mingw32- --cc=x86_64-w64-mingw32-gcc-posix \
    --enable-shared --disable-static --disable-programs --disable-doc \
    --disable-debug --disable-autodetect
make -j "${JOBS:-$(nproc)}"
make install

env -u PKG_CONFIG_SYSROOT_DIR PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig" \
    pkg-config --modversion libavcodec libavformat libavutil libswresample libswscale
