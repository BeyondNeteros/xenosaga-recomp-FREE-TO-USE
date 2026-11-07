
## Internal resolution upscaling (2026-11-07, runs/up3, up4)

`settings.ini` `[display] upscale = 1-4` (default 3; env `PS2X_UPSCALE` overrides
for test runs). GPU renderer only (`gs_gl_backend.cpp`):

- Render targets are drawn at `kScale` x native (viewport `2048*S`, scissor x S;
  the vertex shader is unchanged). Targets are allocated `min(fbw*64,2048) x
  rows` (start 512 rows, grow in 256-row steps) instead of 2048x2048, so 4x
  stays small. Lines become one-PS2-pixel-thick quads above 1x (core profile
  has no wide lines).
- Local memory keeps native pixels: `loadFromMemory` uploads to a native
  staging texture and blits it up (NEAREST); `syncToMemory` blits down into
  staging and reads that back. So everything the game reads stays exact.
- A target used as a texture (CT32, same layout) is sampled at full
  resolution: state `s10.x` = scale; `texelScaled` wraps on the native texel
  and keeps the sub-pixel. Feedback snapshot texture is resized on demand.
- Presentation: `Present` still builds the native frame, then
  `readHiresFrame` reads the displayed rectangle (DISPFB fbp/fbw/DBX/DBY,
  native display size x S) straight from the GPU target into
  `PresentationFrame::hiresPixels`. The frontend keeps it
  (`GS::copyLatchedHiresFrame`) and the window draws it instead of the native
  texture (shape still from the native size). Skips (logged once as
  `[gs-gl] native frame shown: <why>`): preferred source, circuits showing
  different buffers, non-CT32/24, no GPU target (early boot). Field mode is
  allowed: the game draws whole 448-line frames, so the hi-res path shows all
  lines, where the native path line-doubles one field.
- `XS_FRAME_KEEP=1` also saves `frame_NNNN_hi.bmp` per heartbeat.

Results (RTX 5090): 3x 1536x1344 holds 30.1 fps through the tutorial run,
4x 2048x1792 29.3-29.9 (vsync dips to ~58 from the 14 MB/frame readback and
upload). The bottom line of the picture has garbage at every scale; it is in
the game's buffer (overscan on a TV). Not yet checked: battles, effects that
read the frame through local memory (they come back at native resolution).
Possible speed-up: share the texture with the window's GL context instead of
reading back.

## Menu background black: VRNEXT was never written (2026-11-07, runs/ui-menu-*, ui-rtps)

Menus (save, shop, results) draw a 3D backdrop (tya_menu_bg_entry.c: 16 strip
tasks with random offsets from xglFRand). The strips reached the GS but all
corners fell on two points, Z 0 (ZTST greater: nothing drawn). XS_RTPS_LOG=<n>
(xglRotTransPersN hook, reads the scratchpad) showed inputs of 3e36: xglFRand is
`vrnext.x vf1, R; qmfc2`, and the recompiler's VRNEXT/VRGET/VRINIT/VRXOR were
invented ("LFSR-like"); VRNEXT never wrote ft. Now as PCSX2: R = 23 random bits
| 0x3F800000, VRNEXT steps bit0 ^= bit4 ^ bit22 and writes ft's dest fields,
RINIT/RXOR take fs.fsf (vu_translation_helpers.cpp; regenerate). Every
xglFRand user (effects, particles) got garbage before.
Tools: tools/gscap_draws.py lists what a GS capture (PS2X_GS_CAPTURE) draws;
tools/xsarc.py lists/extracts files in the XENOSAGA.xx archives (offsets count
from the start of XENOSAGA.00 / .10).

## Parallel VU1: built, correct, not faster yet - shelved (2026-11-07, runs/par-verify1-3, par-perf1-2)

`patches/ps2recomp-runtime-vu1-parallel-wip.patch` is the full runtime patch WITH
the parallel VU1 work (apply it instead of ps2recomp-runtime.patch to resume).
Design as in the section below: Vu1Parallel (src/lib/vu/ps2_vu1_parallel.cpp)
queues MSCAL/MSCNT from the VIF1 worker to N threads, each job on a private VU
memory snapshot with VU1Interpreter::Tracking (lane-level memory reads/writes,
register live/written masks incl. Q/P landing times, CLIP shift counts, status
sticky as read-modify-write), commits in order, reruns on conflicts, barriers
on DIRECT/MPG/MSKPATH3/chain end. PS2X_VU1_PARALLEL=1 enables,
PS2X_VU1_PAR_VERIFY=1 reruns everything and compares.

