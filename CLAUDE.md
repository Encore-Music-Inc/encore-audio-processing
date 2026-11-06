# Notes for Claude (and whoever is driving it)

This is ENCORE.'s shared audio processing code: header-only C++20 used by three
products. encore-studio's CLAUDE.md does not apply here; this file does.

## What uses it

- ENCORE. VST (encore-vst): the plugins, built with JUCE (`juce_dsp`).
- ENCORE. Basement Studio (encore-basement-studio): the desktop DAW, built with JUCE.
- ENCORE. Green Room: the browser tools, through `master.wasm` (built from
  `wasm/master_api.cpp` and the headers, without JUCE).

## Decisions (already made, don't revisit)

- Licence: GNU AGPLv3 (`LICENSE`). Copyright holder: Encore Music, Inc.
- The repo will be made public later. Outside contributions are not accepted
  (`CONTRIBUTING.md`). The ENCORE. name and logo are not licensed (`TRADEMARKS.md`).
- Contact: support@encoremusic.studio
- Always write ENCORE. (capitals and the period) in anything people read.
- No long dashes in any text. Use commas, periods or parentheses.
- Don't write "DSP" in names, titles or prose for people (in music business it
  means Spotify or Apple Music). Say "audio processing". Code folders and
  identifiers (`dsp/`) keep the name.
- Anything third-party goes in `THIRD-PARTY-NOTICES.md`. Today that is only JUCE.

## Layout

| Path | What |
|---|---|
| `dsp/` | the headers (`MasterChain.h`, `EqBand.h`, `ReverbEngine.h`, ...) |
| `dsp/lite/JuceLite.h` | stand-in for the JUCE pieces the headers use, for builds without JUCE (`VS_NO_JUCE=1`). Don't delete it: the wasm build needs it |
| `wasm/master_api.cpp`, `wasm/build.sh` | the browser engine and its Emscripten build |
| `tests/` | six offline test suites and their CMake build |
| `external/JUCE` | JUCE 9 submodule pinned at `7ff644f`, for the tests only |
| `.github/workflows/ci.yml` | tests and master.wasm on pull requests and main (skipped when only docs change); `v*` tags attach main's master.wasm to the Release, no rebuild |

## Consumers

- They add this repo as a submodule at `external/encore-audio-processing` and put
  that folder on their include path, so `#include "dsp/MasterChain.h"` resolves.
  Keep includes in that form: never move or rename headers without a major version.
- They must not recurse into this repo's submodules (no `--recursive` for this
  one). They bring their own JUCE; `external/JUCE` here is only for the tests.
- They pin the submodule to a release tag. Green Room pins `master.wasm` by its SHA-256.

## Building and testing

- Tests: `git submodule update --init --depth 1 external/JUCE`, then
  `cmake -S tests -B build -DCMAKE_BUILD_TYPE=Release`, `cmake --build build --parallel`,
  `ctest --test-dir build --output-on-failure`. All must pass before a pull request.
- master.wasm: source `emsdk_env.sh` for Emscripten 6.0.13 (the version in `ci.yml`)
  and run `wasm/build.sh`. Built with 6.0.13, it is byte-identical to the
  master.wasm Green Room shipped at the import. Changing the Emscripten version
  can change the bytes: do it on purpose, and run Green Room's Node suites with
  the new file.
- StrumTests' whole-plugin checks only build with `ENCORE_STRUM_PLUGIN_TESTS=1`,
  in the plugin repo.

## Versioning and releases

- Semantic versioning, tags `vMAJOR.MINOR.PATCH`, starting at v1.0.0. MAJOR for
  anything that breaks a consumer (headers, classes, functions, the `mc_*` wasm
  exports, or a sound change a product must re-check); MINOR for additions;
  PATCH for fixes. The README has the details.
## Release process

1. The version lives in exactly one place: `project(EncoreAudioProcessingTests VERSION x.y.z)`
   in `tests/CMakeLists.txt`.
2. Changing it is its own small pull request ("Release vX.Y.Z"), so a person sees the
   number before it's built.
3. Merging builds everything on `main`: the tests run and `master.wasm` is uploaded as
   a workflow artifact.
4. A person publishes the release in GitHub (Releases, Draft a new release, a new tag
   `vX.Y.Z` on that `main` commit, Publish). Never a Claude session.
5. The tag's CI run doesn't rebuild. It checks that the tag matches the version, finds
   the successful `main` build of that exact commit and attaches its `master.wasm` and
   `master.wasm.sha256` to the Release. If either check fails, nothing is attached.
   If that commit was docs-only (its build was skipped), run CI by hand on `main`
   (Actions, CI, Run workflow) and then re-run the release job.

- Claude sessions can't push tags: the push is refused with HTTP 403. Don't try, and
  don't look for another way (the API, a workflow, a different remote). When a
  release is due, open the version pull request, then tell your human the version
  number, why that number, and what changed since the last release, and wait for
  them to publish it.
- Tags `v*` are protected: they can't be moved or deleted (an org admin can, in an
  emergency).

## Changes

- main only changes through pull requests. Never push to main directly.
