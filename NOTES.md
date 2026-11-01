
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

## Collapsed legs / flung hair: fixed - SQRT.S operand (2026-11-01, runs/sqrt1)

Characters' skinning matrices (e.g. 0x1001170, built by JNT_setMDLMatrix =
xglMatrixMul(joint, inverse bind)) were NaN/1e38. Traced with the new write
watchpoint and NaN hunts:
- joint matrices come from JNT_constructMatrix and EXM_CalcExMotion (hair /
  skirt physics); the first joint to go bad is a type-3 EXM element:
  translate, then MATRIX_rotZ4s/rotY4s/rotX4s -> MATRIX_rotate4(axis, angle).
- MATRIX_rotate4 normalises the axis with `sqrt.s`. The EE encodes SQRT.S
  fd, ft (operand in ft, fs = 0; all 108 sqrt.s in the game are like that),
  but PS2Recomp's FPU translator read fs, i.e. sqrt(f0). For the Y and X axes
  that divided by 0 -> +-max -> NaN. Fixed in fpu_translator.cpp; every
  square root in the game was affected.
- Also fixed on the way (no visible effect alone): VU0 macro VDIV/VSQRT/VRSQRT
  now use the PS2 rules (x/0 = +-max, sqrt(|x|), fs/sqrt(|ft|)) via ps2_fdiv/
  ps2_fsqrt/ps2_frsqrt instead of returning 0.
- Checked and fine: CUR_MATRIX_Rzyx4s (self-check vs C++), VU0 sin/cos and
  matrix inverse microprograms (tools/vu0_test.cpp), VOPMULA/VOPMSUB, broadcast
  multiply-adds, VMULq, ldl/ldr/sdl/sdr, quaternion helpers, bind matrices.

Debugging infrastructure added for this (see CLAUDE.md): PS2X_WATCH write
watchpoint (runtime), XS_RAM_DUMP (EE RAM + scratchpad), XS_CUR_COUNT (CUR_MATRIX
call counts + NaN hunt), XS_EXM_HUNT, XS_ROT_CHECK, XS_MUL_WATCH, XS_JNT_DUMP,
tools/vu0_test (run VU0 microprograms offline), tools/cop2dis.py. Pass debug
switches with `run_game.sh ... --env NAME=VALUE`: variables set in front of the
command did not reliably reach the game. Note the skeleton object lives in the
scratchpad (0x70000000); hooks must read it via memory().getScratchpad().

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

## State after the SQRT fix (runs/sqrt1)

The "Virtual Tutorial" scene matches the PCSX2 save state: characters,
textures, camera and text. Remaining big item: speed (below). Things not yet
looked at: sound (skipped), FMV playback quality, later scenes/battles
(overlays ov10/ov11/ov12), widescreen.

## Parallel rasterizer (2026-11-01, runs/gspar1)

- GSCpuBackend queues primitives and draws them on `PS2X_GS_THREADS` threads
  (default hardware threads / 3, max 8; 1 = old immediate path). Each thread
  owns interleaved 8-row bands of every target and walks the queue in order,
  so every pixel sees the same operations as single-threaded.
- Drained before anything that reads/changes VRAM or transfer state (uploads,
  transfers, clears, readbacks, VRAM snapshot/present, FINISH) and when the
  GS worker's input queue runs dry. Queued primitives keep a CLUT snapshot,
  so CLUT loads don't drain (unless the CLUT source is being drawn into).
- Conflict checks use page ranges: frame/Z by the primitive's vertex rows,
  textures by their format's page geometry. A primitive that reads memory an
  earlier queued one writes (or writes what one reads, or aliases another
  target layout) drains first. A primitive that samples its own target or
  whose Z overlaps its frame is drawn ordered on one thread (~300 per frame in
  the tutorial: 2D effects around the characters that sample the framebuffer
  half a row below each pixel; a real row-order dependency).
- The texture page snapshot cache was removed (texels read straight from VRAM;
  same output, faster, and thread-safe).
- Drawing always uses round-to-nearest. `GS_REPLAY_ROUND=nearest` makes
  gs_replay do the same for every record: 1, 4 and 8 threads give identical
  VRAM; with 1 thread and recorded modes it still matches the game capture.
- gs_replay: 65.7 -> 46.8 ms per frame (8 threads); game 8.5 -> 9.6 fps, GS
  thread ~39% busy. VU1 (~70% of the main thread) is the limit again.

## VU1 static block scheduling (2026-11-01, runs/static1)

- Straight-line blocks (ending after a branch's delay slot, after the pair
  following an E-bit, at a D/T pair, or at 64 pairs) get a precomputed
  schedule: the issue cycle of each pair (same rules as
  calculatePairReadyCycle/markPairWrites, on offsets from block entry), the
  register lanes read before being written in the block ("live-ins") with the
  offset they are read at, first FDIV/EFU uses, and the final readiness of
  every register written. `staticEntryOk` checks live-ins, pending FDIV/EFU
  and that no XGKICK is in progress when the block kicks; otherwise that run
  of the block goes through the dynamic loop. Blocks with two XGKICKs or more
  than two EFU issues are never static. Per-pair execution and the
  pc/branch/E/D/T logic are shared (`execPairDirect`, `finishPair`); cycles are
  stepped one by one while a kick is active so its memory reads interleave
  with stores exactly as before. FDIV commits now order flag commits by the
  FDIV's own ready cycle (same result in the dynamic loop).
