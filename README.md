# ENCORE. Audio Processing

The audio processing code shared by ENCORE.'s products: the mastering chain,
EQ, compressor, de-esser, gate, rider, reverb, pitch detection and correction,
and the acoustic guitar fixes. It is header-only C++20.

Copyright (c) Encore Music, Inc. Licensed under the GNU Affero General Public
License v3 (see `LICENSE`). The ENCORE. name and logo are not licensed (see
`TRADEMARKS.md`). Outside contributions are not accepted (see
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

Includes then take the form `#include "dsp/MasterChain.h"`.
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

## Building and running the tests

Needs CMake (the minimum is in `tests/CMakeLists.txt`) and a C++20 compiler. On Linux:

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

`.github/workflows/ci.yml` runs the test suites on Linux x64, Windows x64 (MSVC)
and macOS (Apple Silicon), in parallel, and builds `master.wasm`, on every pull
request and every push to `main`. The products build these headers with all
three compilers, so the suites run on all three. On the schedule in `ci.yml`
(or a manual run with `checks: weekly`) the suites are also built with AddressSanitizer and
UndefinedBehaviorSanitizer on Linux, to catch memory errors and undefined
behaviour. Undefined behaviour is reported and the suites carry on, so one run
lists every finding; a memory error stops it. Exceptions, each with its
reason, go in `.github/sanitizers/ubsan.supp`. Builds use Ninja and a compiler cache (ccache, sccache on Windows).
Every action is pinned to a commit, and Dependabot proposes updates (`.github/dependabot.yml`).

## Building master.wasm

Install the Emscripten version `.github/workflows/ci.yml` uses and run:

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
in its own pull request, with the release notes in `CHANGELOG.md`. Every merge
to `main` builds and tests `master.wasm`. Once that build has passed, a person
at Encore Music, Inc. publishes the release in GitHub (the steps are in
`CLAUDE.md`). The release isn't rebuilt: CI checks that the tag matches the
version, attaches the `master.wasm` and `master.wasm.sha256` that `main` built
from the same commit, and sets the title and notes from `CHANGELOG.md`.

- MAJOR: a change that breaks a consumer: a renamed or removed header, class
  or function, a changed `mc_*` export in `master.wasm`, or a change in the
  sound that a product must re-check (for example, a mastering style that
  measures differently).
- MINOR: new processing or options that leave existing behaviour alone.
- PATCH: fixes that keep the API and the intended sound.

Products pin the submodule to a tag, and Green Room pins the matching
`master.wasm` by its SHA-256.
