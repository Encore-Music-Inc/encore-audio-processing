# ENCORE. Audio Processing

All the audio processing shared by ENCORE.'s products, as header-only C++20:

- **Building blocks:** the FFT, windows, filters, band splits, delays,
  oversampling, resampling, ramps, gain and mix stages, oscillators,
  envelopes, random numbers and statistics.
- **Measurement:** loudness, the loudness report, true peak, the whole-file
  analysis a release check reads, spectrum, resonances and waveform overviews.
- **Processing and mastering:** EQ, compressor, de-esser, gate, rider, drive,
  reverb and delay, chorus, phaser, time stretch and pitch shift, the
  acoustic guitar tools, the mastering chain and whole-song mastering, dither,
  channel work and fades, playback device simulation, the loudness-matched
  "hear the original", and the presets of these chains.
- **Pitch:** the pitch detector, pitch correction, the tuner and its list of
  instruments and tunings, following a singing voice, and drum tuning.
- **Rhythm:** tap tempo and the tempo finder, note onsets, timing against a
  click, and the metronome.
- **Sound:** the synth, the sampler, the drum synthesizer and the metronome's clicks.
- **Music vocabulary:** note names, keys, Hz and MIDI, the scale library,
  chords (naming notes, numerals, the chords of a key and the moves between
  them, shapes on fretted instruments and the piano), scales (spelling, modes,
  which scale fits a chord or a progression, patterns on a fretted neck), and
  editing notes on a beat grid.

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
tools/chords.py       the one list of chord types; writes dsp/ChordTypes.h and data/chords.json
tools/reference.py    the reference set: cases, tolerances and their reasons; writes data/reference.json
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
| | `Svf.h` | the state-variable filter |
| | `Convolution.h` | zero-latency convolution, with crossfaded filter changes |
| | `DelayLine.h` | a delay line, read between samples |
| | `FracDelay.h` | fixed fractional delays per channel |
| | `Oversampler.h` | power-of-two oversampling |
| | `Resampler.h` | streaming sample rate conversion (also speed and wow fixes), its live form for playback, and decimation |
| | `Ramp.h` | a value ramping to a new target, and how automation moves between its points |
| | `Mix.h` | gain stages, crossfades, the dry/wet send law, pan laws, a hosted plugin's dry and wet levels and a rack return's ramps |
| | `Oscillator.h`, `Envelope.h` | band-limited oscillators, and the ADSR envelope |
| | `Dynamics.h` | the gain curve, the glide of a cut and the dynamic EQ bell that turn a sound down by its level |
| | `Lpc.h` | linear prediction |
| | `Random.h` | seeded random numbers, the same on every system |
| | `Stats.h` | median, percentile, mean, deviation and a least-squares line |
| | `Util.h` | smoothers, decibels, mid and side, rounding |
| | `Denormals.h` | the denormal guard: turns the processor's denormals off while audio code runs |
| Measurement | `Loudness.h` | the loudness meter (ITU-R BS.1770, EBU R128), and the true-peak limiter the mastering chain ends with |
| | `LoudnessReport.h` | a song's loudness report |
| | `TruePeak.h` | the true-peak detector |
| | `Analysis.h` | everything a release check measures on a finished song |
| | `Spectrum.h` | the live analyser, the long-term spectrum, the lossy-encoder cliff, tonal balance and drawing axes |
| | `ResonanceFinder.h` | narrow resonances in a long-term spectrum |
| | `Waveform.h` | waveform overviews for drawing, and a DAW's waveform picture at any zoom |
| | `Meters.h` | how a meter moves on screen (ballistics, peak hold, its scale) and the volume fader's law |
| Processing and mastering | `EqBand.h`, `VocalEq.h` | a parametric EQ band, and the vocal EQ |
| | `Compressor.h`, `DeEsser.h`, `Gate.h`, `Rider.h` | the vocal compressor (with a key input for a sidechain), de-esser, gate and fader rider |
| | `Saturator.h` | preamp drive |
| | `ReverbEngine.h`, `Space.h` | the reverb, and the delay (the vocal send, with its reverb) |
| | `Chorus.h`, `Phaser.h` | the chorus and the phaser |
| | `TimeStretch.h` | time stretching and pitch shifting (on Signalsmith Stretch) |
| | `Doubler.h`, `GuitarFixes.h`, `BodyMatch.h` | the acoustic guitar tools: double, the Squeak, Pick and Boom fixes, and the pickup-to-mic body match |
| | `MasterChain.h` | the one-knob mastering chain |
| | `Mastering.h` | mastering a whole song with the chain |
| | `Dither.h` | float to integer samples, with dither |
| | `Channels.h` | channel maps, trims, fade shapes, peak, RMS, silence, normalizing and the clean-up after a plugin |
| | `DeviceSim.h` | how a mix sounds on everyday playback devices |
| | `Dsd.h` | DSD (the one-bit audio of SACDs, .dsf and .dff files) to PCM |
| | `AbCompare.h` | the loudness-matched "hear the original" |
| | `Presets.h` | the presets of the Vox, Verb, Equalize, Master and Strum chains (written by `tools/presets.py`) |
| Pitch | `Yin.h` | the pitch detector, with its settings for following a voice, the tuner and singing |
| | `PitchDetector.h`, `PitchEngine.h` | pitch correction: its detector and its real-time engine |
| | `TunerPitch.h`, `Tunings.h` | the tuner, and its instruments and tunings |
| | `VoiceTracker.h` | following a singing voice and measuring it, played notes, and the notes of warm-ups, sing-back and the keys that suit a range |
| | `DrumPitch.h` | drum tuning |
| Rhythm | `Tempo.h` | the tempo range, tap tempo and the tempo finder |
| | `Onsets.h` | note onsets in a live stream |
| | `TimingCheck.h` | the speaker-to-mic latency and playing in time with a click |
| | `Metronome.h` | the beat grid, the sequencer, the click sounds, the click engine and its voices for a host's own timeline |
| Sound | `Synth.h` | the synth |
| | `Sampler.h` | the sampler |
| | `DrumSynth.h` | the drum synthesizer and its kits |
| Music vocabulary | `Music.h` | note names, spelled notes, keys, Hz and MIDI, General MIDI drum names |
| | `ScaleLibrary.h` | the scale library |
| | `ChordTypes.h` | the chord types: formulas, symbols, numerals, families, the picker's groups |
| | `Harmony.h` | chords: degrees, symbols, naming a set of notes, numerals, the map of a key, key changes, the moves between chords and the facts behind them, transposing, guessing a song's key from its chords, a capo for a shift |
| | `Voicings.h` | chord shapes on fretted instruments (courses, fingerings, the shape search, capo choices) and the piano |
| | `ScaleTheory.h` | scales on a root: spelling, steps, comparing, modes and parents, the chords they build, which scale fits a chord or a progression, the neck (box positions, notes per string) |
| | `TheoryCalls.h` | the theory and the voice exercises as JSON requests and answers, for the browser and the reference set |
| | `Scales.h` | pitch correction's keys, scales and harmony intervals |
| | `NoteEdit.h` | editing notes on a beat grid: grid sizes, quantize, transpose, velocities, step grids |