- `PS2X_VU1_STATIC=0` disables it. vu1_replay 22.9 -> 18.5 ns per VU cycle, 72/72
  captures identical to the frozen reference; game 7.1 -> 8.5 fps.
- Profile now: almost no scheduling cost left; FMAC fast path ~25%, block
  runner ~20%, execLower ~10%. GS thread is at ~60%, so it becomes the
  limit around 14 fps.

## VU1 vector fast path (2026-11-01, runs/simd1)

- `execUpperFmacFast` (ps2_vu1_upper.cpp): ADD/SUB/MUL/MADD/MSUB in bc/q/i/full
  and ACC forms with SSE, flags written straight into the flag ring; plus
  MAX/MINI, ITOF/FTOI, ABS and upper NOP without the generic operand setup.
  It only takes a lane it can prove exact: result safely normal, or an exact
  +-0 (ADD/SUB zero sums; MUL/MADD where an operand is 0). Otherwise it falls
  back before touching state. `PS2X_VU1_NO_FAST_FMAC=1` disables it.
- vu1_replay 25.7 -> 22.9 ns per VU cycle, 71/71 captures identical to the
  frozen reference; game 6.5 -> 7.1 fps.
- Tried and reverted: a "registers with pending writes" bitmask to skip the
  hazard check. No gain: the microcode is tightly scheduled, nearly every pair
  reads something written a cycle or two before.
- So the remaining cost is the cycle-accurate timing itself (hazard check,
  per-cycle bookkeeping ~50%). The next real step is static scheduling of
  straight-line blocks (stalls and flag/Q timing precomputed from block entry,
  with a cheap entry check that falls back to the dynamic path), or a full
  microcode recompiler.

## Graphics (GS) speed (2026-11-01, runs/async1)

- In-game profile (`XS_PROFILE=180,45`): one thread did all the work, ~65% VU1
  and ~30% software rasterizer (SampleTexture, WritePixel, DrawTriangle,
  texture/Z reads); the EE game code itself is small.
- `PS2X_GS_CAPTURE=185,3` + `tools/gs_replay` (target gs_replay): exact offline
  replay of 3 tutorial frames, final VRAM byte-identical to the game's (the
  capture records whether each call ran with VU1's round-toward-zero mode, which
  changes the rasterizer's float maths). ~70 ms of GS work per frame.
- The GS now runs on its own worker thread (`PS2X_GS_ASYNC=0` to disable): game
  5.2 -> 6.5 fps, GS thread ~46% busy, main thread now ~75% VU1. Readbacks
  sync with the queue (CSR/SIGLBLID reads, CSR writes, local->host, VRAM reads,
  presentation latch).
- Tried and dropped: per-row span limiting in DrawTriangle (no gain, triangles
  are tiny; per-pixel work dominates).
- Next for GS when it becomes the limit again (~14 fps): hoist per-primitive
  setup out of SampleTexture/WritePixel, then split rasterization over several
  threads by screen bands (same per-pixel order, so output stays exact; flush
  on uploads and when a draw samples a buffer drawn earlier in the batch).

## Speed status (2026-11-01, runs/fast3)

The heartbeat prints `fps: game X, vsync Y` (game = DISPFB1 flips per
second, sampled every 1 ms). Tutorial scene: **game 5.2 fps** (was 2.6; the
game targets 30, i.e. 2 vsyncs per frame). Host time: VU1 ~72%, GS ~37%.

VU1 interpreter, vu1_replay on runs/sqrt1: 112 -> 25.7 ns per VU cycle, results
identical to the original interpreter on all 66 captures (`tools/vu1_check.sh`
against the frozen reference build in `PS2Recomp-ref/`, checksum now includes
MAC/status/clip). What changed:
- results go straight into registers (readers stall until the value would
  have landed anyway); `PS2X_VU1_FAST=0` restores the delayed-write queues
- FMAC flags skip the long-double exact result when the float result is safely
  normal (`PS2X_VU1_EXACT_FLAGS=1` forces it)
- flag pipeline is an issue-order ring committed lazily (flag reads, full ring,
  FDIV status update, program end)
- decode cache validated once per call; per-instance decode epoch and commit
  deadline (the old global 4-slot tables could be shared by VU0 instances)
- cheaper hazard check / write bookkeeping, inline broadcast

Workload: no duplicate drawing (`PS2X_GS_DUPCHECK=1`): ~13,400 GIF packets /
6 MB and ~4.2M VU1 cycles per frame. 30 fps therefore needs VU1 at ~5 ns per
cycle (or VU1 on its own thread plus ~10 ns) AND a much faster GS (37% at 5 fps
would be >200% at 30): a GPU renderer or a threaded/optimised software one.

Next for VU1: a translator that removes decode/dispatch and computes stalls
statically inside straight-line blocks (only reads of registers written before
the block need a dynamic check; flags/Q/P are already lazy), validated with
vu1_check.sh. Profile now: execUpper 20%, hazard check 15%, run loop 13%,
commit 12%, FMAC result/flags ~22%.

## Next: VU1 speed (older notes)

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