Results: verify mode 0 mismatches over millions of calls; 90-93% of calls
valid in parallel (remaining: vf25.x/vf3.x/vi15 set by matrix-setup calls,
then memory). But the master (VIF1 thread) cost per call (~3.6 us: snapshot
copy, checks, merge, reruns) exceeds a call's own cost (~1.5-3 us), so the
game ran 17-20 fps instead of 28. Incremental snapshots (base job + delta,
built by workers) and SIMD checks were added but not measured before
shelving. The tracking hooks alone (one branch per load/store/block) cost
~1-2 fps with the feature off, so the runtime was restored to the committed
state. To make it pay: master work must be well under 1 us per call - e.g.
batch many calls per job (a job = a run of calls between setup calls), keep
write lists sparse, avoid re-validating every call; and learn to wait after
matrix-setup calls instead of rerunning the queued calls behind them.

## Frame-rate dips: what limits them (2026-11-07, runs/vifwait1, peek1-5, cap1)

The dips left in gameplay (battles 21-24 fps, story cutscenes) are VU1-bound:
the VIF1/VU1 worker is 86-92% busy, the EE waits on it (vifwait ~70%),
eeidle 0. In the battle dip (cap1, PS2X_VU_CAPTURE_SKIP=25000000 on
inputs-prev) every call is the same small effect program (entry 0x170: setup
or a batch of tiny loops, 284 VU cycles, one 208-byte packet), ~125k calls/s.
Native blocks cover everything (no vu1_blocks.vbl written); time is ~55% in
two native blocks, ~35% per-block bookkeeping (run loop, finishStaticBlock,
lazy flag commits): the loops are 2-4 pairs each, so every iteration is a block.

Checked and not the cause:
- Waits: PS2X_VIF1_SYNCLOG=1 logs who makes the EE wait (`[vif1-wait]`
  return address ^ register address<<32). The big one is sceGsSyncPath at the
  end of the frame (processPendingTransfers), i.e. the frame really waits for
  VU1. VIF1_STAT/GIF_STAT reads now answer "busy" while the worker runs (up to
  two reads per chain, then wait; PS2X_VIF1_PEEK=0 = old), as on hardware, but
  that made no measurable difference. mapVuMemory no longer syncs for those two.
- XGKICK drain at program end went cycle by cycle with full pipeline scans;
  now bulk when only the kick is left (PS2X_VU1_BULKKICK=0 = old). Same
  checksums/cycles, ~2% faster.
- PS2X_VU1_STATS=1 (vu1_replay or game, printed at exit): static blocks vs
  dynamic pairs and why. Battle capture: all static.

The big lever (not done): run VU1 draw calls in parallel. VU_REPLAY_STALE=1
(vu1_replay) reruns each call from the previous call's start registers: 299/299
identical in cap1, audio2, soak1; 294/299 in field captures (the misses follow
matrix-setup calls). So calls almost never depend on the previous call's
register results. A safe design: one job per MSCAL/MSCNT with a private copy of
VU data memory, per-job read/write qword bitmaps and register read/write masks
(VF/VI/ACC from InstructionUsage; MAC/status/clip/Q/P/I/R need a decoder:
flag reads FMAND/FSAND/FCAND..., CLIP and status sticky bits are
read-modify-write), commit in order (GIF packets buffered per job), rerun
sequentially on any overlap with earlier in-flight jobs' writes, barrier on
MPG/DIRECT/end of chain, plus a verify mode that also runs every call
sequentially and compares. Needs the battle dips ~1.35x to reach 30.

## Audio skipping fixed: SPU2 runs on IOP time (2026-11-07, runs/audio-old, audio2)

Cause: the SPU2 was rendered on the host audio thread in real time, while the
IOP (sound driver) only runs as EE cycles pass on the game thread. Streams
(decomp audio/ssd/ssd_vag.c: per voice two 1 KB chunks at SPU 0x6000, IRQ on
the chunk being played, ~37 ms to refill the other) were missed whenever the EE
stalled or ran slow, so old chunks played again. New counter: a stream chunk
played again without a DMA refill = "stale". Replay of inputs-prev, 5 min:
old 300 stale of 7526; new 0 stale, 0 underruns.

Now (default; `PS2X_SPU2_SYNC=0` = old behaviour):
- the IOP loop renders 16 samples per 768x16 IOP cycles into a ring
  (`Spu2::produce`); idle jumps are capped to that step so IRQs land on time;
- the audio callback plays the ring with a ~64 ms cushion and a +-1.5% speed
  trim; empty ring = fade out and wait to refill;
- the IOP is kept within -5/+20 ms of the host clock (`realtimeCatchUp`:
  extra cycles when behind, at most 100 ms per catch-up; held when ahead),
  as a real IOP keeps time while the EE is overloaded.
- `[spu2] last 5 s:` log line: stream chunks, stale, underruns, dropped,
  buffered frames, catch-up and held ms.
Side effect to watch: in slow cutscenes voices now run at real speed and may
lead the animation (before, they skipped instead).

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

## Saving and loading work (2026-11-07, runs/savecrash2, savetest1-7, loadtest2, soak1)