### The browser engine

| File | Prefix | What |
|---|---|---|
| `wasm/api_master.cpp` | `mc_` | the mastering chain |
| `wasm/api_mastering.cpp` | `mt_` | whole-song mastering |
| `wasm/api_loudness.cpp` | `lm_` | the loudness meter |
| `wasm/api_measure.cpp` | `ms_` | true peak, the loudness report, the analysis, spectrum, waveforms, meters on screen, the band that tames a resonance |
| `wasm/api_processing.cpp` | `pr_` | resampling, delays, dither, channel work, device simulation, the vocal EQ, "hear the original", DSD to PCM, the true-peak limiter on its own, an MP3's sample rate |
| `wasm/api_effects.cpp` | `fx_` | the chorus, phaser, filter, pitch shifter, time stretch, compressor, delay and reverb, and the send law |
| `wasm/api_instruments.cpp` | `in_` | the synth and the sampler |
| `wasm/api_pitch.cpp` | `pi_` | voice pitch and its measurements, played notes, drum tuning |
| `wasm/api_tuner.cpp` | `tp_` | the tuner, the block averaging before a reading, and following its readings |
| `wasm/api_rhythm.cpp` | `rh_` | tempo, onsets, timing, the beat grid's meters (groupings, accents, note values, subdivisions, tempo names) |
| `wasm/api_sound.cpp` | `sd_` | the drums and the metronome (its sounds, its click engine and mending its settings) |
| `wasm/api_music.cpp` | `mu_` | note names, spelled notes and the sounds they make, keys and the key lists, Hz and MIDI, note editing on a grid |
| `wasm/api_theory.cpp` | `th_` | the chord and scale theory: one function that answers a JSON request (`dsp/TheoryCalls.h` lists them) |

The lists (tunings, scales, presets, chord types) are the `data/*.json` files, not functions.

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
Every header builds this way, so the browser build can use any of them.

On macOS (and other Apple systems), link the Accelerate framework:
`dsp/Fft.h` runs its transforms on Apple's Accelerate framework there. JUCE products
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

### The reference set

`data/reference.json` holds inputs (made from recipes by
`dsp/ReferenceSignals.h`, the same samples on every system) and the results
this repository gives for them, each with its tolerance and the reason for it.
It covers every area: loudness, true peak, pitch, rhythm, mastering, every
sound, file analysis, spectrum, waveforms, processing, the music vocabulary,
and every processor. A product proves it uses these headers by running it:

```bash
# a C++ product: build tests/ReferenceTests.cpp with the product's compiler and flags, then
ReferenceTests external/encore-audio-processing/data/reference.json
# the browser engine
node external/encore-audio-processing/wasm/reference.mjs master.wasm reference.json
```

Both exit non-zero if any result is out of tolerance, missing or unexpected.
The release attaches `reference.json` with its `.sha256`, like the other lists.

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
