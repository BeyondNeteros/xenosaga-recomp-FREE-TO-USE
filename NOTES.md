
## Garbage 3D textures: fixed (2026-11-01, runs/fix1)

Scenes rendered with noise on every 3D surface. Cause: PS2Recomp replaces
`sceGsSetDefLoadImage`/`sceGsSetDefStoreImage` (libgraph) with a hand-written
runtime stub that writes a different struct (x/y/w/h/address) instead of the
real 6-qword GIF packet (A+D giftag, BITBLTBUF, TRXPOS, TRXREG, TRXDIR, image
giftag) and returns 0 instead of 6. Xenosaga uploads every model's textures
each frame through `xglPacketTextureTrans` (XTX objects -> CT32 512x128 uploads
to 0x3800/0x3c00, read back as one T8 TBW16 atlas), so all of those uploads were
garbage and the 3D draws sampled whatever was left there (the 4-bit font).

Fix: recompile the game's own functions. The recompiler stubs known SDK names
by itself (`ps2_runtime_calls::isStubName`), so the patch adds a config option
`[general] recompile = ["name", ...]` that wins over that, and
`tools/setup_recomp.py` (UNSTUB) removes the two from `stubs`, adds the
function's jump table (0x4d50c0, read from the ELF) and writes `recompile`.
Other `sceGsSetDef*` functions (DispEnv, DrawEnv) are still stubbed and may
have the same problem; review them against the SDK code.

How it was found (tools worth reusing): `tools/run_game.sh` unattended runs;
`PS2X_GS_TRXLOG` (upload log), `PS2X_GS_TRXDUMP=<dbp,..>` (raw upload bytes +
GS memory right after), `PS2X_GS_TEXDUMP=<s>` (GS memory + TEX0/CLAMP at
textured draws), `PS2X_GS_SEQLOG=<s>` (ordered uploads/draws; the timer starts
at the first textured draw, so ~100 s reaches the tutorial scene),
`tools/gsvram.py` (decode GS memory), and a PCSX2 save state of the same scene
(runs/pcsx2: the .p2s is a zip with zstd entries; GS.bin holds VRAM at byte
offset 425; eeMemory.bin is main RAM) as ground truth.

Also changed on the way (kept, no visible effect on their own): the GIF arbiter
now runs packets in submission order (`PS2X_GIF_PRIORITY_SORT=1` restores the
old path-priority sort), and the texture page cache is invalidated by uploads.

## Squashed 3D view: fixed (2026-11-01, runs/fix2)

The camera frustum's X scale was 6.7x too big. xglCameraViewScreen derives the
pixel aspect from the game's dispEnv DISPLAY register (MAGH), which comes from
`sceGsSetDefDispEnv` - another stubbed libgraph builder, writing DX=2136,
DY=2047, MAGH=0 instead of the real DX=636, DY=50, MAGH=4, DW=2559 (checked in
the PCSX2 state). Now recompiled from the game (UNSTUB in setup_recomp.py).
It reads `sceGsGetGParam`, whose stub used byte fields; the SDK struct is four
shorts (interlace=1, omode=2, ffmode=0 on the real console), fixed in the
patch (Stubs/Helpers/Support.h). Display registers now match real hardware.

## In progress: collapsed legs / stray polygons (2026-11-01, runs/ram2)

Legs collapse upward to the hips and hair is flung out (see shots/). Findings:
- Object placement matrices (part +0x180/+0x80) match the PCSX2 state exactly,
  as do heap addresses, so RAM can be diffed directly (XS_RAM_DUMP=1 writes
  xs_ram.bin; compare with runs/pcsx2/eeMemory.bin).
- The VU1 skinning packets differ: per bone the packet has UNPACK V4-32 of 4
  qwords (quaternion-like values, some ~2x the real ones) then UNPACK of a 4x4
  matrix, which in the port is zeros/NaN/huge (e.g. 0xc00e30-0xc00f40 in
  xs_ram.bin vs eeMemory.bin). Source arrays near 0x112af20 differ too.
- Ruled out: VU0 inverse microprogram (tools/vu0_test.cpp runs it through the
  interpreter: correct), mirrored rotations, VOPMULA/VOPMSUB and broadcast
  multiply-add translations, VRSQRT (translator ignores the numerator, but this
  game always uses vf0w = 1).
- Next suspects: xglQuaternion2Matrix / xglQuaternionInterpolateLinear (pure
  VU0 macro code; decode with tools/cop2dis.py), the code that fills the
  per-bone quaternion block, and the remaining sceGsSetDefDrawEnv stub.



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