The save point still crashed after the first fix below. Log: "Game thread
returned. PC=0x289dbc": the game called exit(1) because the thumbnail JPEG was
over 4 KB (file.c MenuFileMain). Three bugs:

- **`cvt.w.s` rounded to nearest** (`FPU_CVT_W_S` used `nearbyintf`). The EE
  always truncates. Every `F2I` in the game was off; in the JPEG quantizer
  (`xgl_jpeg.c` Quantize adds +-0.5 then F2I) every coefficient became +-1:
  speckled ~17 KB thumbnails whatever the quality. Now `ps2_fpu_to_word`
  (truncate, clamp on overflow) in ps2_runtime_macros.h. VU FTOI already
  truncated. Side effect seen: the "Ssd function error -19" (sound effect
  index out of range) lines are gone from a 10-minute replay (soak1).
- `sceGsExecLoadImage/StoreImage` (stubs) read the image struct as the stub's
  own `GsImageMem`, but `sceGsSetDefLoad/StoreImage` run as the game's code and
  fill the real libgraph packet (+0x20 BITBLTBUF, +0x30 TRXPOS, +0x40 TRXREG).
  Both now detect that packet (`readGsDefImagePacket`, GS.cpp).
- The thumbnail guard sat around GameSnapShotSaveThumbnail, but the encoder can
  pause at a checkpoint and resume straight into MenuFileMain past the wrapper.
  The retry now wraps `exit` (0x32E168) at ra 0x289DD0: lower quality, pc back
  to the encoder; other exit() calls are logged as `[exit]`.

Verified by replay: save writes mc0/BASLUS-20469Xeno00 (91 KB data, icon.sys,
xenosaga.ico); the slot list shows the thumbnail; title Continue
(inputs/title-continue.txt) loads it to the field; overwriting 30 times in a
row works; backing out of the menu returns to the field. Slots past ~No. 50 show
"NOT USABLE": 8 MB card / ~160 KB per save, as on hardware.

Saves are now backed up before being replaced: `save-backups/mc0/<dir>/<file>.bak1..3`
beside mc0 (MemoryCard.cpp `backupMcSave`), and a format renames the card
folder (`mc0-before-format-N`) instead of `remove_all`.

