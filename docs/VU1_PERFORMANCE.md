# Native VU1 block validation

The block generator specializes VU1 instruction pairs observed in locally
collected VBL1 logs. The runtime selects a registered block only when the
live block matches. The shared scheduler handles readiness, delayed writes,
cycle budgets and PATH1 transfer timing. Other blocks and dynamic paths
execute through the interpreter. Set `PS2X_VU1_NATIVE=0` for the interpreter
control, or `PS2X_VU1_NATIVE=1` to allow registered native blocks.

A Windows gameplay crash exposed a mismatch between the generated block
object and the runtime's `VU1Interpreter` layout. Replay controls and
counters now travel with the replay call. Generated registrations refer to
a linker symbol derived from the compiled layout. Regenerate and rebuild
native block objects against the same headers as the runtime. The ABI
fixture checks a matching link and a deliberate layout mismatch.

## Generate and validate locally

1. Use a locally built game or replayer to collect VBL1 records with
   `PS2X_VU1_BLOCKLOG` set to a private output path. Save independent XVU1
   captures for verification. Keep all inputs, generated C++, and results
   outside this repository.
2. Run `python3 port/tools/vu1gen.py <private-vbl1-file> ...`. The tool writes
   the block translation unit to `recomp/` and copies it to the PS2Recomp
   runner directory. The assembler also picks it up.
3. Configure the patched PS2Recomp runtime with
   `-DPS2X_BUILD_VU_REPLAY=ON`. Build the `vu1_replay` target with the same
   compiler and runtime headers used for the block object.
4. Place the independent input files in `training/` and `held-out/` under
   a private suite directory. The sealed manifest binds both groups and
   records the expected number of registered blocks. Use the count from
   the generator and replayer.

```sh
python3 port/tools/vu1_validate.py seal "$suite" --blocks "$block_count"
python3 port/tools/vu1_validate.py run "$replayer" "$suite" "$new_results"
```

The default block gate requires every sealed capture, matching per-entry
counts, zero state, ordered GIF stream and cycle differences, zero program
registrations and dispatches, and native-block dispatch on every recorded
call in block mode. The interpreter control must dispatch zero native
blocks. Timing starts separately after the comparisons. The result passes
only when the command exits zero and writes `PASS.txt`. For a Windows
replayer, place `vu1_validate_windows.bat` and `vu1_validate.py` next to
`vu1_replay.exe` and pass the suite and a fresh results directory.

The inherited immediate-write whole-program implementation had 3,443 cycle
mismatches on the sealed suite. The publication branch removes its emitter
and rejects sequential registration. Scheduler-backed regions remain an
offline experiment. They use the runtime scheduler and need a private
generated region file in `ps2xRuntime/tools`, an explicit
`-DPS2X_VU_REPLAY_REGIONS=ON` replayer build and the explicit `--gate programs`
validation mode with region targets sealed in the suite. The gameplay
runner excludes generated program code, and gameplay calls cannot dispatch
regions.

## Gameplay comparison

Use one patched stage and one private seed for both trials. From a local
benchmark folder in Windows Command Prompt, set `XS_AB_STAGE` to the
staged runner directory and `XS_AB_SEED` to a directory with both memory-card
slots. Supply private `ab-settings\gpu-1x.ini` and
`ab-settings\gpu-3x.ini` files with the same settings apart from resolution.
Set `AB` to the full path to `tools\vu1_gameplay_ab.bat`, `ISO` to your
disc image and `ROUTE` to your recorded replay script.

```bat
call "%AB%" interpreter 1 control-1x "%ISO%" "%ROUTE%"
call "%AB%" blocks 1 blocks-1x "%ISO%" "%ROUTE%"
call "%AB%" interpreter 3 control-3x "%ISO%" "%ROUTE%"
call "%AB%" blocks 3 blocks-3x "%ISO%" "%ROUTE%"
```

Use fresh trial names. Pass `record` in place of the route path to record
a new route or `manual` for a manual check. The launcher copies both
memory-card slots and keeps the trials in separate directories. Verify the
stage, seed and route inputs match and keep the CSV and raw logs local.

```sh
python3 port/tools/vu1_gameplay_benchmark.py "$route" "$control_log" "$blocks_log" \
    --finish-frame "$expected_completion_frame"
```

The extractor checks all ordered landmarks, route-load counts and replay
completion before emitting timestamps. Subtract matching start and end
timestamps to compare segments. The baseline used four complete attended
Windows trials with 89 matching landmarks and replay completion at pad
frame 2577. The stage also passed manual progress through the route and
an internal-resolution change.

| Woglinde segment | Interpreter 1× | Native blocks 1× | Interpreter 3× | Native blocks 3× |
|---|---:|---:|---:|---:|
| Exploration | 85.1 s | 53.3 s | 85.3 s | 52.5 s |
| Cutscene and later gameplay | 61.9 s | 40.3 s | 58.8 s | 46.3 s |

Exploration was 1.60× faster at 1× and 1.62× faster at 3× on the tested
OpenGL system. Strict block replay passed 3,600 captured calls per platform
on Linux and Windows, including 3,501 ordered GIF packets and zero state,
GIF or cycle differences. Attended measurements apply to the validated
baseline build and recorded route. Whole-game compatibility, stable 30 fps
in demanding 3× scenes, and broader audio and rendering coverage remain open.

## Host checks

```sh
cmake -S port/tests/wsl -B /path/to/host-build -G Ninja -DPS2RECOMP="$PS2RECOMP"
cmake --build /path/to/host-build --parallel 4
ctest --test-dir /path/to/host-build --output-on-failure
```

The patched GS suite exposes seven known visibility failures. Four test
texture visibility after flush, upload, local copy or raster operations.
The other failures cover CLUT page-cache visibility and two memory alias
cases. The synthetic ISO lookup check passes. The original GS assertions
remain in place.
