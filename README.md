# Xenosaga Episode I: Recompiled (work in progress)

An unofficial native PC port of *Xenosaga Episode I: Der Wille zur Macht* (PS2, USA, SLUS-20469),
built by statically recompiling the original game code with [PS2Recomp](https://github.com/ran-j/PS2Recomp).

**This repository contains no game code or data.** You must own the game and supply your own disc files.

## Status

Playable from the start through the Virtual Tutorial and into the first field
areas, at a locked 30 fps in the tutorial.

| Part | State |
|---|---|
| Main executable (`SLUS_204.69`) | Recompiles cleanly |
| Overlays (`OV01/02/10/11/12.OVL`) | Recompile cleanly; swapped in at runtime |
| Boot, disc access (dual-layer ISO) | Working |
| Menus, text, FMVs, 3D scenes | Rendering |
| Battles | Tutorial battle plays through; field battles being tested |
| Sound and music | Working (game's own sound driver on an emulated SPU2) |
| Speed | 30 fps in the tutorial; first field area ~20 fps until a battle |
| Display | 4:3 / 16:9 (experimental), fullscreen, sharp scaling, screenshots |

## Getting the game running (players)

This download contains no game code. You need your own copy of
**Xenosaga Episode I (USA, SLUS-20469)** as a disc image (.iso), and a PC
with Windows 10/11, a CPU from 2013 or newer (AVX2), and about 10 GB free.

1. Double-click **`Xenosaga Launcher.bat`**.
2. **Choose disc image...** and pick your .iso. The launcher checks it is
   the right disc.
3. **Install build tools** (first time only): installs MSYS2, a free
   compiler toolkit, through Windows' winget (about 1.5 GB).
4. **Build the game**: copies the game program off your disc, downloads
   [PS2Recomp](https://github.com/ran-j/PS2Recomp), recompiles the game into
   C++ and builds it. 15-30 minutes, once. Everything stays on your PC
   (`work/` and `game/` next to the launcher).
5. **PLAY**. Your disc image stays where it is; the game reads it directly.

**Optimize** (after you've played a while): the game notes the graphics
routines it runs, and Optimize builds fast versions of them in (a few
minutes). **Settings** opens `settings.ini`.

Want a picture behind the launcher? Put it at `launcher/background.png`.

## Playing

Start the game with the launcher's **PLAY** (a hand-made install uses
`dist/Launch Xenosaga.bat`). On first start it writes `settings.ini` next to
the exe; the launcher's **Settings** opens it.

| Key | Does |
| --- | --- |
| F11 or Alt+Enter | Borderless fullscreen on/off |
| F3 | Frame-rate counter |
| F4 | Scaling filter: sharp, smooth, pixel |
| F5 | Picture shape: 4:3, 16:9 widescreen, stretch, native |
| F12 | Screenshot (into the `screenshots` folder) |

- **Picture:** shown at the original TV shape (4:3) by default, scaled with a
  "sharp bilinear" filter (crisp, without uneven pixels). `16:9` widens the
  3D view; menus and the HUD are stretched with it (experimental).
- **Controls:** any controller and the keyboard work at the same time.
  Sticks get a small deadzone so worn sticks don't drift; analog triggers
  count as L2/R2. Keyboard: arrows/WASD move, X/Space = Cross, C/Esc/Backspace
  = Circle, Z = Square, V = Triangle, Q/E = L1/R1, Shift = L2/R2, Enter =
  Start, Tab = Select. Escape no longer quits the game.
- **Sound:** `volume` in settings.ini; the game goes quiet while another
  window is in front (`mute_when_unfocused`).
- The window remembers its size; the mouse pointer hides after two seconds.
- **Gameplay options** (`[gameplay]` in settings.ini): battle fast-forward
  (press Start in a battle; on by default), hide the battle screen flash,
  faster combos, and swap confirm/cancel (Circle confirms).

## How it works

- The retail executable and overlays still contain their original function names, so every
  recompiled function keeps its real name (`scParseScript`, `ACT_draw`, `command_mpeg2`, ...).
- The game loads five code overlays into the same address (`0xA00000`) through
  `xglCdLoadOverlay`. Each overlay is recompiled separately with an `xs_ovNN_` prefix, and
  `src/xenosaga_overlays.cpp` replaces the loader so the right overlay's functions are active.

## Layout

- `src/` port-specific code (hooks, overrides)
- `tools/assemble.py` merges the generated main and overlay code into the PS2Recomp runtime build

## Building (developer notes)

See `CLAUDE.md` for the full setup. In short:

1. Dump your own disc. Copy `SYSTEM.CNF`, `SLUS_204.69`, and `OV*.OVL` from its root into a folder.
2. Run `ps2_analyzer` and `ps2_recomp` on `SLUS_204.69` (output in `recomp/output`).
3. For each overlay, set its ELF entry point to its first function (the retail files carry the main
   executable's entry), then analyze and recompile it into `recomp/ovNN/output`.
4. `python3 tools/assemble.py`, then build the `ps2EntryRunner` target of PS2Recomp.
5. Run `ps2EntryRunner path/to/SLUS_204.69`.