Test aid: `XS_TEST_SAVE=<field frames>` opens the save screen that many field
frames in, presses Circle (PadData +0x2A and the list's +0x30), answers Yes
(MenuSelectMove with 2 rows returns 0), logs menu states (`FileWork` +3), and
backs out with Cross after one save. Combined with title-continue.txt it tests
load + save + return. Not done: replays that follow a recording only in the
field (skip cutscenes, auto-battle) - recordings carry no scene markers yet.

## Save point crash; vsync catch-up limited (2026-11-07, runs/field-freeze/session3.log, save1, pace1)

Beyond's first long live session: about 10 min, battles fine, no freeze. Confirm at a
save point ended the game: `Game thread returned. PC=0x289dbc` is the game's own
`if (GameSnapShotSaveThumbnail(30, buf) > 0x1000) exit(1)` (main/file.c). The
thumbnail is a sprite at Z 0xFFFFFFFF with ZTST greater into page 0x1C0
(game_snap_shot.c). The GPU shader's float z overflowed uint and failed the test, so
the JPEG encoded whatever texture data sat there. The test now uses the full value
and masks only the store. saveThumbnail_guard also re-encodes at lower quality if it
still doesn't fit (skipped if the call yields). NOT yet confirmed: replays drift and
didn't reach the save.

Cutscene swings (20-40 fps): VBlank fires when both cycle and host deadlines pass,
and host deadlines were absolute, so after a slow stretch vsync ran up to 115 Hz.
No catch-up at all halves slow scenes (save1: 25 -> 15 fps, 3 min at 14-17), so the
host deadline may lag by at most two periods. pace1: max 34.6 fps, mostly 30.
save1's story cutscenes were VU1/VIF-heavy (~38 ms/frame); pace1's replay drifted
and may not have hit them. Audio glitches not measured.
VU1 blocks now 440 (runs/vu1blocks/game-1107.vbl), PGO retrained, vu1_check clean.
Also: use the normal git for pushing; the MSYS2 one on PATH hangs without credentials.

## GPU renderer; battle freeze root cause fixed (2026-11-07, runs/gpu1-5, tr1, heap1)

**Battle freeze, root cause:** `IopMemory::allocate` only moved m_heapCursor
forward, so freed blocks were never reused and the 832 KB IOP heap "ran out"
after a few minutes of RSSD messages. iRssdCallFunc's AllocSysMemory then returned
0, the "wave transfer done" (fn 37) node was lost, and the EE spun in
SsdSpuDmaCompleted forever. Found with iopTrace, a silent ring buffer printed by
PS2X_IOP_WATCH (printing live hid the bug): `irq 36 -> wakeup -> sleep`, no
`call-ee 37`. Now next-fit with wrap-around. heap1: 6 min with battles, no freeze.
The earlier "race" reading was wrong; it depended on total sound traffic, which
is why drifting replays missed it. The WaitRpc and interrupt-gating fixes were
still real bugs, but neither was this one.

**GPU renderer:** `src/lib/gs/gs_gl_backend.cpp` (GL 4.5 +
ARB_fragment_shader_interlock, own hidden WGL context, claimed by the first thread
to draw). The GPU only rasterizes; textures, tests, blending and masks run in
a fragment shader on integers in an ordered interlock, with WritePixel's maths on
R32UI images. No fixed-function ROP, so masks like fbmsk 0x80000000 are exact. It
wraps a GSCpuBackend that still owns local memory, transfers, CLUT and Present.
Render targets are written back only when read: Present syncs just the displayed
buffer (syncing all 60x/s cost 40% of the frame rate). Textures decode per page,
via m_pageVersions. Feedback draws read a snapshot of the target.
`renderer = gpu|cpu` in settings.ini, PS2X_RENDERER overrides, falls back to cpu
without interlock (AMD on Windows lacks it).

Offline: `GS_REPLAY_GL=1 GS_REPLAY_COMPARE=prefix gs_replay capture 1` compares
with the capture's end VRAM and writes got/want/diff BMPs. Field capture 34.3 ->
12.8 ms/frame. In game the field went from 20 fps to a locked 30, with eeidle
~50%. VU1 is now the limit in heavy scenes.

Differences from CPU (GS_REPLAY_GL syncs everything at the final FINISH; without
that, undisplayed buffers look wildly different): bplay2 0.03%, hpcap2 0.02-2.3%,
hpcap 0-2.7%, gscap1 43% but nearly all within 8 levels, ffcap1 4-10%, mostly one
streak area: feedback triangles (tex = target, ALPHA 0x64) read pre-draw pixels on
GPU, while the CPU's ordered draw reads pixels it just wrote and smears the ground
beside Shion. GPU looks right but is unverified: compare with PCSX2.

Two bugs after the first install, both missed because tests read frames from
memory, not the window (use PS2X_SCREENSHOT_AT to see the real window):
- ps2x_createGlBackend must restore the window thread's GL context, or the window
  is black while the game runs.
- Z rounding: uint(z + 0.5) in a float rounds up from 2^23, so z 0xFFFFFF became
  0x1000000 and won equal-depth tests. That darkened the movie pause text
  (PauseMenu's shadow sprite is ZTST greater at the text's depth). Sprites now use
  a flat Z.

Next: PBO readback instead of stalling in Present, retrain PGO, batch feedback
draws, upscaling (images are 2048x2048; scale viewport/scissor/texel fetches and
downsample on sync), Vulkan for AMD.

## Decomp reference (2026-11-07, runs/rpcfix1, mash1; freeze solved above)

`references/xsg-i-decomp` (not in this repo, stays local) is a shallow clone of
github.com/OpokXeno/xsg-i-decomp, matching C for all 7,645 game functions incl.
SSD.IRX and RSSD.IRX (src/audio), main (src/main), overlays (src/ov*). No
license and derived from the game: read it, never copy it into this repo.
Useful map: EE driver src/main/ssd_init.c + ssd_1.c, IOP driver
src/audio/rssd/rssdinit.c + rssdfunc.c, DMA queue src/audio/ssd/ssd_dma.c, wave
upload src/audio/ssd/ssd_wave.c, per-attack sound upload src/ov01/snd.c
(sndSeTrans).

Real bug fixed: a blocking IOP sceSifCallRpc used the same Sleep state as
SleepThread, so a WakeupThread during the wait (iRssdCallFunc from a DMA
interrupt) ended the wait early. RssdSifClientThread then frees the node
whose packet is the RPC's receive buffer, and the reply is written into freed
memory. New state IopThreadState::WaitRpc: wakeups are counted, completeEeRpc
releases it. `wakesInRpc=` in [iop-watch] counts how often this happens.

That is NOT shown to be the freeze: wakesInRpc stayed 0 in rpcfix1 and mash1,
and neither froze. mash1 (runs/field-freeze/inputs-mash.txt: the session plus
scripted attacks, ~30 sndSeTrans) did not reproduce it. Live freezes: 2 of 2
sessions, at battle state 3 (action start); thread 7 in SsdSpuDmaCompleted
(flags bit 2), IOP client thread in SleepThread. Replays: 0 of 7. Next freeze:
read [iop-watch] (SSD dma_queue +0x180, flags +0; RSSD firstRequest 0x5FA08)
and `[IOP printf args]` after "Ssd function error".

Performance: PS2Recomp (upstream too, checked e563698 = its head) only has
the CPU GS backend. Field: gs 92%, vu1 62%. A GPU backend is the one big lever.

## First field area: window at 1-2 fps fixed, scenery 24% faster (2026-11-02, runs/ffcap1, ffrep1-5)

Beyond's session to the first field area (runs/field-freeze/inputs-last.txt
+ session2.log) replays unattended: the field is at ~140-190 s. GS capture
there: runs/ffcap1/gs_capture.bin (12 frames, 45.4 ms/frame vs 5.9 in battle).
- Why the window was at 1-2 fps while the game made ~20: the presenter
  (`latchHostPresentationFrame`) called `sync()`, waiting for the whole GS
  backlog (up to 64 MB, over half a second in the field). Now with the async
  GS it queues an `AsyncQueue::Present` command carrying the request; the GS
  worker carries it out when it gets there (draws, stores the frame) and
  the window shows the latest stored frame without waiting. One present per
  new game frame (dispfb1 change), at most 4 queued. kMaxPendingBytes 64 MB
  -> 8 MB (about two heavy frames) keeps the picture and input current.
  The fps overlay (F3) counts new frames handed to the window
  (`ps2x_presentedFrameCount`); `[present] window showed N new frames/s`
  is logged every 5 s. ffrep5: field game 20-23 fps, window ~20.
