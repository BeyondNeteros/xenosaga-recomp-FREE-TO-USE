
## Known broken, parked until 30 fps (reported 2026-11-01)

- Battles: the game skips them entirely (not investigated yet).
- Music: parts of songs missing (see "Sound playing" for what the SPU2 lacks).

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

## Battles: first look (2026-11-02, runs/rec1, battle-fast, battle-safe2)

- Beyond recorded a new-game run into the first battle after the tutorial
  (runs/rec1-inputs.txt; enemy movement differs per replay, so the battle
  does not always trigger). XS_FRAME_KEEP=1 keeps a frame every heartbeat
  (run_game.sh moves them to runs/<name>/frames).
- Second area (field) draws fully and runs at 30 fps in the fast build.
- Fast build (Beyond's run): the battle starts, its UI shows faintly over
  black, and the field stays black after it except characters/HUD.
- All speed-ups off (PS2X_VU1_NATIVE/STATIC/FAST=0, PS2X_VIF1_ASYNC=0): the
  battle ends at once (results "Clear Time 00:00:04"), field fine after.
- In every run only overlay 1 is ever activated (xglCdLoadOverlay is never
  called again), so battle code presumably never gets its overlay (OV10/11/
  12?) - next thing to find: how/where the game loads battle code.
- "disc file not found: \DATA\..." lines are loose-file probes; the disc
  has no DATA folder (everything is in XENOSAGA.xx archives).

## 30 fps reached in the tutorial (2026-11-02, runs/prio-on, prio-off)

- The PC is an i7-13700KF (8 P-cores + 8 E-cores, 24 threads). The VIF1/VU1
  worker now runs at THREAD_PRIORITY_HIGHEST and the GS band helpers at
  BELOW_NORMAL, so Windows keeps the worker on a P-core
  (`PS2X_THREAD_PRIORITIES=0` turns this off).
- Remaining interpreter fallbacks per capture pass are only a few thousand
  (CLIP, DIV, FCAND, XGKICK...), not worth inlining for speed.
- Game (PGO retrained): priorities on = locked 30.0 fps, VU1 worker 81% busy,
  EE idle 64% (waiting for vsync); priorities off = ~30.0 but at the edge
  (worker 91%, no idle). Memory flat ~260 MB, picture correct.
- Next (Beyond's order): modernize (widescreen, 60/120 fps), then the parked
  items: battles are skipped; recheck music in other scenes.

## Bigger GS batches, native MTIR/MFIR (2026-11-02, runs/opt30a)

- GS: a batch drains after 48 distinct write ranges / 96 read ranges (was
  16/32; most drains were this limit). gs_replay 8 threads 25.0 -> 22.1 ms
  per frame, VRAM identical.
- VU1: MTIR and MFIR inlined in lowerC. 118/118 captures identical; ~5%
  faster (gsopt2 capture 6.2 -> 5.8 ns per VU cycle, no PGO).
- Game (PGO retrained): 27.4-29.8 fps, ~29.0 average and steadier; GS 69%,
  vif1 ~91% (the VU1 worker is the only limit). Memory flat ~261 MB.

## Memory runaway fixed; 28-30 fps (2026-11-02, runs/vu1opt3)

- Beyond saw freezes/crashes during a PGO training run: the process was at
  ~15 GB. The GS async command queue had no limit; in the slow instrumented
  build the GS thread fell behind the VIF1/VU1 worker and the queue grew
  without bound. GS::AsyncQueue now blocks producers above 64 MB of queued
  commands. The heartbeat fps line also prints process memory (normal
  build: flat ~258 MB). Earlier "freeze and crash" reports were likely the
  same thing plus run_game.sh force-closing the game.
- VU1: SQ/SQI write VU memory directly when no XGKICK is active (a queued
  store is visible before the next pair anyway); pairs needing the branch-VI
  backup use `pairNB` when the next pair is not a branch (306 pairs).
  116/116 captures identical.
- `PS2X_WINDOW_MONITOR=n` opens the window on monitor n (run_game.sh passes
  it through with --env).
- Game (PGO retrained on the game): 27.2-30.2 fps, ~28.6 average; vif1
  ~91%, GS ~81%: both limit now.

## GS per-texel work (2026-11-02, runs/gsopt2)

- SampleTexture picks the texel decoder (CT32/CT16/CLUT) once per lookup
  instead of per texel; bilinear blends all four channels with SSE using the
  same float operations per lane (`bilinear4`; scalar fallback for |x| >=
  2^23 / NaN). Forcing it inline was slower (code size).
- ownsRow caches the last row's answer (it divided per pixel).
- gs_replay single thread 66.3 -> 56 ms per frame; VRAM identical (nearest
  8 threads 9bd6e6333fd57e89; 1 thread recorded rounding = game's end VRAM).
- Tried and dropped: spinning pool threads before sleeping. No gain in
  gs_replay, and in game the spinning threads took CPU from the VU1 worker
  (26.8 -> 25.2 fps).
- Game (PGO retrained on the game): ~27.0 fps, GS thread 79% -> 73%. The
  VU1 worker (vif1 ~91%) is the limit again.

## PGO trained on the game (2026-11-01, runs/pgogame1)

- In-game profile (runs/prof2, XS_PROFILE) showed the hottest VU1 blocks in
  the game are not the ones in the short captures, so the capture-trained
  profile missed them. `bash port/tools/pgo.sh game` builds the PGO sources
  instrumented in build-runtime (non-atomic counters), plays the tutorial
  script, the heartbeat calls __gcov_dump() at XS_PGO_DUMP=160 s (heartbeat
  time starts after boot), then rebuilds with PS2X_PGO_USE and installs.
- 113/113 captures identical; game 23.6 -> 26.8 fps, picture correct. Now
  the GS thread is nearly as busy as the VU1 worker (gs 79%, vif1 92%).
- Note: run_game.sh force-closes any running XenosagaRecomp.exe at start
  and end; tell Beyond before unattended runs.

## Native branches, wider dead flags, bigger flag ring (2026-11-01, runs/br1)

- lowerC inlines B/BAL/JR/JALR/IBEQ/IBNE/IBLTZ/IBGTZ/IBLEZ/IBGEZ (VI read
  through the branch backup, as readBranchVi); block-end pairs use
  `finishPairC<U, L>` (E/D/T bits and "lower is a branch" are constants).
- Dead flag rule widened: op i is dead if the next MAC/status writer j is
  >= 4 pairs before the block end and no flag read sits in pairs i+1..j+3
  (472/741 ops now, was 155). Flag ring 8 -> 32 entries.
- PGO profiles go stale when code changes and can then fail the build
  ("profile data is not flow-consistent"): build-runtime now also passes
  -fprofile-correction; for iterating, configure with -DPS2X_PGO_USE=OFF and
  run pgo.sh at the end.
- 111/111 captures identical; 8.2 -> 7.0 ns per VU cycle (PGO on both);
  game 22.2 -> 23.6 fps. GS thread now ~73% busy, gssync ~10%.

## Profile-guided optimisation (2026-11-01, runs/pgo1)

- `bash port/tools/pgo.sh`: instrumented vu1_replay in PS2Recomp/build-pgo
  (CMake `PS2X_PGO_GENERATE`), trained on every runs/*/vu1_capture.bin, the
  .gcda files copied next to the objects in build-runtime, rebuilt with
  `PS2X_PGO_USE=ON` (VU1 core/upper/lower, VIF1, memory, GS files and the
  native blocks only). ~2.5 min.
- 110/110 captures identical; 9.15 -> 8.2 ns per VU cycle; game ~20.7 ->
  22.2 fps, picture correct.
- Rerun pgo.sh after changing VU1/GS code or regenerating native blocks
  (stale profiles are ignored per function, so a forgotten rerun only loses
  speed). -fprofile-dir cannot be used: the project path has spaces.

## VU1 bookkeeping trimmed (2026-11-01, runs/vu1opt1)

- Profile (vu1_replay, runs/fbsnap1 capture): native block bodies ~48%, the
  rest per-block/per-call bookkeeping: staticEntryOk 9%, finishStaticBlock
  8%, commitFlagsUpTo 7%, run 4%, std::fesetround 2.4%.
- `run()` sets MXCSR.RC and the x87 control word directly (setting only
  MXCSR changed 43 captures: some library maths still uses x87). The GS
  backend and the GS capture use MXCSR too.
- `m_readyMax` bounds every VF/VI/ACC readiness value; live-ins are sorted by
  offset and the entry check stops at the first one past the bound.
- Final VF readiness is stored per register with one AVX2 masked store.
- Dead flag entries: vu1gen marks FMAC ops whose MAC/status entry is
  replaced (a later writer >= 4 pairs before the block end, no flag reads in
  the block); they skip the ring and fold their sticky Z/S bits into the next
  entry (`m_deadSticky`). Only 155/741 ops qualify (many blocks read flags).
- 108/108 captures identical; 11.9 -> 9.15 ns per VU cycle; game (without the
  dead-flag part yet) 18.7 -> 21.2 fps; with it (runs/vu1opt2) ~20.5-21 fps,
  same within run-to-run noise.
- Path stats (one pass over the fbsnap1 capture): 99% of pairs run in
  native blocks, ~10 pairs per block run; ~19k interpreter execLower calls
  per 117k pairs, mostly block-end branches (IBEQ/IBNE/JR/B) and MTIR/MFIR/
  DIV. Next: native branch pairs + chaining straight into the next block,
  then cheaper per-pair cycle bookkeeping inside blocks.

## VU1 on its own thread (2026-11-01, runs/async1v)

- Measured first (runs/gap1, gap2): the game kicks one VIF1 chain per frame
  (~51 ms of VIF1/VU1 work), then runs ~11 ms of EE code before it next
  touches DMA/GIF/GS/VU1 state (a GIF_STAT read; VU0 memory accesses in
  between don't count).
- PS2Memory now hands VIF1 chains >= 16 KB (chain data is already copied at
  kick time) to a worker thread, which also drains the GIF arbiter. The EE
  syncs (`syncVif1`) before any non-RAM access except timers, VU0 memory and
  SIF registers, before GS priv register access, at the start of
  processPendingTransfers and in syncCoreSubsystems (GS library stubs). The
  channel completes (CHCR, QWC, D_STAT, DMAC cause) on the EE at that sync or
  at the next vblank poll (`pollAsyncVif1`), where DMAC handlers run. VU1
  D/T stop bits from the worker are applied to the EE context at completion.
  `PS2X_VIF1_ASYNC=0` turns it off. Heartbeat `vifwait` = EE waiting for it.
- Game 16.6 -> 18.7 fps, steady (the late 9-13 fps dips are gone), picture
  identical. Worker ~94% busy, EE waits ~74%: VIF1/VU1 alone sets the frame
  rate now; 30 fps needs it ~1.5x faster (~50 -> ~33 ms per frame).

## Feedback sprites drawn in parallel (2026-11-01, runs/fbsnap1)

- gs_replay stats showed the ordered (single-thread) path was more than half
  of the GS time: ~300 feedback primitives per frame, but ~92% of their pixels
  are 2 full-screen sprites per frame that sample their own frame buffer one
  row below / one pixel to the right (shift/blur passes).
- `isForwardFeedbackSprite`: a sprite on the same address/width/format whose
  every nonzero-weight texel (same maths as DrawSprite/SampleTexture) is at or
  after the pixel in raster order only ever sees pre-sprite values. Those are
  drawn alone on all threads from a VRAM copy of the texture pages.
- gs_replay (8 threads): 45 -> 34 ms per frame, VRAM hash unchanged at 1/4/8
  threads. In game: GS thread 76% -> 42% busy, gssync 39% -> 1%, fps
  unchanged (~16.5): the main thread (VIF1/VU1 ~78%) is now the only limit.
- Left ordered: the ~300 small heat-haze triangles per frame (cheap).
- The fps dips to 9-13 once the "Virtual Tutorial" text box is up (late in
  the 260 s run; not compared with older runs yet).

## XGKICK batching (2026-11-01, runs/kick1)

`advanceTo` steps the first cycle normally (the last pair's stores land),
then applies the remaining commits and PATH1 progress in one step
(`progressXgkickCycles`); the qword copy uses memcpy unless it wraps. Native
blocks regenerated with the game's own block log (139 blocks). 103/103
captures identical; game 15.4 -> 16.8 fps. Main thread ~80% VIF1/VU1, GS
thread ~76%: both now limit.

## GS sync narrowed (2026-11-01, runs/sync1)

The main thread spent 33% waiting for the async GS: every CSR write synced
the GS queue, and xglRenderDispEnvMove writes CSR = 0x10 (EDWINT ack) every
frame. Now CSR writes only sync when they clear SIGNAL/FINISH (bits 0-1), and
only CSR/SIGLBLID reads sync. Game 12 -> 15.4 fps, picture correct.
New diagnostics: heartbeat `gssync` (time waiting for the GS, any thread incl.
the presentation latch) and `eeidle` (scheduler waiting on host time);
`PS2X_GS_SYNC_LOG=<n>` prints the EE pc/ra of each sync; XS_PROFILE now also
writes per-thread samples ("texe" lines).

## Sound playing (2026-11-01, runs/spu8)

ps2xIOP now has an SPU2 (`src/emulator/core/iop_spu2.*`): register file for
both cores, 2 MB sound RAM filled by IOP DMA ch4/ch7 and the data port,
PS-ADPCM voices with loop flags, ADSR, pitch (linear interpolation), fixed
volumes, dry + effect mixing, master/effect volumes, IRQ address -> IOP IRQ 9.
The runtime plays it through a raylib audio stream (48 kHz, pulled on the
audio thread, so pitch is right even below full speed; music tempo follows
the game's speed). The launcher and run_game.sh now run with sound
(XS_NO_SOUND=0).
- Bugs found on the way: STATX must read 0 after reset (libsd init waits for
  it) and 0x80 after a DMA; voice address high/low pairing.
- The effect bus goes through a simple stand-in reverb (two feedback delays);
  voices the game sends only to reverb were silent before.
- Not yet: the SPU2's real reverb program, volume sweeps (treated as full),
  noise, pitch modulation, AutoDMA (streamed audio input, likely needed for
  voices/FMV), gaussian interpolation.
- Diagnostics: `PS2X_SPU2_OUTPUT=0` mute, `PS2X_SPU2_WAV=<file>` record raw
  output, `PS2X_SPU2_LOG=<n>` register accesses, `PS2X_SPU2_EVENTS=<n>` key-ons
  and DMA transfers.

## Sound driver running (2026-11-01, runs/snd12)

With `XS_NO_SOUND=0` the game's own IOP modules (LIBSD, SSD = Monolith's
sequencer, RSSD = its RPC server) run on ps2xIOP and the game reaches the
tutorial at the same speed, sending thousands of sound commands. Still silent:
there is no SPU2 emulation yet (next step). Fixes on the way:
- ps2xIOP: after a module starts, its new threads run until they block
  (`settleReadyThreads`), so RSSD's RPC server is registered before the game's
  one-shot bind (it gives up otherwise).
- IOP -> EE RPC: sifcmd sceSifBindRpc/sceSifCallRpc/sceSifCheckStatRpc on the
  IOP now reach servers the game registered (`IopHost::hasEeRpcServer` /
  `callEeRpc`, `IopSubsystem::completeEeRpc`); blocking callers sleep until
  the reply, end functions run on completion (as the SDK, in both modes).
- EE side: real `sceSifRpcLoop` (sleeps, runs the server, replies, resumes at
  its own entry decoded from the caller's j/jal at ctx->branch_pc, since the
  stub wrapper sets pc = ra) and `sceSifGetNextRequest`/`sceSifExecRequest`
  (SsdSpuDmaCompleted polls with these).
- `sceSifRegisterRpc` read cfunc/cbuf/qd from the stack; the EE ABI passes
  them in t0-t2.
- Diagnostics: `PS2X_RPC_LOG=1` (every SIF RPC event, both directions),
  `PS2X_IOP_HWLOG=1` (IOP SPU2 DMA registers and interrupts), IOP thread
  states in the heartbeat, `tools/irxdis.py` (disassemble a loaded IRX).
- Launcher still sets XS_NO_SOUND=1 until audio actually plays.

## VU1 native blocks (2026-11-01, runs/native1)

- Static blocks have a key (hash of start pair, code words and pair classes).
  Generated C++ for known keys is registered at startup
  (`VU1Interpreter::registerCompiledBlocks`) and replaces the per-pair loop
  for that block; everything else still runs in the interpreter
  (`PS2X_VU1_NATIVE=0` turns native blocks off).
- Generated code calls `include/runtime/ps2_vu1_fastops.h` with constant
  instruction words: the upper fast paths (FMAC, MAX/MINI, ITOF/FTOI, ABS),
  the full pair logic (I-bit, shadowing, branch-VI backup) and the common
  lower ops (LQ/SQ/ILW/ISW/IADDIU/ISUBIU/IADD/ISUB/IADDI/IAND/IOR/MOVE/MR32/
  LQI/SQI) fold down to straight-line code; other lower ops call execLower.
- Workflow: `PS2X_VU1_BLOCKLOG=<file>` logs blocks without native code (the
  launcher and run_game.sh write `vu1_blocks.vbl` in the game folder; for
  captures: run vu1_replay over runs/*/vu1_capture.bin with it set). Then
  `python port/tools/vu1gen.py <logs...>` writes recomp/xenosaga_vu1_blocks.cpp
  and copies it into the runner (assemble.py copies it too); reconfigure
  cmake and rebuild. vu1_replay compiles it too, so vu1_check.sh covers it.
  The log and generated file are game-derived: never commit them.
- Current set: 57 blocks / 947 pairs from the 75 captures. vu1_replay 17.3 ->
  13.1 ns per VU cycle (75/75 identical); game ~9.9 -> 12.0 fps. GS thread
  now ~50% busy.

## VU1 next steps (2026-11-01, runs/plain1)

- Static blocks now classify each pair once: "plain" pairs (no branch, E/D/T,
  I-bit, upper/lower shadowing or branch-VI backup, not at the end of
  microcode memory) skip NOP halves and finishPair. Live-in checks are merged
  per register. 18.5 -> 17.3 ns per VU cycle, 74/74 captures identical; game
  ~9.9 fps, picture correct.
- Profile: execUpperFmacFast ~45% (per-instruction decode/branching), block
  runner ~12%, execLower ~9%, entry checks ~7%.
- Budget for 30 fps: ~4.2M VU1 cycles per frame -> <= ~8 ns per VU cycle with
  VU1 overlapping everything else. Two pieces left:
  1. Native code for VU1 blocks: the runtime logs blocks it runs; a generator
     emits C++ with each pair's instruction words as template constants (decode
     and branches fold away), compiled into the runner and looked up by block
     hash at run time, falling back to the interpreter. Verifiable with
     vu1_check.sh. Expected ~1.5-2x on VU1.
  2. VU1 (VIF1 processing + VU1 + XGKICK) on its own thread, with the EE only
     queuing copied DMA data and syncing when it reads VIF1/DMA/VU1 state:
     frame time becomes max(EE, VU1) instead of the sum.

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
