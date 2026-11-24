# Contributing

Contributions are welcome, whether you write code, test gameplay, improve documentation or report bugs.

## Find something to work on

Browse the [issue tracker](https://github.com/gwencrow/xenosaga-recomp/issues). Look for `good first issue` or `help wanted` labels. If you plan to work on an existing issue, leave a comment describing your approach so others can coordinate with you. Discuss larger architectural changes before implementing them.

## Development setup

You need your own USA copy of Xenosaga Episode I (`SLUS-20469`). The game is recompiled locally; it is not distributed with this project.

- [WSL2 build guide](docs/BUILD_WSL.md)
- [Development guide](docs/DEVELOPMENT.md)
- [VU1 performance and validation](docs/VU1_PERFORMANCE.md)

The runtime is based on PS2Recomp commit `e563698` with the project patch in `patches/ps2recomp-runtime.patch`. Follow the documented build process rather than committing changes to a generated runtime checkout.

## Submit a change

1. Fork this repository and create a branch from its current `master`.
2. Keep the change focused on one issue or objective.
3. Test the change and run `git diff --check` before committing.
4. Push the branch to your fork and open a pull request against this repository's `master`.
5. Describe the change, link the related issue and report what you tested, including failures or tests you could not run.

There is no need for direct write access. Maintainers review contributions before merging.

## Testing

For runtime changes, run the relevant host regression tests and, where practical, a Windows gameplay test. For performance changes, compare the same scene or recorded route with matching settings. VU1 changes must preserve architectural state, ordered GIF output and guest cycle counts in the supported strict replay tests. Do not enable experimental whole-program execution in gameplay without separate validation.

The project has known regression failures and incomplete gameplay coverage. Report existing failures separately from new ones. Documentation-only changes do not require a game rebuild.

## Reporting bugs

Include the build commit, OS, CPU, GPU, driver, renderer, resolution and steps to reproduce the problem. Mention whether native VU1 optimization was used. If possible, include relevant sanitized log excerpts and describe the affected scene without unnecessary story spoilers.

## Licensing and private data

Contribute only material you have the right to submit under the project's applicable license. Preserve original attribution and third-party notices. Do not commit or attach disc images, game assets, generated game code, game executables, saves, raw memory dumps or game-derived captures. Remove personal information, credentials and local paths from diagnostics before sharing them. See [LICENSE](LICENSE) and the README for the scope of GPLv3.

Please follow the [code of conduct](CODE_OF_CONDUCT.md).
