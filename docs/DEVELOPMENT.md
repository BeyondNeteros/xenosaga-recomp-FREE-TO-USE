# Development guide

Xenosaga Episode I Recompiled uses PS2Recomp to translate the USA release
(`SLUS-20469`) and its five code overlays. The port applies
`patches/ps2recomp-runtime.patch` to PS2Recomp revision `e563698`. See the
[contribution guide](../CONTRIBUTING.md) for licensing and submission rules.

## Build and edit

- On Windows, use `Xenosaga Launcher.bat` to select your own disc image,
  install the MinGW build tools and build the game. The launcher calls
  `tools/build_from_iso.py` to extract, generate, assemble and build.
- For manual builds, follow the [WSL2 build guide](BUILD_WSL.md). The
  analyzer and recompiler can run natively on Linux while MinGW-w64
  targets Windows x64. The guide covers FFmpeg, CMake and private staging.
- `tools/setup_recomp.py` generates the game C++ from the extracted retail
  executable and overlays. Use absolute paths for `--bin` when invoking it
  manually. `tools/assemble.py` stages the generated runner and headers
  together with the checked-in hooks from `src/`. Set `XS_ROOT`, `XS_PORT`
  and `PS2RECOMP` to select the workspace, source worktree and patched
  PS2Recomp checkout when using a custom layout.
- Edit the original hooks in `src/` and rerun assembly. Changes to the
  recompiler require rebuilding its tools, regenerating and reassembling
  the game code. Runtime-only changes normally require rebuilding only
  the affected objects, though common headers can invalidate many
  generated translation units.
- Edit the runtime in a clean, isolated PS2Recomp checkout with the
  project patch applied. Review the upstream-relative diff and exclude
  generated runner files, game-specific generated headers and local
  build data. Mark new **handwritten** patch files with `git add -N` so
  they appear in the diff. Verify the refreshed patch with
  `git apply --check` in a clean checkout of the pinned revision.

## Code map

| Area | Entry points |
|---|---|
| Overlay loading | `src/xenosaga_overlays.cpp` hooks `xglCdLoadOverlay`. The overlays share guest load address `0xA00000`. The hook switches to the corresponding generated function table, whose symbols are prefixed with `xs_ovNN_`. |
| Disc access | `src/xenosaga_disc.cpp` handles the game's sector-based disc access. `src/xenosaga_iso.h` provides ISO 9660 lookup. |
| Runtime | The PS2Recomp patch covers EE scheduling, memory and DMA, VIF/VU1, GIF/GS, and device emulation. Work within the patched source and trace both the guest entry point and any host workers it dispatches. |
| Guest and renderer diagnostics | `src/xenosaga_debug.cpp` supplies the heartbeat. It reports guest thread PCs, VU and GS timing, and display state. The captured `xs_frame.bmp` can help inspect rendering failures. |
| VU0 calls | The recompiler patch translates `VCALLMSR` using `ctx->vu0_cmsar0`. Check generated code as well as the translator when diagnosing VU0 macro-mode calls. |

## Diagnose and validate

- First reproduce with a known build and record the revision, renderer,
  resolution, audio and VU1 settings, and the point in the route. Use a
  fresh trial directory per attempt and retain original logs. Compare
  like-for-like settings, route and save inputs. Treat early-close tests
  as shutdown tests and use complete routes for timing comparisons.
- `dist/Launch Xenosaga.bat` passes `XS_DISC` through as
  `XS_DISC_IMAGE`. VU capture is opt-in. Enable `PS2X_VU_CAPTURE=1` for
  local diagnosis and build the optional `vu1_replay` target with
  `PS2X_BUILD_VU_REPLAY=ON` to replay captures offline.
- `tools/vu1_gameplay_ab.bat` stages separate Windows trials using a
  shared private seed. Use the [VU1 validation guide](VU1_PERFORMANCE.md)
  for replay gates, matched route checks and host build commands.
  `tools/run_game.sh` is a separate timed diagnostic helper. It can
  terminate a running game and collects artifacts, so use it with a
  dedicated private install. Its forced termination cannot validate
  normal shutdown.
- Pad scripts are consumed by the runtime. Input rows use
  `frame buttons [lx ly rx ry]` with frames counted by port-0 pad reads.
  Optional `@ frame key` rows synchronize against guest events.
  `tools/addsync.py` adds synchronization points from a matching log.
- `tools/eedis.py` disassembles EE code by symbol or address using
  Capstone and pyelftools. Check the raw instruction word when a PS2
  opcode is decoded as a different MIPS instruction or shown as data.
  `tools/vudis.py` inspects VU microcode from a capture and
  `tools/gifparse.py` summarizes GIF packets.
- On Windows, `XS_PROFILE=start,seconds` starts a local sampling profile.
  Use `tools/profsym.py` with the executable from that run and MinGW's
  `nm` to symbolize the output in `xs_profile.txt`.
- `tools/pgo.sh` is an optional developer profiling workflow using private
  captures or a gameplay run. Run the strict replay gate again after a
  PGO rebuild. `tools/vu1_check.sh` compares against a separately prepared
  reference executable for diagnosis. Use `tools/vu1_validate.py` for
  the sealed-suite acceptance check.
- Run targeted checks and the host suite against the same patched
  revision used for the build. When comparing regressions, run the
  same tests against the baseline and report both outcomes. Confirm
  loader, controller, graphics-driver and shutdown behavior on Windows
  when a change reaches those paths.

Keep generated sources, game-containing executables, private inputs and
raw evidence local. Share only reviewed diagnostics that omit private
paths, personal data and game-derived content.
