# Xenosaga Episode I: Recompiled (work in progress)

An unofficial native PC port of *Xenosaga Episode I: Der Wille zur Macht* (PS2, USA, SLUS-20469),
built by statically recompiling the original game code with [PS2Recomp](https://github.com/ran-j/PS2Recomp).

**This repository contains no game code or data.** You must own the game and supply your own disc files.

## Status

Exploring: the game code recompiles; the first boot tests are in progress.

| Part | State |
|---|---|
| Main executable (`SLUS_204.69`) | Recompiles cleanly (4,526 functions) |
| Overlays (`OV01/02/10/11/12.OVL`) | Recompile cleanly; swapped in at runtime |
| Boot | In progress |
| Graphics, audio, FMV | Not started |

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

1. Dump your own disc. Copy `SYSTEM.CNF`, `SLUS_204.69`, and `OV*.OVL` from its root into a folder.
2. Run `ps2_analyzer` and `ps2_recomp` on `SLUS_204.69` (output in `recomp/output`).
3. For each overlay, set its ELF entry point to its first function (the retail files carry the main
   executable's entry), then analyze and recompile it into `recomp/ovNN/output`.
4. `python3 tools/assemble.py`, then build the `ps2EntryRunner` target of PS2Recomp.
5. Run `ps2EntryRunner path/to/SLUS_204.69`.