- Why it was heavy: ~4000 big ground triangles per frame, mostly off
  screen (14 Mpix raw vs 0.22 Mpix on screen). `DrawTriangle` tested every
  pixel of the scissor-clipped bounding box; it now solves each row's span
  from the edge equations (+1 px slack, exact test unchanged). Same VRAM
  hash on every capture. Field capture 45.4 -> 34.5 ms/frame (PGO on).
- Still slow: single-thread profile = texture sampling ~48%, WritePixel 17%;
  8 threads only ~3x: ~180 feedback triangles/frame (texture = frame
  buffer) are drawn ordered on one thread with a drain each (~15%), plus the
  main thread draws band 0 and does uploads. Ideas: snapshot groups of
  feedback triangles, SIMD span loop, dedicated band workers.
- Battle freeze not fixed (session2.log, 0x2409F0 again; my IOP interrupt
  change didn't cure it). The replay doesn't freeze: it's a race.
  `PS2X_IOP_WATCH=58da0:190,58f24:10,5fa08:10` (on in Launch Xenosaga.bat)
  dumps SSD's DMA manager (0x58DA0: +0 flags, +0x180 current buffer),
  next-wave state (0x58F24) and RSSD's client queue every 5 s with the IOP
  interrupt state, for the next freeze. (SSD loads at 0x48500, RSSD 0x5C900.)

## Every session is recorded (2026-11-02)

Both launchers set PS2X_INPUT_RECORD=inputs-last.txt (previous session moved
to inputs-prev.txt, previous log to xenosaga-prev.log). Replay a reported
problem with `run_game.sh <name> <s> --input "<game>/inputs-last.txt"`.
The old rec1 recording no longer reaches the field (recorded on a build
with broken battles; it now desyncs into a tutorial fight), and there is no
save on mc0, so the slow first field area (GS 99% until a battle) still has
no unattended repro. Next session of Beyond's that goes there provides one.
VU1 native blocks regenerated with Beyond's field/battle block log
(runs/vu1blocks/game-1102.vbl): 326 blocks, vu1_check all ok.

## Shareable builder and launcher (2026-11-02, launcher-test, runs/fresh1-2)

`Xenosaga Launcher.bat` -> `launcher/launcher.ps1` (WinForms, no extra
installs): pick ISO (checked in PowerShell: ISO 9660 root, SHA-1 of
SLUS_204.69 = fd206d57...), install MSYS2 via winget + pacman packages,
build, play, optimize, settings. Background art only from a local
`launcher/background.png` (gitignored; Beyond's copy is local only).
`tools/build_from_iso.py` does the build (stamps make it resumable, runs at
below-normal priority): extract files from the ISO, clone PS2Recomp
e563698 + runtime patch, tools, setup_recomp, assemble, runtime build
(console hidden), copy exe + 82 DLLs to `game/`. assemble.py/vu1gen.py take
XS_ROOT/XS_PORT/PS2RECOMP so the workspace can live in `work/`.

Clean test from the ISO worked end to end (launcher-test/). The fresh build
ran the opening at 16.6 fps (VU1 13.7 ns/cycle): no native VU1 blocks.
After one Optimize with the blocks the game logged in 95 s: 27 fps (8.0
ns/cycle; Beyond's PGO build is 7.0). So the first build ends with a tuning
step: the game plays `inputs/newgame-to-tutorial.txt` by itself for 210 s
with PS2X_VU1_BLOCKLOG, then the blocks are built in. Logs are archived in
`work/vu1blocks/` (the game only logs blocks it has no native code for, so
each Optimize must keep the earlier ones). Not in the builder yet: PGO.

## Gameplay options from Beyond's PCSX2 patch (2026-11-02, runs/opts1-4)

settings.ini `[gameplay]`, implemented in `xenosaga_overlays.cpp` as
function-table wrappers (no code patches):
- `battle_fast_forward` (default on): Start in battle (mode 0x33868C == 5,
  battle module signature 0xA09A90 == 0x26027568) toggles it; badge ">> x2"
  via `ps2x_setHostBadge`. While on and battle state 0xA5756C != 1, the
  frame limiter `xglRenderSyncMove` (0x22CC08) gets its 60fps mode byte
  (gp-0x37B4 = 0x4DC3BC, 1 = one vsync per frame). The byte is set every
  frame (base value captured while not forcing), never saved/restored: the
  call is suspended in its vsync wait and re-entered, and a restore left the
  game stuck at 60 (opts1-3). Fast frames are capped at one per 16 ms real
  time: the emulated vsync runs fast after a slow stretch to catch up (seen
  at 109 Hz). opts4: actions 35-45 fps (port-bound), back to 30 after.
- `hide_battle_flash`: `sefCreateEffectCf2` (0x2E7830) with id 17 -> id 0.
- `faster_combos`: `sefIsFinishEffect` (0x2E7A18) called from 0xA0CB9C
  (ra 0xA0CBA4) returns done after 4 consecutive waiting ticks (counter
  0xA5A1E8); `battleExec` (0xA09A80, ra 0xA078D8) runs up to 6 steps per tick
  between actions (state 2/8 under handler 0xA0BEB8 moving to 8/1).
- `swap_confirm_cancel` (untested): face remap bytes 0x490DED/EE swapped
  outside battle; `dataPadRead` (0xA1D2D0) result swaps 0x20/0x40 in turn
  states 2-15 except 4 (0xA57578).
Wrappers that run logic after the original check `runtime->m_checkpointYielded`.
Not ported: the pnach's 60 FPS sections (need perf headroom first).

## Modern PC features (2026-11-02, runs/modern2-4, wide1)

Host side, all in `ps2_runtime.cpp` (presenter) and `ps2_pad.cpp`:
- `settings.ini` next to the exe (written with comments on first start):
  fullscreen, aspect, filter, integer_scale, vsync, monitor, show_fps,
  hide_cursor, volume, mute_when_unfocused. `window_state.ini` remembers the
  window size (not written when the game is force-closed).
- Picture shown at 4:3 (was square pixels: 512x448 looked 8:7). Filters:
  sharp bilinear (integer nearest prescale into a render texture, then
  bilinear), smooth, pixel. The prescale pass runs before BeginDrawing.
- Hotkeys F11/Alt+Enter (borderless), F3 fps (counts DISPFB1 flips), F4
  filter, F5 aspect, F12 screenshot (`screenshots/`, PNG written on a worker
  thread). `PS2X_SCREENSHOT_AT=s1,s2` takes them unattended for test runs.
  Screenshots read `GetRenderWidth()` pixels with rlReadScreenPixels:
  raylib's LoadImageFromScreen multiplies by the DPI scale again and reads
  past the framebuffer on Beyond's 125% monitor.
- `SetExitKey(KEY_NULL)`: Escape (a Circle key) used to quit the game.
- Pad: controller and keyboard merged (was keyboard ignored when a pad was
  connected), first of 4 pad slots, radial deadzone 0.15, analog triggers as
  L2/R2, input only on port 0 (port 1 used to mirror port 0).
- Launcher: debug recorders (VU capture, GS TRX log) off by default.

Widescreen (aspect = 16:9, experimental): `xenosaga_overlays.cpp` wraps
`xglCameraViewScreen` (0x22B9B8) through the function table and multiplies
camera+0x28 (horizontal projection scale, rebuilt every call) by 0.75
(`XS_WIDESCREEN_SCALE` to experiment); the presenter stretches to 16:9.
wide1 shows a wider battle view with normal-looking 3D proportions; 2D
HUD/menus stretch. Not checked: field/cutscene cameras, culling at the
screen edges.

Ideas not done: 60 fps (Beyond's PCSX2 pnach `<PCSX2 folder>\patches\6D1276AB.pnach`
has a full 60 fps set plus battle options; needs perf headroom first, GS is
~70% busy at 30), HUD-correct widescreen, internal resolution upscaling
(software GS: 4x the fill cost per 2x).

## Field battle freeze: IOP interrupts ignored CpuDisableIntr (2026-11-02, runs/field-freeze, irqfix1)

Beyond's first field battle froze (fps 0). EE thread 7 spun in
`SsdSpuDmaCompleted` (0x2409b0) waiting for bit 2 of 0x4AA080 ("wave DMA
busy", set by `SsdNextWaveData`/`RssdBackNextWaveThread` with cmd 0x21). Only
`RssdBackgroundNextWave` (EE RPC sid 0x901 fn 0x25) clears it. On the IOP,
RSSD's `RssdSpuDmaTransferCallback` sends fn 0x25; SSD calls it from
`SsdWaveTransferInterruptProcess` when the SPU2 DMA for the wave finishes.
All IOP threads were asleep: the completion was lost.

SSD's `SsdDmaStartTransfer` edits its DMA queue (`+0x180` current buffer,
`+0x1c` next links) between CpuDisableIntr/CpuEnableIntr (intrman #8/#9).
`iop_emulator.cpp` dispatched DMA completion interrupts after every
instruction regardless, so one landing inside that window could finish the
current buffer while the thread appended to it: that transfer never starts
and never calls back. Fix: `runCpu` services interrupts and guest callbacks
only while interrupts are enabled, and stretches the timeslice (up to 200k
extra instructions) so a thread is not switched out inside a critical
section. Between slices nothing changed. Tutorial battle replay (play3)
still plays to the results screen at 30 fps. The race is random; it needs
Beyond's field play to confirm.

Tools: `py -m pip install capstone pyelftools`; the scratchpad `irxdis.py`
disassembles IRX functions by symbol (RSSD/SSD keep their symbol tables).

## Battles play through (2026-11-02, runs/rep6)

- Enemy model missing + turn stall: the battle copies the field's enemy
  model (3 parts, 547904 bytes, dataEnemyMdlChk) with xglMemCopy16b
  (backwards, lq/sq loop whose head is the function's first instruction).
  A checkpoint inside that loop returned with ctx->pc == the function
  entry, and dispatchGuestBranch treated "pc == entry" as an HLE-style
  return, so the caller continued with the copy cut short: only the end
  (JNT skeleton) arrived, the lex model and XTX textures were garbage, the
  enemy was invisible and its action motion (Mot=5) never finished.
  Fix: PS2Runtime::m_checkpointYielded (set when generated code or
  dispatchGuestBranch stops at a checkpoint, cleared by the scheduler before
  it resumes a thread); dispatchGuestBranch leaves ctx->pc alone when set.
  This affected any function whose loop starts at its first instruction.
- Shion HP display ("0080/0000" etc.): NOT a game rendering bug. Only the
  heartbeat snapshot (xs_frame.bmp / XS_FRAME_KEEP frames) shows it: it
  reads VRAM at an arbitrary moment, mid-way through redrawing the HUD. The
  live end-of-frame VRAM in a battle GS capture (runs/hpcap2) shows
  "0080/0080", and gs_replay of that frame is byte-identical to it. (The HUD
  font at TEX0 0x3800 is uploaded as CT32 512x128 and read as T8; later
  uploads in the same frame overwrite it, so a mid-frame snapshot can catch
  digits drawn after... it is the snapshot, not the GS.) Don't trust
  heartbeat frames for fine HUD detail; use a GS capture's end VRAM.
- PS2X_GUEST_PRINTF=1 prints the game's own printf output (very useful:
  "** MDL read Size", "** Create (ID=..)", "** NBread: ...").
- Tutorial battle replay now: enemy drawn, both characters act, results
  screen (Clear Time 0:30, EXP +2). Still odd: Shion's HP shows 0000/0080,
  0000/0000 or 0080/0080 depending on the frame; "SetTrans err" (enemy SE
  bank busy) still printed while the enemy acts.

## Battle root causes found (2026-11-02, runs/btut*, bfix*)

Traced with XS_BATTLE_TRACE on the deterministic tutorial-battle replay
(runs/bplay2-inputs.txt) and RAM dumps at battleMain #30 (xs_ram_battle.bin):

1. **Recompiler patched out 66 stores.** ps2_analyzer replaces stores it
   thinks write code with nops ("Potential self-modifying code"); all false
   positives (overlay data sits next to overlay code). Among them
   `sw v0,(s0)` at 0xA1D5FC in thinkInit, which stores pThinkTop
   (0xA59BE8): without it thinkMonsTblGet reads the monster table from
   address 0, no enemies are created (unit slots 3-7 empty), battleEndChk
   sees no live enemies and the battle ends at once as a win with 0 EXP.
   setup_recomp.py now drops those patches (main 6, ov01 42, ov02 3, ov10 7,
   ov11 6, ov12 2); code regenerated.
2. **Runtime RPC placeholders lived in game RAM.** IOP-side servers get a
   placeholder sd address from a pool at 0x1F10000 (packets 0x1F00000, TLS
   0x1F20000) - ordinary Xenosaga RAM. sceSifCallRpc wrote server fields
   there and copied the send buffer to the garbage "buf" it read back: every
   65568-byte sound-bank chunk (rpc 33) landed at ~0x42AF20, over batdat.bin
   and the think data at 0x436F40. Placeholders are now never read/written
   (rpcIsPlaceholderServer); the pools no longer zero guest RAM. TLS pool
   still writes guest RAM (not yet checked).
- Result (runs/bfix4): the tutorial battle now runs - arena, characters,
  "Drone M" target, battle tutorial text, Knuckle/Firecracker menu and GTW;
  30 fps, memory flat ~270 MB. To check with real play: field battles,
  Shion's HP display ("0000/0080" in later frames), post-battle field.
- Beyond's play test (runs/play3, inputs runs/play3-inputs.txt): tutorial
  battle starts, Shion can act, but the Drone M model is never drawn and
  the battle stalls after Shion's turn (KOS-MOS never gets a turn); Shion's
  HP display alternates 0080/0000 and 0000/0080. The enemy file is read
  (sceCdRead lsn 0x7dab3, 15 sectors -> 0x610950, extract no-op) and
  feso_001.bin (motions) loads: next, follow unitLoad / the enemy model
  setup and what the turn system waits for.
- OV01 layout facts: unit table 0xA57240 (3 party + 5 enemy slots),
  batdat.bin -> 0x426F40 (data\yamamotoatdat.bin, 29 sectors), think
  data 0x436F40 (16 KB, copied from the map archive by dataThinkDataSet in
  MapReset; think%03d.bin only via dataThinkFileLoad).

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
- Only overlay 1 is ever activated, but that is expected: field battles
  live in OV01 itself (battleInit/battleMain/battleProc/battleDisp/
  battleEndChk, encounter effect encEffVramCopy/grVramCopyFBtoFB). OV10 is
  the Xenocard minigame (CardPlay*), OV12 the "rg_" robot minigame, OV02
  the title/menu dispatcher (ControlEntry 0x218240 loads ov02, calls
  0xA00E18 for the next module: id in bits 31-24, entry in 23-0).
- So the battle bug is an emulation bug inside OV01. Suspects: the encounter
  transition's VRAM copies (local->local transfers / FB-to-FB copy) and a
  fade that never finishes (fast build: battle UI faint over black; field
  black afterwards except characters/HUD), and something timing-related
  that makes the battle end at once in the slow build.
- runs/bplay2-inputs.txt (Beyond, 2026-11-02) reaches the scripted tutorial
  battle ("Entering battle mode") and replays deterministically (~110 s).
  XS_BATTLE_TRACE=1 logs OV01 battle functions called through the function
  table (intra-overlay JAL/J are direct C++ calls and are not seen) and arms
  the GS capture (PS2X_GS_CAPTURE=99999,90) at battleInit.
- Tutorial battle: black 3D with UI, ends after ~3 s as a win with 0 EXP,
  Shion max HP shown as 0000. battleEndChk: win when unitLiveNumGet2(1)==0
  (alive = no 0x80 flag and calcStatGet(unit,0/4,0x2000) status bits clear).
  unitTblGet(2)=8 units, side 0 = 3 units.
- GS capture of the battle (runs/bplay2/gs_capture.bin) replays identical to
  the game's VRAM; Z buffer is cleared normally each frame there (the
  0xFFFFFC Z seen in Beyond's field battle dump comes from a per-frame
  852 KB image upload over the Z area, cleared again by a small packet).
- The capture used to Flush the GS on every packet while armed (game at
  4 fps); it now flushes only when it starts/ends.
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

## IPU movies (boot logos) play (2026-11-07, runs/ipu-boot9, ipu-regress)

Black boot logos (namco.ipu, logo_msi.ipu; also movieull|small\*.ipu) =
the IPU was not emulated. Story movies (.pss, sceMpeg via FFmpeg) already played.
- src/lib/ps2_ipu.cpp (Ps2Ipu): BCLR, IDEC (MPEG-1/2 intra macroblocks, VLC
  tables from ISO annex B, reference IDCT, PS2 CSC coefficients, TH0/TH1 alpha,
  RGB32/RGB16), FDEC, SETTH; SETIQ/SETVQ ignored (default intra matrix).
  tools/ipu_test.cpp decodes a .ipu file offline (tools/xsarc.py extracts one).
- PS2Memory: IPU registers; channel 4 (toIPU) is pulled on demand by
  ipuFetchQword, following chain tags, and reads its first tag when started
  (xglMoviePlay checks D4_MADR right after the start; otherwise its ring logic
  jumped to chain[1], the file header); channel 3 (fromIPU) copies decoded
  output (ipuServiceFromDma). BP/IFC/FP report fetched-but-unused quadwords so
  the game's rewind math works. PS2X_IPU_LOG=<n> logs commands/DMA.
- Second bug: DMA addresses with bit 31 (MADR/TADR/tag ADDR = scratchpad +
  offset) were read as RAM (KSEG0) and tag ADDR lost bit 31. The movie's GIF
  upload chains live in scratchpad and read zeros. GIF/VIF chain code now maps
  bit 31 to the scratchpad (dmaAddr in writeIORegister). Regression replay:
  same fps (28.5 avg), no crash.
