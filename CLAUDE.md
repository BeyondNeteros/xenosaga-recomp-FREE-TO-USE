# Xenosaga Episode I: Recompiled — notes for Claude Code

This is Beyond's unofficial native PC port of *Xenosaga Episode I* (PS2, USA,
SLUS-20469). It is built by statically recompiling the original game with
[PS2Recomp](https://github.com/ran-j/PS2Recomp). It was started in a cloud
session and moved here so builds, runs, and logs all happen on this PC.

Read `NOTES.md` for the current investigation before changing anything.

## Hard rules

- **Never commit or distribute game files or anything derived from them**: the
  ISO, `SLUS_204.69`, `OV*.OVL`, IRX modules, generated C++ (`recomp/`), VU
  captures, frame dumps. `.gitignore` covers the usual names.
- Don't permanently delete Beyond's files. Use the Recycle Bin or move to a
  `_to_delete` folder.
- End commit messages with the attribution lines Claude Code normally adds.
- After each commit, push to the private GitHub repo: `git push github`
  (https://github.com/BeyondNeteros/xenosaga-recomp; `origin` is the old
  bundle). Only this repo goes up, never game data.

## Layout (project root = the folder containing `port/`)

```
<root>/
  port/          this git repo (port code, launcher, tools, runtime patch)
  PS2Recomp/     PS2Recomp checkout at commit e563698 + patches/ps2recomp-runtime.patch
  recomp/        generated C++ (tools/setup_recomp.py) - never commit
  runs/          logs/captures from test runs - never commit
```

The playable install lives at `<install folder>`
(`Launch Xenosaga.bat`, `XenosagaRecomp.exe`, `SLUS_204.69`, `xenosaga.log`,
`xs_frame.bmp`). The ISO is
`<your disc image>`
(Redump, DVD9 dual layer).

## Setup (once)

1. Clone PS2Recomp into `<root>\PS2Recomp`, check out `e563698`, then
   `git apply ..\port\patches\ps2recomp-runtime.patch`.
2. Toolchain: the build that works is MinGW-w64 GCC (it was cross-compiled from
   Linux). On Windows, use MSYS2's MINGW64 environment: `mingw-w64-x86_64-gcc`,
   `cmake`, `ninja`, `pkgconf`, `ffmpeg`, `python`. MSVC may also work, but the
   patch's Windows tweaks (objcopy symbol rename for the raylib/user32
   `CloseWindow`/`ShowCursor` clash, pkg-config FFmpeg) target MinGW.
3. Build the tools (`ps2_analyzer`, `ps2_recomp`), then generate the game code
   from Beyond's own files (the folder needs `SLUS_204.69` and `OV01/02/10/11/12.OVL`
   from the disc root; the install folder already has `SLUS_204.69`; the OVLs can be
   pulled from the ISO, e.g. with 7-Zip):
   `python port/tools/setup_recomp.py <game files folder> --bin PS2Recomp/<build dir>`
4. `python port/tools/assemble.py` copies the generated code, the renamed
   overlay code (`xs_ovNN_` prefixes) and `port/src/*` into
   `PS2Recomp/ps2xRuntime/src/runner` and `include`.
5. Configure and build the runtime, target `ps2EntryRunner`:
   `-DCMAKE_BUILD_TYPE=Release -DPS2X_DISABLE_IPO=ON -DPS2X_FFMPEG_USE_PKGCONFIG=ON
   -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DCMAKE_CXX_FLAGS="-msse4.2 -mavx2 -Wa,-mbig-obj"`.
   Optional `-DPS2X_BUILD_VU_REPLAY=ON` builds `vu1_replay` (offline VU1 replayer).
   Leave aggressive logs off; they produce gigabyte logs and crawl.
6. Copy `ps2EntryRunner.exe` to the install folder as `XenosagaRecomp.exe`
   (plus any MinGW/FFmpeg DLLs it needs if not linked statically).

### Windows build on Beyond's PC (works as of 2026-10-31)

