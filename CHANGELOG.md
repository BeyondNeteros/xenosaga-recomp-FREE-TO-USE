# Changelog

Notable changes to Xenosaga Episode I: Recompiled are recorded here. This project is still in development and does not yet have a stable release.

## 2026-11-24

### Runtime

- Added strict validation of native VU1 blocks against the interpreter.
- Added ABI compatibility checks to prevent linking incompatible native block objects.
- Restricted experimental whole-program VU1 execution to offline replay.
- Validated native block execution across 3,600 captured calls on both Linux and Windows, with matching architectural state, GIF output and cycle counts.

### Build and testing

- Added a Debian WSL2 build workflow for Windows x64 using MinGW-w64 and FFmpeg.
- Added build preflight checks, repeatable executable relinking, DLL dependency verification and local staging tools.
- Added host regression tests and automated VU1 replay and gameplay comparison tools.
- Updated the launcher to skip automatic tuning when the training recording is unavailable. Manual optimization remains available after gameplay.
- Completed a Windows gameplay smoke test of the integration contribution.

### Performance

Controlled gameplay comparisons on the validated development baseline showed the following improvements with locally generated native VU1 blocks:

| Scene | Interpreter | Native blocks | Speedup |
|---|---:|---:|---:|
| Exploration (1x) | 85.1 s | 53.3 s | 1.60x |
| Exploration (3x) | 85.3 s | 52.5 s | 1.62x |
| Later gameplay (1x) | 61.9 s | 40.3 s | 1.54x |
| Later gameplay (3x) | 58.8 s | 46.3 s | 1.27x |

Test system: Intel Core i9-13900K, NVIDIA RTX 4090, 64 GB RAM, Windows 11, OpenGL.

These results represent one recorded route with one paired trial per resolution, not a full-game benchmark or a guaranteed improvement on every system.

### Documentation

- Updated the README, project banner and gameplay screenshots.
- Added the [WSL2 build guide](docs/BUILD_WSL.md) and [VU1 performance documentation](docs/VU1_PERFORMANCE.md).
- Clarified installation, manual optimization and known compatibility limitations.

### Known limitations

- Demanding scenes remain below the 30 FPS target.
- In-engine cutscenes can exhibit significant audio desynchronization.
- Seven existing GS regression tests remain unresolved.
- Full Windows launcher installation and full-game compatibility have not been validated.

Related pull requests: [#1](https://github.com/gwencrow/xenosaga-recomp/pull/1), [#2](https://github.com/gwencrow/xenosaga-recomp/pull/2), [#3](https://github.com/gwencrow/xenosaga-recomp/pull/3).

## Earlier development

The original port was developed by [BeyondNeteros](https://github.com/BeyondNeteros/xenosaga-recomp-FREE-TO-USE). Earlier development history is preserved in Git. This changelog begins with the continued development of the fork.
