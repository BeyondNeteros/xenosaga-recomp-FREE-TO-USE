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

A full build compiles ~7,500 generated files and is slow. Changing a header
included by `ps2_runtime.h` (for example `runtime/ps2_vu1.h`) or adding a new
source file rebuilds everything; editing one runtime `.cpp` is quick.

Port code lives in `port/src` and is copied by `assemble.py`. After editing a
port file, copy it to `PS2Recomp/ps2xRuntime/src/runner` (or rerun assemble.py).
Runtime changes go straight into `PS2Recomp`; afterwards refresh the patch with
`git -C PS2Recomp diff -- . ":(exclude)ps2xRuntime/src/runner" > port/patches/ps2recomp-runtime.patch`.

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
- `port/tools/vudis.py capture.bin <record> <start> <end>` disassembles VU1
  microcode from a capture; `port/tools/gifparse.py gif.bin` summarizes GIF
  packets (tags, AD registers, primitive types, vertex ranges).
- The current-matrix tracer prints the last VU0 matrix operations whenever
  `ps2__CurMatrixGet` returns a bad matrix (see NOTES.md).
- Beyond has to drive the game (New Game, skipping FMVs); Claude can launch it
  and read the log and frame.

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
