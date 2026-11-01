
## 3D black screen: root cause found (2026-10-31, run4)

The run4 matrix trace (runs/run4/xenosaga.log) showed the matrix that
nmlPacketAddParts loads from `camera+0x470` is a correct matrix shifted up one
row: (0 1 0 0) (0 0 1 0) (0.98 1.01 -2.4 1) (garbage).

- `camera+0x470` is written by `xglCameraViewVolume` with the matrix-stack
  helpers: `xglMatrixStackUnit/Frustum/Mul` then `xglMatrixStackSave`
  (0x22a590), which is just `sqc2 vf28..vf31`.
- The matrix stack lives in VU0 vf28-vf31 and every operation is a VU0
  microprogram (microcode at `_$Vu0MicroCodeStart`, 0x4ade90; symbols
  `Vu0MatrixStack*`). The EE side starts them with `ctc2 vN, vi27` (CMSAR0)
  followed by `vcallmsr`.
- **Bug:** PS2Recomp translated `VCALLMSR` as `ctx->vi[inst.rd]`, i.e.
  `vi[27]` on a 16-entry array, reading bytes of `vu0_r` instead of CMSAR0.
  The call landed at address 0 (`nop[E]`) so no matrix-stack program ever ran.
  vf28-30 were left holding rows 1-3 of the "current matrix" (vf27-30, loaded
  by CurMatrixSet) and vf31 stale data, which is exactly the shifted matrix.
- Fix: `translateVU_VCALLMSR` now uses `ctx->vu0_cmsar0` (in the runtime patch,
  `ps2xRecomp/src/lib/vu_translation_helpers.cpp`). 30 call sites in the main
  executable are affected; regenerating the code picks it up.
- Not yet verified in game: needs a test run (New Game, skip FMVs).

Side note: the generator also emits `vi[21]`/`vi[24]` writes in two
functions that are data misread as code (sub_00A11338, ov01 sub_00A46248
near 0xa46e34). Harmless unless executed.

## Next: VU1 speed

VU1 interpreter is ~110-160 ns per VU cycle (needs ~30x). It is a
cycle-accurate model: every instruction pair is copied out of the decode cache
by value, every result is queued into per-register pipelines and committed
later. Options, roughly in order of payoff:
1. Static recompilation of the VU1 microcode (it is embedded in the ELF:
   `_$NewVu1*MicroCodeStart` blocks) into C++ at build time.
2. A fast interpreter without pipeline modelling (only Q/P latency and the
   few documented hazards), falling back to the accurate one per program.
Also now that VU0 microprograms really run, check their cost in the
heartbeat: each call resets and re-seeds a VU1Interpreter instance.

## Older notes (before the fix)
- Sharing VU0 registers across threads changed the rows but didn't fix them.
- User saw a white screen first, then random shapes/colours.
- GS DISPFB1 once read 0x57615f5961575644 (ASCII junk), so something writes
  text into GS privileged registers. Still open.
- Matrix chain in nmlPacketAddParts: CurMatrixSet(s2+0x180 or camera+0x470),
  then CurMatrixMul(s2+0x180 / s2+0x80 / sp), CurMatrixGet(sp+0x150) -> VU1 962.