MSYS2 is at `C:\msys64` (installed with `winget install MSYS2.MSYS2`). From Git
Bash, put `/c/msys64/mingw64/bin:/c/msys64/usr/bin` first on `PATH` (Git for
Windows ships its own `/mingw64`; don't use that one). Folders used:

- `<root>/gamefiles/` Beyond's `SLUS_204.69`, `OV*.OVL`, `SYSTEM.CNF` (extracted
  from the ISO with `C:\Program Files\7-Zip\7z.exe e`). Never commit.
- `PS2Recomp/build-tools/` tools only: `-G Ninja -DCMAKE_BUILD_TYPE=Release
  -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF`, then
  `cmake --build build-tools --target ps2_recomp ps2_analyzer`.
- `PS2Recomp/build-runtime/` runtime: `-DPS2X_BUILD_RECOMP=OFF
  -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF` plus
  the step-5 flags and `-DPS2X_BUILD_VU_REPLAY=ON`; build `ps2EntryRunner
  vu1_replay`. Full build is ~368 ninja steps, about 15 minutes on 24 threads.
- `runs/build-logs/` configure/build logs.
- Install with `bash port/tools/install.sh`: it renames the old exe to
  `XenosagaRecomp.exe.bak-<timestamp>`, copies the new one and the MSYS2 DLLs it
  imports (82 of them, mostly FFmpeg codecs; a static or trimmed FFmpeg
  would cut this down).

Windows fixes so far: `setup_recomp.py` needed absolute tool paths (it runs
the tools with cwd set to the output folder). The patch's MinGW tweaks
(objcopy rename, pkg-config FFmpeg) worked unchanged with GCC 16.2.

Recompiler fix in the patch: `VCALLMSR` reads CMSAR0 (`ctx->vu0_cmsar0`), not
`vi[27]`. Without it no VU0 microprogram started from EE code ever ran (see
NOTES.md). Changing `ps2xRecomp` means rebuilding the tools and regenerating
the code (`setup_recomp.py` + `assemble.py`) before the runtime build.

`tools/eedis.py <elf> <symbol|0xaddr> [count]` disassembles EE code from the
ELF by symbol (`--sym text` lists symbols); needs `pip install capstone
pyelftools`. Capstone decodes PS2 `lq`/`sq` (opcodes 0x1e/0x1f) as MSA junk and
COP2 macro ops as `.word`; read those from the raw word.

A full build compiles ~7,500 generated files and is slow. Changing a header
included by `ps2_runtime.h` (for example `runtime/ps2_vu1.h`) or adding a new
source file rebuilds everything; editing one runtime `.cpp` is quick.

Port code lives in `port/src` and is copied by `assemble.py`. After editing a
port file, copy it to `PS2Recomp/ps2xRuntime/src/runner` (or rerun assemble.py).
Runtime changes go straight into `PS2Recomp`; afterwards refresh the patch with
`git -C PS2Recomp diff -- . ":(exclude)ps2xRuntime/src/runner" > port/patches/ps2recomp-runtime.patch`
(new files need `git -C PS2Recomp add -N <file>` first or they are left out).

## Running and debugging

- `Launch Xenosaga.bat` sets `XS_NO_SOUND=1` (sound driver RPC not working yet),
  `XS_DISC_IMAGE` (the ISO), `PS2X_VU_CAPTURE=1`, and logs to `xenosaga.log`.
- Every 5 s the heartbeat (`src/xenosaga_debug.cpp`) logs EE cycles, host time
  for VU1/GS/VIF1, the top VU1 programs (`vu1 prog 0x...`; 0x8xxx = resumed at
  that pc), GS display registers, every guest thread's pc/function, and saves
  the current frame to `xs_frame.bmp`.
- When VU1 dominates, 300 VU1 calls are written to `vu1_capture.bin`. Replay
  offline with `vu1_replay vu1_capture.bin [repeat]`; env `VU_REPLAY_DETAIL=1`
  (per call), `VU_REPLAY_GIFDUMP=out.bin` (GIF output), `VU_REPLAY_INVALIDATE=1`,
  and `PS2X_VU_KICKLOG=1` (XGKICK addresses).
