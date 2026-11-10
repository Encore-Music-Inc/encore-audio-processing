# ENCORE. Audio Processing

All the audio processing shared by ENCORE.'s products, as header-only C++20:

- **Building blocks:** the FFT, windows, filters, band splits, delays,
  oversampling, resampling, ramps, random numbers and statistics.
- **Measurement:** loudness, the loudness report, true peak, the whole-file
  analysis a release check reads, spectrum, resonances and waveform overviews.
- **Processing and mastering:** EQ, compressor, de-esser, gate, rider, drive,
  reverb and delay, the acoustic guitar tools, the mastering chain and
  whole-song mastering, dither, channel work and fades, playback device
  simulation and the loudness-matched "hear the original".
- **Pitch:** the pitch detector, pitch correction, the tuner and its list of
  instruments and tunings, following a singing voice, and drum tuning.
- **Rhythm:** tap tempo and the tempo finder, note onsets, timing against a
  click, and the metronome.
- **Sound:** the drum synthesizer and the metronome's clicks.
- **Music vocabulary:** note names, keys, Hz and MIDI, and the scale library.

Each thing has one implementation here, and products use it rather than keep
their own copy: processing a product needs is added here first, released, then
used. It returns numbers; the words and judgements people see are the
product's.

Copyright (c) Encore Music, Inc. Licensed under the GNU Affero General Public
License v3 (see `LICENSE`). The ENCORE. name and logo are not licensed (see
`TRADEMARKS.md`). Outside contributions are not accepted (see
`CONTRIBUTING.md`). Contact: support@encoremusic.studio

## Who uses it

| Product | How |
|---|---|
| ENCORE. VST (the plugins) | builds the headers with JUCE (`juce_dsp`) |
| ENCORE. Basement Studio (the desktop DAW) | builds the headers with JUCE, with the plugins built in |
| Green Room, the artist tools in the ENCORE. app (browser) | loads `master.wasm`, built from `wasm/api_*.cpp` and the headers without JUCE, and the `data/*.json` lists |

## What is here

```
dsp/                  the headers (below)
dsp/JuceInclude.h     includes juce_dsp, or without JUCE the stand-in below
dsp/lite/JuceLite.h   a small stand-in for the JUCE pieces the headers use, for builds without JUCE
tools/tunings.py      the one list of the tuner's instruments, tunings, temperaments and
                      reference pitches; writes dsp/Tunings.h and data/tunings.json
tools/scales.py       the one list of the scale library; writes dsp/ScaleLibrary.h and data/scales.json
data/*.json           those lists for the browser
wasm/api_*.cpp        the browser engine: plain C functions, one file per area (below)
wasm/build.sh         builds master.wasm with Emscripten, exporting every EMSCRIPTEN_KEEPALIVE function
tests/                offline test suites and their CMake build
external/JUCE         JUCE (submodule): the JUCE version for all of ENCORE.
```

To change a list (an instrument, a tuning, a scale), edit the script in
`tools/`, run it (`python3 tools/<name>.py`), and commit both files it writes.
CI fails if either differs from what the script writes.

### The headers

| Area | Header | What |
|---|---|---|
| Building blocks | `Fft.h` | the FFT, complex and real input |
| | `Windows.h` | window functions and the windowed-sinc table |
| | `Biquad.h` | second-order filter designs (the RBJ cookbook) and their curves |
| | `Crossover.h` | Linkwitz-Riley band splits that recombine flat |
| | `DelayLine.h` | a delay line, read between samples |
| | `FracDelay.h` | fixed fractional delays per channel |
| | `Oversampler.h` | power-of-two oversampling |
| | `Resampler.h` | streaming sample rate conversion (also speed and wow fixes), and decimation |
| | `Ramp.h` | a value ramping to a new target |
| | `Dynamics.h` | the gain curve, the glide of a cut and the dynamic EQ bell that turn a sound down by its level |
| | `Lpc.h` | linear prediction |
| | `Random.h` | seeded random numbers, the same on every system |
| | `Stats.h` | median, percentile, mean, deviation and a least-squares line |
| | `Util.h` | smoothers, decibels, mid and side, rounding |
| Measurement | `Loudness.h` | the loudness meter (ITU-R BS.1770, EBU R128) |
| | `LoudnessReport.h` | a song's loudness report |
| | `TruePeak.h` | the true-peak detector |
| | `Analysis.h` | everything a release check measures on a finished song |
| | `Spectrum.h` | the live analyser, the long-term spectrum, the lossy-encoder cliff and tonal balance |
| | `ResonanceFinder.h` | narrow resonances in a long-term spectrum |
| | `Waveform.h` | waveform overviews for drawing |
| Processing and mastering | `EqBand.h`, `VocalEq.h` | a parametric EQ band, and the vocal EQ |
| | `Compressor.h`, `DeEsser.h`, `Gate.h`, `Rider.h` | the vocal compressor, de-esser, gate and fader rider |
| | `Saturator.h` | preamp drive |
| | `ReverbEngine.h`, `Space.h` | the reverb, and the vocal delay with its reverb |
| | `Doubler.h`, `GuitarFixes.h`, `BodyMatch.h` | the acoustic guitar tools: double, the Squeak, Pick and Boom fixes, and the pickup-to-mic body match |
| | `MasterChain.h` | the one-knob mastering chain |
| | `Mastering.h` | mastering a whole song with the chain |
| | `Dither.h` | float to integer samples, with dither |
| | `Channels.h` | channel maps, trims, fades, peak and normalizing for whole files |
| | `DeviceSim.h` | how a mix sounds on everyday playback devices |
| | `AbCompare.h` | the loudness-matched "hear the original" |
| Pitch | `Yin.h` | the pitch detector, with its settings for following a voice, the tuner and singing |
| | `PitchDetector.h`, `PitchEngine.h` | pitch correction: its detector and its real-time engine |
| | `TunerPitch.h`, `Tunings.h` | the tuner, and its instruments and tunings |
| | `VoiceTracker.h` | following a singing voice and measuring it, and played notes |
| | `DrumPitch.h` | drum tuning |
| Rhythm | `Tempo.h` | tap tempo and the tempo finder |
| | `Onsets.h` | note onsets in a live stream |
| | `TimingCheck.h` | the speaker-to-mic latency and playing in time with a click |
| | `Metronome.h` | the beat grid, the sequencer, the click sounds and the click engine |
| Sound | `DrumSynth.h` | the drum synthesizer and its kits |
| Music vocabulary | `Music.h` | note names, spelled notes, keys, Hz and MIDI, General MIDI drum names |
| | `ScaleLibrary.h` | the scale library |
| | `Scales.h` | pitch correction's keys, scales and harmony intervals |

