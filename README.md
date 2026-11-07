# ENCORE. Audio Processing

The audio processing code shared by ENCORE.'s products: the mastering chain,
EQ, compressor, de-esser, gate, rider, reverb, pitch detection and correction,
and the acoustic guitar fixes. It is header-only C++20.

Copyright (c) Encore Music, Inc. Licensed under the GNU Affero General Public
License v3 (see `LICENSE`). The ENCORE. name and logo are not licensed (see
`TRADEMARKS.md`). Outside contributions are not accepted yet (see
`CONTRIBUTING.md`). Contact: support@encoremusic.studio

## Who uses it

| Product | How |
|---|---|
| ENCORE. VST (the plugins) | builds the headers with JUCE (`juce_dsp`) |
| ENCORE. Basement Studio (the desktop DAW) | builds the headers with JUCE, with the plugins built in |
| Green Room, the artist tools in the ENCORE. app (browser) | loads `master.wasm`, built from `wasm/master_api.cpp` and the headers without JUCE |

## What is here

```
dsp/                  the headers (MasterChain.h, EqBand.h, ReverbEngine.h, ...)
dsp/lite/JuceLite.h   a small stand-in for the JUCE pieces the headers use, for builds without JUCE
wasm/master_api.cpp   the browser engine: plain C functions around the mastering chain
wasm/build.sh         builds master.wasm with Emscripten
tests/                offline test suites and their CMake build
external/JUCE         JUCE (submodule): the JUCE version for all of ENCORE.
```

## Using it in a product

Add this repo as a submodule at `external/encore-audio-processing` and put
that folder (the repo root) on the include path:

```cmake
target_include_directories(MyTarget PRIVATE ${CMAKE_SOURCE_DIR}/external/encore-audio-processing)
```

Includes then stay as they are today: `#include "dsp/MasterChain.h"`.
Pin the submodule to a release tag. This repo's `external/JUCE` is the JUCE
version for all of ENCORE.: a product builds with it instead of a JUCE of its
own, so the JUCE these tests run on is the JUCE that ships. Fetch it by its
path, not recursively:

```bash
git -C external/encore-audio-processing submodule update --init --depth 1 external/JUCE
```

With JUCE (desktop), link `juce::juce_dsp`; `dsp/JuceInclude.h` includes it.
Without JUCE, define `VS_NO_JUCE=1` and `dsp/lite/JuceLite.h` is used instead
(only the headers the mastering chain needs work this way).

## Pinned releases

What this repository uses, each pinned to a published release. CI checks that
each row is a release and is exactly what the folder is pinned to
(`.github/pins/check.sh`).

<!-- pinned-releases:start -->
| Folder | Repository | Release |
|---|---|---|
| `external/JUCE` | [JUCE](https://github.com/juce-framework/JUCE) | `9.0.5` |
<!-- pinned-releases:end -->

## How ENCORE.'s repositories fit together

```
encore-audio-processing ──▶ JUCE                        (the JUCE version for all of ENCORE.)
encore-vst              ──▶ encore-audio-processing     (brings JUCE with it)
encore-basement-studio  ──▶ encore-vst                  (brings the sound engine and JUCE with it)
                        ──▶ Tracktion Engine
encore-os               ──▶ encore-vst and encore-basement-studio releases
Green Room (encoremusic)──▶ master.wasm from encore-audio-processing, by its SHA-256
```

- Each repository pins each thing it uses in one place, and only to a
  published release. The Pinned releases table in the README names them, and
  CI (`.github/pins/check.sh`) checks that each one is a release and is exactly
  what the folder is pinned to. An exception (no usable release exists) is
  written in the table with its reason.
- Nothing is pinned twice. A repository that uses encore-vst uses encore-vst's
  sound engine and JUCE, never its own copy; one that uses
  encore-audio-processing uses its JUCE.
- Fetch pinned folders by their paths, one level at a time
  (`git -C <folder> submodule update --init --depth 1 <path>`), never with
  `--recursive`: some pinned repositories have test-only folders of their own.
- Changes travel up one repository at a time, each through a release:
  encore-audio-processing, then encore-vst, then encore-basement-studio, then
  encore-os (Green Room beside them). A repository moves to a new release only
  when something changes for its own users, and picks its own version number
  by what its users notice, not by the number of what it uses.
- Pull request titles: "Use <repository> vX.Y.Z (was vA.B.C)" in the repository
  that moves a pin; "Release vX.Y.Z" only for the repository's own version.
- After this repository publishes a release: if something the plugins use changed, encore-vst opens "Use encore-audio-processing vX.Y.Z". If `master.wasm` changed (its SHA-256 differs from the last release), Green Room takes the new file. A JUCE change here moves JUCE for encore-vst and encore-basement-studio too, through the same chain.

## Building and running the tests

Needs CMake 3.22 or newer and a C++20 compiler. On Linux:

```bash
git submodule update --init --depth 1 external/JUCE
cmake -S tests -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Each suite prints a PASS or FAIL line per check and exits non-zero on any
failure. `LiteMasterTests` builds the mastering chain without JUCE, the way
the browser build does. To build only that one (no JUCE needed), configure
with `-DENCORE_JUCE_TESTS=OFF`.

`StrumTests` also has whole-plugin checks that need the Strum plugin's own
sources; they build only with `ENCORE_STRUM_PLUGIN_TESTS=1`, in the plugin repo.

### CI

`.github/workflows/ci.yml` runs the six suites on Linux x64, Windows x64 (MSVC)
and macOS (Apple Silicon), in parallel, and builds `master.wasm`, on every pull
request and every push to `main`. The products build these headers with all
three compilers, so the suites run on all three. Every Monday (or a manual run
with `checks: weekly`) the suites are also built with AddressSanitizer and
UndefinedBehaviorSanitizer on Linux, to catch memory errors and undefined
behaviour. Undefined behaviour is reported and the suites carry on, so one run
lists every finding; a memory error stops it. Exceptions, each with its
reason, go in `.github/sanitizers/ubsan.supp` (none today). Builds use Ninja and a compiler cache (ccache, sccache on Windows).
Every action is pinned to a commit, and Dependabot proposes updates monthly.

## Building master.wasm

Install Emscripten (CI uses the version in `.github/workflows/ci.yml`,
currently 6.0.13) and run:

```bash
source /path/to/emsdk/emsdk_env.sh
wasm/build.sh
```

This writes `wasm/master.wasm` (not committed). The same Emscripten version
gives the same bytes, so a release's `master.wasm.sha256` can be checked
against a local build.

## Versioning

Releases are tagged `vMAJOR.MINOR.PATCH` (semantic versioning), starting at
v1.0.0. The version is set in one place, `tests/CMakeLists.txt`, and changed
in its own pull request. Every merge to `main` builds and tests `master.wasm`.
A person at Encore Music, Inc. then creates the release in GitHub (Releases,
Draft a new release, a new tag on that `main` commit, titled
"ENCORE. Audio Processing vX.Y.Z", with the notes from the release pull
request, Publish). The release
isn't rebuilt: CI checks that the tag matches the version and attaches the
`master.wasm` and `master.wasm.sha256` that `main` built from the same commit.

- MAJOR: a change that breaks a consumer: a renamed or removed header, class
  or function, a changed `mc_*` export in `master.wasm`, or a change in the
  sound that a product must re-check (for example, a mastering style that
  measures differently).
- MINOR: new processing or options that leave existing behaviour alone.
- PATCH: fixes that keep the API and the intended sound.

Products pin the submodule to a tag, and Green Room pins the matching
`master.wasm` by its SHA-256.
