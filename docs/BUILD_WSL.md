# Build with Debian 13 WSL2

Build the analyzer and recompiler as native Linux tools, then use MinGW-w64
to configure the Windows x64 runtime in a separate build directory. Keep the
workspace on the WSL filesystem. The Windows installation is outside this
workflow.

## Prepare the workspace

Run the commands from a workspace with this repository checked out as
`port/`. Use a fresh PS2Recomp checkout for the setup commands. For an
existing patched checkout, verify the revision and skip the clone and patch
steps if they have already been applied.

```sh
sudo apt update
sudo apt install build-essential cmake ninja-build git python3 pkgconf nasm \
    zlib1g-dev libzstd-dev gcc-mingw-w64-x86-64-posix \
    g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64

export XS_ROOT="$(pwd -P)"
export XS_PORT="$XS_ROOT/port"
export PS2RECOMP="$XS_ROOT/PS2Recomp"

git clone https://github.com/ran-j/PS2Recomp.git "$PS2RECOMP"
git -C "$PS2RECOMP" checkout --detach e563698
git -C "$PS2RECOMP" apply "$XS_PORT/patches/ps2recomp-runtime.patch"
```

Keep PS2Recomp, the dependency sources and build outputs, disc data, logs,
and all generated code outside the `port/` repository. Do not commit or
share the disc image, extracted files, overlays, generated sources and
headers, or compiled game binaries.

## Build the Linux tools and generate the runtime sources

```sh
cmake -S "$PS2RECOMP" -B "$PS2RECOMP/build-tools" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_RUNTIME=OFF \
    -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF
cmake --build "$PS2RECOMP/build-tools" --target ps2_analyzer ps2_recomp
```

Use your own USA disc image (SLUS-20469). The extraction command calls the
existing launcher extraction helper directly, including its executable SHA-1
check, without invoking the launcher or its Windows installation steps.
Use a dedicated extraction directory because the helper overwrites files
with the same names.

```sh
disc="/path/to/your/disc.iso"
python3 -B - "$disc" "$XS_ROOT/gamefiles" <<'PY'
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(os.environ["XS_PORT"]) / "tools"))
from build_from_iso import extract_game_files

extract_game_files(Path(sys.argv[1]), Path(sys.argv[2]))
PY

python3 "$XS_PORT/tools/setup_recomp.py" "$XS_ROOT/gamefiles" \
    --bin "$PS2RECOMP/build-tools"
python3 "$XS_PORT/tools/assemble.py"
```

The generator processes the main executable plus five overlays. Assembly
refreshes the generated runtime runner sources and headers in PS2Recomp.
Keep any hand edits outside those generated locations.

## Prepare Windows FFmpeg 7.1

Unpack the [FFmpeg 7.1 release](https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz)
into a clean directory outside `port/`. The helper builds shared MinGW-w64
libraries and verifies the five pkg-config modules used by the runtime.
Use a dedicated Windows install prefix.

```sh
ffmpeg_prefix="$XS_ROOT/deps/ffmpeg-win64"
JOBS=4 "$XS_PORT/tools/build_ffmpeg_wsl.sh" "$XS_ROOT/deps/ffmpeg-7.1" "$ffmpeg_prefix"

unset PKG_CONFIG_SYSROOT_DIR
export PKG_CONFIG_PATH=
export PKG_CONFIG_LIBDIR="$ffmpeg_prefix/lib/pkgconfig"
pkg-config --modversion libavcodec libavformat libavutil libswresample libswscale
```

Use the target pkg-config environment for both runtime configuration and
build. For an existing compatible FFmpeg install, set `ffmpeg_prefix` and
run the environment and verification commands directly.

## Configure the Windows runtime

Use a fresh build directory when switching toolchains. CMake downloads
raylib, ImGui and rlImGui on first configure. For an offline configure with
matching sources already available, supply the absolute source directories
using `FETCHCONTENT_SOURCE_DIR_RAYLIB`, `FETCHCONTENT_SOURCE_DIR_IMGUI` and
`FETCHCONTENT_SOURCE_DIR_RLIMGUI`.

Add the FFmpeg install prefix to `CMAKE_FIND_ROOT_PATH` so CMake can resolve
the Windows import libraries. With only the pkg-config environment set,
configuration passed but MinGW could not find the five FFmpeg libraries at
link time.

```sh
win_build="$PS2RECOMP/build-wsl-win64"
cmake -S "$PS2RECOMP" -B "$win_build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$XS_PORT/cmake/mingw-w64-x86_64.cmake" \
    -DCMAKE_FIND_ROOT_PATH="$ffmpeg_prefix" \
    -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_RUNTIME=ON \
    -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF \
    -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF \
    -DPS2X_FFMPEG_USE_PKGCONFIG=ON -DPS2X_DISABLE_IPO=ON \
    -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF \
    '-DCMAKE_CXX_FLAGS=-msse4.2 -mavx2 -Wa,-mbig-obj'

cmake --build "$win_build" --target ps2EntryRunner --parallel 4
```

The Windows target requires AVX2. See [VU1 validation](VU1_PERFORMANCE.md)
for the attended baseline evidence and its scope.

## Validation

- The Debian 13 workspace already built the Linux analyzer and recompiler,
  extracted the disc files, generated 8,162 C++ files, and assembled the
  runtime sources. The executable SHA-1 and generated file count were
  independently checked.
- The FFmpeg helper built and installed shared Windows x64 FFmpeg 7.1
  libraries from clean sources. A MinGW-w64 smoke test linked against
  all five required pkg-config modules.
- A fresh runtime-only CMake configuration and Ninja generation passed with
  GCC 14 POSIX and cached sources for raylib, ImGui and rlImGui. CMake
  reported development warnings for deprecated FetchContent calls and a
  warning about IPO. IPO was disabled for this build.
- With the FFmpeg prefix added to the target search roots, MinGW-w64 linked
  `ps2EntryRunner.exe` as a Windows x64 PE executable.

## Incremental developer build

After generation and assembly, run `port/tools/build_runtime_wsl.sh` from the
workspace root. It checks the PS2Recomp revision, tool executables, retail ELF
fingerprint, overlay presence, generated header, free space, and FFmpeg
metadata. The default output is `runs/build-wsl-validation`. Override
`BUILD_DIR`, `FFMPEG_PREFIX`, `GAMEFILES` or `PS2RECOMP` for other layouts.
For offline builds set `FETCH_SOURCES` to an existing FetchContent directory
containing the matching raylib-src, imgui-src and rlimgui-src directories.
`JOBS` defaults to four and accepts one through sixteen.

The patched MinGW pre-link step modifies input archives, so a second build
may relink the runner. Unchanged translation units stay cached. The wrapper
disables PE linker timestamps for repeatable relinks of the same objects.

## Stage a private Windows test build

Staging requires LLVM's `llvm-readobj` (Debian package `llvm`). In the workspace
run `python3 port/tools/stage_wsl.py runs/build-wsl-validation/ps2xRuntime/ps2EntryRunner.exe`.
The final line gives the new or verified stage under `runs/`. The script checks
x64 PE files and named imports against non-system DLL exports. Windows system
DLLs come from the OS. The stage contains the verified private ELF, executable,
DLLs, launcher and SHA-256 manifest. Keep it local.

Use `wslpath -w <stage-path>` to get its Windows path. In Windows Command
Prompt, set `XS_DISC` to the absolute filename of your USA ISO, run
`pushd "<Windows stage path>"`, then `call "Launch Xenosaga.bat"`. On exit,
run `popd`. The launcher writes its log and settings in the test directory.
Verify Windows loader and driver initialization and gameplay on the staged build.