### The browser engine

| File | Prefix | What |
|---|---|---|
| `wasm/api_master.cpp` | `mc_` | the mastering chain |
| `wasm/api_mastering.cpp` | `mt_` | whole-song mastering |
| `wasm/api_loudness.cpp` | `lm_` | the loudness meter |
| `wasm/api_measure.cpp` | `ms_` | true peak, the loudness report, the analysis, spectrum, waveforms |
| `wasm/api_processing.cpp` | `pr_` | resampling, delays, dither, channel work, device simulation, the vocal EQ, "hear the original" |
| `wasm/api_pitch.cpp` | `pi_` | voice pitch and its measurements, played notes, drum tuning |
| `wasm/api_tuner.cpp` | `tp_` | the tuner |
| `wasm/api_rhythm.cpp` | `rh_` | tempo, onsets, timing |
| `wasm/api_sound.cpp` | `sd_` | the drums and the metronome |
| `wasm/api_music.cpp` | `mu_` | note names, keys, Hz and MIDI |

The lists (tunings, scales) are the `data/*.json` files, not functions.

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
Without JUCE, define `VS_NO_JUCE=1` and `dsp/lite/JuceLite.h` is used instead.
Every header builds this way except `DeEsser.h`, `PitchEngine.h` and
`BodyMatch.h`, which need JUCE.

On macOS (and other Apple systems), link the Accelerate framework:
`dsp/Fft.h` runs its transforms on Accelerate's vDSP there. JUCE products
already link it through `juce_dsp`. Define `VS_FFT_PORTABLE` to use this
repository's own FFT instead (the browser build always does).

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
failure. The suites that need JUCE are named in `tests/CMakeLists.txt`; every other
`tests/*Tests.cpp` builds without JUCE, the way the browser build does, and a new one is
picked up automatically. To build only those (no JUCE needed), configure with
`-DENCORE_JUCE_TESTS=OFF`.

`StrumTests` also has whole-plugin checks that need the Strum plugin's own
sources; they build only with `ENCORE_STRUM_PLUGIN_TESTS=1`, in the plugin repo.

Reference test set: (added with the set)

### CI

`.github/workflows/ci.yml` runs the test suites on Linux x64, Windows x64 (MSVC)
and macOS (Apple Silicon), in parallel, checks that the files `tools/*.py`
write are up to date, and builds `master.wasm`, on every pull request and every
push to `main`. The products build these headers with all
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
against a local build. A release's `.json` files are the `data/*.json` files
from the same commit.

## Versioning

Releases are tagged `vMAJOR.MINOR.PATCH` (semantic versioning), starting at
v1.0.0. The version is set in one place, `tests/CMakeLists.txt`, and changed
in its own pull request, with the release notes in `CHANGELOG.md`. Every merge
to `main` builds and tests `master.wasm`. Once that build has passed, a person
at Encore Music, Inc. publishes the release in GitHub (the steps are in
`CLAUDE.md`). The release isn't rebuilt: CI checks that the tag matches the
version, attaches the `master.wasm`, `data/*.json` files and their `.sha256`
files that `main` built from the same commit, and sets the title and notes from
`CHANGELOG.md`.

- MAJOR: a change that breaks a consumer: a renamed or removed header, class
  or function, a changed or removed function in `master.wasm` (any of the
  `EMSCRIPTEN_KEEPALIVE` functions in `wasm/api_*.cpp`), a renamed or removed
  field in a `data/*.json` file, or a change in the sound or the
  measurements that a product must re-check (for example, a mastering style
  that measures differently).
- MINOR: new processing or options that leave existing behaviour alone.
- PATCH: fixes that keep the API and the intended sound.

Products pin the submodule to a tag, and Green Room pins the matching
`master.wasm` and `data/*.json` files by their SHA-256.
