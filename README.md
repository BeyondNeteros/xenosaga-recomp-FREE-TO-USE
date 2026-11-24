# Xenosaga Episode I: Recompiled

![Development Status](https://img.shields.io/badge/status-active_development-blue)
![Platform](https://img.shields.io/badge/platform-Windows_x64-blue)
![License](https://img.shields.io/badge/license-GPLv3-green)

An unofficial native PC recompilation of **Xenosaga Episode I: Der Wille zur Macht**, originally released for the PlayStation 2.

Built using [PS2Recomp](https://github.com/ran-j/PS2Recomp), this project aims to bring Xenosaga Episode I to modern PC hardware through static recompilation, improved performance, enhanced rendering options, and native PC controls.

Originally developed by [BeyondNeteros](https://github.com/BeyondNeteros/xenosaga-recomp-FREE-TO-USE), this fork actively continues development with a focus on performance, rendering accuracy, stability, and accessibility.

> **Active development — Work in progress**
>
> This project is not yet a complete or production-ready PC port. No copyrighted game files are distributed. You must provide your own copy of the original game.

## Development Status

The project is currently playable through the Virtual Tutorial and into the early field areas. Exploration, cinematic sequences, and early-game battles have been successfully tested during development.

| Component | Status |
|---|---|
| Main executable (`SLUS_204.69`) | Recompiles successfully |
| Game overlays (`OV01/02/10/11/12.OVL`) | Recompile successfully; loaded dynamically |
| Boot and dual-layer ISO access | Working |
| Menus, text, FMVs and 3D scenes | Working in tested areas |
| Battles and exploration | Working in tested early-game areas |
| Sound and music | Working; synchronization improvements ongoing |
| Keyboard and controller input | Supported |
| GPU-accelerated rendering | Supported |
| Internal resolution scaling | Supported |
| Runtime performance | Active optimization |
| Widescreen rendering | Experimental |
| Full-game compatibility | Not yet validated |

## Development Roadmap

### Performance

- [ ] Reduce remaining VU1 execution overhead.
- [ ] Improve GS rendering and synchronization performance.
- [ ] Achieve consistent 30 FPS gameplay.
- [ ] Improve performance at higher internal resolutions.

### Graphics

- [ ] Improve texture and shadow rendering accuracy.
- [ ] Resolve intermittent graphical artifacts.
- [ ] Improve native-resolution presentation.
- [ ] Improve widescreen HUD and menu behavior.

### Stability and Accessibility

- [ ] Expand story progression and battle compatibility testing.
- [ ] Improve audio synchronization.
- [ ] Expand hardware and graphics driver compatibility.
- [ ] Improve installation and build procedures.
- [ ] Establish comprehensive regression and performance testing.

## Features

### Graphics and Display

- GPU-accelerated OpenGL rendering.
- Adjustable internal resolution.
- Borderless fullscreen.
- Original 4:3 aspect ratio.
- Experimental 16:9 widescreen support.
- Sharp, smooth and pixel scaling filters.
- Integrated frame-rate counter.
- Screenshot capture.

### Controls and Gameplay

- Keyboard and controller support.
- Analog stick deadzones.
- Configurable confirm and cancel behavior.
- Battle fast-forward.
- Optional faster combos.
- Optional battle screen flash suppression.

## Getting Started

### Requirements

- Your own copy of **Xenosaga Episode I: Der Wille zur Macht (USA)**.
- Game serial: `SLUS-20469`.
- A compatible disc image in ISO format.
- Windows 10 or Windows 11, 64-bit.
- An AVX2-capable processor.
- Approximately 10 GB of available storage for the standard build workflow.
- An OpenGL 4.5 graphics card with fragment shader interlock support for GPU-accelerated rendering.

A CPU renderer is available as a fallback. Minimum system requirements and broader hardware compatibility are still being evaluated.

### Installation

1. Obtain the project source.
2. Double-click **`Xenosaga Launcher.bat`**.
3. Select **Choose disc image...** and locate your ISO.
4. Select **Install build tools** to install the required MSYS2 build environment through Windows Package Manager.
5. Select **Build the game** to analyze, recompile and compile the original game executable.
6. Once compilation completes, select **PLAY**.

All game-derived files are generated locally on your computer. Your original ISO remains required for gameplay.

Build times vary depending on your hardware.

### Launcher Options

- **Optimize:** builds optimized versions of locally observed graphics routines after gameplay.
- **Settings:** opens `settings.ini`.
- **Custom background:** place an image at `launcher/background.png` to customize the launcher.

The existing launcher workflow is experimental and may encounter build environment compatibility issues on some systems.

## Playing

Launch the game using the launcher's **PLAY** button. Manual installations can use `dist/Launch Xenosaga.bat`.

On first launch, the game creates a `settings.ini` file beside the executable.

### Display Shortcuts

| Key | Function |
|---|---|
| F11 / Alt+Enter | Toggle borderless fullscreen |
| F3 | Toggle frame-rate counter |
| F4 | Change scaling filter |
| F5 | Change aspect ratio |
| F12 | Capture screenshot |

Screenshots are saved to the `screenshots` directory.

### Keyboard Controls

| Input | PlayStation 2 Control |
|---|---|
| WASD / Arrow keys | Movement |
| X / Space | Cross |
| C / Escape / Backspace | Circle |
| Z | Square |
| V | Triangle |
| Q | L1 |
| E | R1 |
| Shift | L2 / R2 |
| Enter | Start |
| Tab | Select |

Compatible controllers and the keyboard can be used simultaneously. Analog sticks use a small deadzone to reduce unwanted movement, and analog triggers function as L2 and R2.

### Graphics Settings

The GPU renderer can be enabled through `renderer = gpu` in `settings.ini`.

GPU acceleration requires OpenGL 4.5 with fragment shader interlock support. Systems without the required graphics capabilities fall back to the CPU renderer.

The game is displayed in its original 4:3 aspect ratio by default, using sharp bilinear scaling. Experimental 16:9 support expands the 3D view, although menus and HUD elements may appear stretched.

### Audio and Gameplay Settings

Audio volume can be configured through `volume` in `settings.ini`. The `mute_when_unfocused` option controls audio behavior when another window is active.

The `[gameplay]` section provides additional options, including battle fast-forward, faster combos, battle screen flash suppression, and reversed confirm/cancel controls.

The game also remembers its window size and automatically hides the mouse pointer after a short period of inactivity.

## Known Issues

The following issues have been observed during development testing:

- Reduced frame rates in demanding exploration areas and in-engine cutscenes.
- Additional rendering overhead at higher internal resolutions.
- Intermittent texture, shadow and geometry artifacts.
- Screen shaking in certain scenes at native internal resolution.
- Audio stuttering or synchronization issues during slow gameplay.
- Stretched menus and HUD elements in widescreen mode.
- Incomplete hardware compatibility and full-game progression testing.

These issues are being investigated with an emphasis on preserving original game behavior and rendering accuracy.

## Technical Overview

The project uses static recompilation to translate the original PlayStation 2 game executable into native host code.

The retail executable and overlays retain their original function names, allowing recompiled functions to preserve their original symbols, including `scParseScript`, `ACT_draw`, and `command_mpeg2`.

The game loads five code overlays into the same address (`0xA00000`) through `xglCdLoadOverlay`. Each overlay is recompiled separately using an `xs_ovNN_` prefix.

The custom overlay loader in `src/xenosaga_overlays.cpp` ensures the appropriate overlay functions are active at runtime.

The port targets [PS2Recomp](https://github.com/ran-j/PS2Recomp) commit `e563698`, together with project-specific modifications provided in `patches/ps2recomp-runtime.patch`.

Ongoing development focuses on Vector Unit 1 (VU1) execution, Graphics Synthesizer (GS) performance, rendering accuracy, and runtime stability.

## Repository Structure

| Directory | Purpose |
|---|---|
| `src/` | Game-specific runtime hooks and overrides |
| `tools/` | Recompilation, build and diagnostic utilities |
| `patches/` | PS2Recomp runtime modifications |
| `launcher/` | Windows launcher components |
| `dist/` | Runtime launch scripts |

## Building from Source

See [CLAUDE.md](CLAUDE.md) for the detailed developer setup.

1. Extract `SYSTEM.CNF`, `SLUS_204.69`, and the `OV*.OVL` files from your own disc image.
2. Analyze and recompile the main executable using `ps2_analyzer` and `ps2_recomp`.
3. Adjust each overlay's ELF entry point to its first function, then analyze and recompile the overlays separately.
4. Run `python3 tools/assemble.py` to assemble the generated runtime source.
5. Build the `ps2EntryRunner` target.
6. Launch the compiled runtime with your locally extracted executable and disc image.

Additional development work is underway to provide an alternative Windows cross-compilation workflow using WSL2, Debian and MinGW-w64.

## Contributing

Contributions, technical investigations, and reproducible bug reports are welcome.

When reporting an issue, please include:

- Operating system.
- CPU and GPU specifications.
- Graphics driver version.
- Internal resolution and renderer.
- Project commit or build version.
- Steps to reproduce the issue.

Please do not upload copyrighted game files, generated game source, memory dumps, or other proprietary game-derived content.

## Credits

- [BeyondNeteros](https://github.com/BeyondNeteros) — original Xenosaga recompilation project and foundational port development.
- [PS2Recomp](https://github.com/ran-j/PS2Recomp) — underlying PlayStation 2 static recompilation framework.
- [FFmpeg](https://ffmpeg.org/) — multimedia decoding components.
- All contributors to the project and the wider PlayStation 2 recompilation community.

## License

Copyright (C) 2026 BeyondNeteros and contributors.

The code in this repository (the port code, launcher, tools and the
PS2Recomp runtime patch) is free software: you can redistribute it and/or
modify it under the terms of the **GNU General Public License, version 3**
(see [`LICENSE`](LICENSE)), the same license as
[PS2Recomp](https://github.com/ran-j/PS2Recomp), which this port builds on and
patches. Parts of the movie decoder's MPEG tables come from FFmpeg
(LGPL 2.1 or later), credited in that file.

The license covers this project's own code only:

- **Xenosaga Episode I is not included and not licensed here.** The game, its
  code, data, movies and audio, and anything generated from them (such as the
  recompiled C++ the launcher builds on your PC), belong to their owners
  (Bandai Namco / Monolith Soft). You need your own copy of the game. Don't
  share game files or builds that contain them.
- The game decompilation used as a reference during development is not part
  of this repository and is not covered by this license.

This is an unofficial fan-developed project and is not affiliated with or endorsed by the original developers or publishers.
