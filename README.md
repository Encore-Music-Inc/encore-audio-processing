# ENCORE. audio processing

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
| ENCORE. Green Room (the browser tools) | loads `master.wasm`, built from `wasm/master_api.cpp` and the headers without JUCE |

## What is here

```
dsp/                  the headers (MasterChain.h, EqBand.h, ReverbEngine.h, ...)
dsp/lite/JuceLite.h   a small stand-in for the JUCE pieces the headers use, for builds without JUCE
wasm/master_api.cpp   the browser engine: plain C functions around the mastering chain
wasm/build.sh         builds master.wasm with Emscripten
tests/                offline test suites and their CMake build
external/JUCE         JUCE 9 (submodule, used by the tests)
```

## Using it in a product

Add this repo as a submodule at `external/encore-audio-processing` and put
that folder (the repo root) on the include path:

```cmake
target_include_directories(MyTarget PRIVATE ${CMAKE_SOURCE_DIR}/external/encore-audio-processing)
```

Includes then stay as they are today: `#include "dsp/MasterChain.h"`.
Pin the submodule to a release tag.

With JUCE (desktop), link `juce::juce_dsp`; `dsp/JuceInclude.h` includes it.
Without JUCE, define `VS_NO_JUCE=1` and `dsp/lite/JuceLite.h` is used instead
(only the headers the mastering chain needs work this way).

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
v1.0.0. A tag builds and tests everything and attaches `master.wasm` and
`master.wasm.sha256` to a GitHub Release.

- MAJOR: a change that breaks a consumer: a renamed or removed header, class
  or function, a changed `mc_*` export in `master.wasm`, or a change in the
  sound that a product must re-check (for example, a mastering style that
  measures differently).
- MINOR: new processing or options that leave existing behaviour alone.
- PATCH: fixes that keep the API and the intended sound.

Products pin the submodule to a tag, and Green Room pins the matching
`master.wasm` by its SHA-256.