- `bash port/tools/vu1_check.sh` replays every `runs/*/vu1_capture.bin` through the
  current vu1_replay and a frozen reference (`PS2Recomp-ref/`: a git worktree of
  PS2Recomp at e563698 + the runtime patch from port commit a9b3cd4, built into
  `PS2Recomp-ref/build`) and compares checksums and GIF output. Run it after any
  VU1 change. `runs/prof/prof.sh <capture>` profiles vu1_replay.
- VU1 native blocks: `python port/tools/vu1gen.py runs/vu1blocks/*.vbl "<install folder>/vu1_blocks.vbl"`
  regenerates the C++ for every VU1 block logged so far (see NOTES.md); then
  reconfigure and rebuild. Never commit the logs or the generated file.
- VU1 native programs (whole calls, 2.1-2.4x faster than blocks):
  `python port/tools/vu1prog.py runs/*/vu1_capture.bin --min-calls 50`, then
  rebuild; check with `VU_REPLAY_NATIVE_CMP=1 vu1_replay <capture> 1` and
  vu1_check.sh. `PS2X_VU1_PROGRAMS=0` disables them. Never commit the output.
- PGO: `bash port/tools/pgo.sh` profiles the hot VU1/VIF1/GS code on the
  captures and rebuilds build-runtime with it (~11% faster VU1). Rerun it
  after VU1/GS changes or regenerating native blocks, then vu1_check.sh.
- `port/tools/vudis.py capture.bin <record> <start> <end>` disassembles VU1
  microcode from a capture; `port/tools/gifparse.py gif.bin` summarizes GIF
  packets (tags, AD registers, primitive types, vertex ranges).
- The current-matrix tracer prints the last VU0 matrix operations whenever
  `ps2__CurMatrixGet` returns a bad matrix (see NOTES.md).
- **Unattended runs:** `bash port/tools/run_game.sh <name> <seconds> --input
  port/inputs/newgame-to-tutorial.txt` boots the installed exe with the launcher's
  env, replays a pad script and copies log/frame/VRAM dump to `runs/<name>/`.
  `--env PS2X_WINDOW_MONITOR=1` puts the window on the second monitor.
  run_game.sh force-closes any running game: tell Beyond before runs.
  That script reaches the "Virtual Tutorial" 3D scene in about 3 minutes with
  nobody at the PC. `--record file.txt` writes live controller input as a new
  script. Replay/record live in the runtime's `ps2_pad.cpp` (code adapted from the
  console-controller skill; stock key/pad mapping unchanged); scripts are
  `frame buttons [lx ly rx ry]` where frame = port-0 pad reads, and
  `padmap.py check --script` (console-controller skill) validates them.
  The game reads only the controller while one is connected (keyboard ignored).
- Screen capture of the game window shows black (GL); read `xs_frame.bmp`.

## Key game facts

- Overlays all load at 0xA00000; `xglCdLoadOverlay` (0x21F5A0) is hooked to swap
  the `xs_ovNN_` function tables (`src/xenosaga_overlays.cpp`).
- Game data is read by absolute sector across `XENOSAGA.xx` archives; disc file
  lookups are answered from the ISO's ISO 9660 directory (`src/xenosaga_disc.cpp`,
  `src/xenosaga_iso.h`); layer 1 starts at sector 2077312 on the Redump image.
- Display: dispEnv at RDRAM 0x4A9000; sticky DispOn fix for a sound-skip race.
- VU1 microcode is uploaded with MPG every frame; the main 3D program resumes
  at 0x188 (batch loop) and builds GIF tags from a template at VU1 qword 1011.
  The world-to-clip matrix is VU1 qwords 962-965 (uploaded in
  `nmlPacketAddParts`, 0x238454, from `ps2__CurMatrixGet` = VU0 vf27-vf30).
