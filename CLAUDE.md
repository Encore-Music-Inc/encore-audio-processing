# Notes for Claude (and whoever is driving it)

This is ENCORE.'s shared audio processing code: header-only C++20 used by ENCORE.'s
products. By area: building blocks (FFT, windows, filters, delays,
oversampling, resampling, ramps, random numbers, statistics), measurement
(loudness, true peak, the whole-file analysis, spectrum, waveforms), processing
and mastering (EQ, dynamics, reverb, the mastering chain and whole-song
mastering, dither, channel work), pitch (detection, correction, the tuner, voice
tracking, drum tuning), rhythm (tempo, onsets, timing, the metronome), sound
(the drum synthesizer and the clicks) and music vocabulary (note names, keys,
the scale library). It returns numbers; the words people see are the products'.

## What uses it

- ENCORE. VST (encore-vst): the plugins, built with JUCE (`juce_dsp`).
- ENCORE. Basement Studio (encore-basement-studio): the desktop DAW, built with JUCE.
- Green Room, the artist tools in the ENCORE. app (encoremusic.studio): through `master.wasm` (built from
  `wasm/api_*.cpp` and the headers, without JUCE) and the `data/*.json` lists.

## Decisions (already made, don't revisit)

- Licence: GNU AGPLv3 (`LICENSE`). Copyright holder: Encore Music, Inc.
- The repo is public. Outside contributions are not accepted
  (`CONTRIBUTING.md`). The ENCORE. name and logo are not licensed (`TRADEMARKS.md`).
- Contact: support@encoremusic.studio
- Always write ENCORE. (capitals and the period) in anything people read.
- The product name is ENCORE. Audio Processing (release titles, headings). In
  lowercase, "audio processing" is only a plain description.
- No long dashes in any text. Use commas, periods or parentheses.
- Don't write "DSP" in names, titles or prose for people (in music business it
  means Spotify or Apple Music). Say "audio processing". Code folders and
  identifiers (`dsp/`) keep the name.
- Anything third-party goes in `THIRD-PARTY-NOTICES.md`.
- All of ENCORE.'s audio processing lives here. A product never keeps its own
  copy of something this repository has, or a second version of it. When a
  product needs processing that isn't here, it is built here first (with its
  tests and, for the browser, its wasm functions), released, and then used.
- Inside this repository there is one implementation of each thing: one FFT,
  one true-peak detector, one gain curve, one delay line, one ramp, one fade
  module, one dither, one mastering driver, one pitch detector, one metronome
  engine, one drum engine, and so on. A new need adds an option or a preset to
  the existing one, never a second copy.
- Products return words and judgements (feedback text, grades, thresholds);
  this repository returns numbers.
- Docs (README, CLAUDE.md) describe this repository and what it directly uses
  or is used by, never a map of ENCORE.'s other repositories.
- Docs say what the repository does and point to the file that sets a value
  (`ci.yml`, `dependabot.yml`, a CMake file, the Pinned releases table). They
  never repeat a value from config or code, and never give an estimate or a
  measurement (how long, how fast, how often). Release notes are the
  exception: each describes its release on the day it was published.

## Layout

| Path | What |
|---|---|
| `dsp/` | the headers, by area below |
| `dsp/JuceInclude.h` | includes `juce_dsp`, or with `VS_NO_JUCE=1` the stand-in below |
| `dsp/lite/JuceLite.h` | stand-in for the JUCE pieces the headers use, for builds without JUCE. Don't delete it: the wasm build needs it |
| `tools/tunings.py` | the one list of the tuner's instruments, tunings, temperaments and reference pitches; writes `dsp/Tunings.h` and `data/tunings.json` |
| `tools/scales.py` | the one list of the scale library; writes `dsp/ScaleLibrary.h` and `data/scales.json` |
| `tools/reference.py`, `data/reference.json` | the reference set: the cases (inputs made by `dsp/ReferenceSignals.h` with integer arithmetic, so every build makes the same samples), each result's tolerance and its reason; it writes `data/reference.json` from this repository's own results |
| `tests/ReferenceTests.cpp`, `wasm/reference.mjs` | check every result in `data/reference.json`, natively (also built with JUCE as ReferenceJuceTests) and through master.wasm; they fail on any result out of tolerance, missing or unexpected |
| `wasm/api_*.cpp`, `wasm/build.sh` | the browser engine, one file per area with its own prefix (below), and its Emscripten build, which exports every `EMSCRIPTEN_KEEPALIVE` function |
| `tests/` | the offline test suites and their CMake build |
| `external/JUCE` | JUCE, pinned to a release (the README's Pinned releases table): the JUCE version for all of ENCORE. (see below) |
| `CHANGELOG.md` | every release's notes, newest first; the release job copies the version's section onto the GitHub release |
| `.github/release-notes.sh` | prints a version's notes from `CHANGELOG.md`, and fails if they're missing (CI runs it on every build) |
| `.github/pins/check.sh` | CI's check that every pin is a release (the README's Pinned releases table) |
| `.github/sanitizers/` | the scheduled sanitizer run's exceptions (`ubsan.supp`, each with its reason; keep it short) and `report.sh` |
| `.github/workflows/ci.yml` | tests on Linux, Windows and macOS, the `tools/*.py` check and master.wasm on pull requests and main (skipped when only docs change, and for a pull request that only changes the version line and `CHANGELOG.md`); on a schedule: the tests with sanitizers; `v*` tags attach main's master.wasm and `data/*.json` to the Release, no rebuild; "CI passed" is the one check to require |

`tools/*.py`: edit the list in the script and run it (`python3 tools/<name>.py`),
never the files it writes, and commit both. CI fails if they differ from what it
writes. A new list is a new script that writes a header in `dsp/` and a file in
`data/`.

The headers, by area (each header's top comment says more):

| Area | Header | What |
|---|---|---|
| Building blocks | `Fft.h` | the one FFT (complex and real input), on Apple's Accelerate framework on Apple systems |
| | `Windows.h` | window functions and the shared windowed-sinc table |
| | `Biquad.h` | second-order filter coefficients (the RBJ cookbook designs) and their curves |
| | `Crossover.h` | Linkwitz-Riley band splits that recombine flat |
| | `Svf.h` | the state-variable filter (the de-esser's split, and the step the Linkwitz-Riley splits use) |
| | `Convolution.h` | zero-latency convolution, with crossfaded filter changes (Body match) |
| | `DelayLine.h` | the delay line and reading between samples |
| | `FracDelay.h` | fixed fractional delays per channel |
| | `Oversampler.h` | power-of-two oversampling |
| | `Resampler.h` | streaming sample rate conversion (also speed and wow fixes) and integer decimation |
| | `Ramp.h` | a value ramping to a new target, linearly or by equal ratios |
| | `Dynamics.h` | the gain curve, the glide of a cut, and the dynamic EQ bell |
| | `Lpc.h` | linear prediction (gap and click repair, onsets, FLAC) |
| | `Random.h` | seeded random number generators, the same on every system |
| | `Stats.h` | median, percentile, mean, deviation, least-squares line |
| | `Util.h` | smoothers, envelope steps, decibels, mid and side, rounding |
| Measurement | `Loudness.h` | the BS.1770 / EBU R128 loudness meter |
| | `LoudnessReport.h` | a song's loudness report |
| | `TruePeak.h` | the true-peak detector |
| | `Analysis.h` | everything a release check measures on a finished song |
| | `Spectrum.h` | the live analyser, the long-term spectrum, the encoder cliff, tonal balance |
| | `ResonanceFinder.h` | narrow resonances in a long-term spectrum |
| | `Waveform.h` | waveform overviews for drawing |
| Processing and mastering | `EqBand.h` | one band of a parametric EQ |
| | `VocalEq.h` | the vocal EQ (low cut and four bands) |
| | `Compressor.h`, `DeEsser.h`, `Gate.h`, `Rider.h` | the vocal compressor, de-esser, gate and fader rider |
| | `Saturator.h` | preamp drive |
| | `ReverbEngine.h`, `Space.h` | the reverb, and the vocal delay and reverb send |
| | `Doubler.h`, `GuitarFixes.h`, `BodyMatch.h` | Strum's double, its Squeak, Pick and Boom fixes, and its Body match |
| | `MasterChain.h` | the one-knob mastering chain |
| | `Mastering.h` | the one way to master a whole song with the chain |
| | `Dither.h` | the one float to integer conversion, with dither |
| | `Channels.h` | channel maps, trims, fades, peak and normalizing for whole files |
| | `DeviceSim.h` | how a mix sounds on everyday playback devices |
| | `AbCompare.h` | the loudness-matched "hear the original" |
| Pitch | `Yin.h` | the one pitch detector, with its follow, tuner and voice settings |
| | `PitchDetector.h`, `PitchEngine.h` | pitch correction's detector and its real-time engine |
| | `TunerPitch.h` | the tuner's pitch and what it means on the instrument |
| | `Tunings.h` | the tuner's list (written by `tools/tunings.py`) |
| | `VoiceTracker.h` | following a singing voice and measuring it; played notes |
| | `DrumPitch.h` | drum tuning |
| Rhythm | `Tempo.h` | tap tempo and the tempo finder |
| | `Onsets.h` | note onsets in a live stream |
| | `TimingCheck.h` | latency and playing in time with a click |
| | `Metronome.h` | the beat grid, sequencer, click sounds and click engine |
| Sound | `DrumSynth.h` | the drum synthesizer and its kits |
| Music vocabulary | `Music.h` | note names, spelled notes, keys, Hz and MIDI, General MIDI drum names |
| | `ScaleLibrary.h` | the scale library (written by `tools/scales.py`) |
| | `Scales.h` | pitch correction's keys, scales and harmony intervals |

The browser engine, by file and prefix: `api_master.cpp` the mastering chain
(`mc_*`), `api_mastering.cpp` whole-song mastering (`mt_*`), `api_loudness.cpp`
the loudness meter (`lm_*`), `api_measure.cpp` measurement (`ms_*`),
`api_processing.cpp` processing (`pr_*`), `api_pitch.cpp` pitch (`pi_*`),
`api_tuner.cpp` the tuner (`tp_*`), `api_rhythm.cpp` rhythm (`rh_*`),
`api_sound.cpp` sounds (`sd_*`), `api_music.cpp` music vocabulary (`mu_*`). The
lists (scales, tunings) are `data/*.json` files, not functions. A new area is a
new file with its own prefix.

## Consumers

- They add this repo as a submodule at `external/encore-audio-processing` and put
  that folder on their include path, so `#include "dsp/MasterChain.h"` resolves.
  Keep includes in that form: never move or rename headers without a major version.
- `external/JUCE` here is the JUCE version for all of ENCORE.: products that
  use this repo build with it instead of a JUCE of their own, so the JUCE these
  tests run on is the JUCE that ships. Consumers fetch it by its path
  (`git -C external/encore-audio-processing submodule update --init --depth 1 external/JUCE`),
  never with `--recursive`. Move JUCE only to a JUCE release.
- Every header builds without JUCE (`VS_NO_JUCE=1`, through `dsp/JuceInclude.h`);
  only `JuceInclude.h` includes `juce_dsp`. Keep it that way for new headers.
- On Apple systems `dsp/Fft.h` runs on Apple's Accelerate framework, so a product that uses
  the headers there links the Accelerate framework (JUCE's `juce_dsp` already
  does; `tests/CMakeLists.txt` does it for the suites without JUCE).
  `VS_FFT_PORTABLE` forces this repository's own FFT; the browser build always
  uses it.
- They pin the submodule to a release tag. Green Room pins `master.wasm` and the
  `data/*.json` files by their SHA-256.

## Pins

- What this repository uses is pinned in one place, and only to a published
  release. The Pinned releases table in the README names each pin, and CI
  (`.github/pins/check.sh`) checks that each one is a release and is exactly
  what the folder is pinned to. An exception (no usable release exists) is
  written in the table with its reason.
- Fetch pinned folders by their paths, one level at a time
  (`git -C <folder> submodule update --init --depth 1 <path>`), never with
  `--recursive`.
- Move a pin to a new release only when something changes for this
  repository's users, and pick this repository's version number by what its
  users notice, not by the number of what it uses.
- Pull request titles: "Use <repository> vX.Y.Z (was vA.B.C)" for a pull
  request that moves a pin; "Release vX.Y.Z" only for this repository's own
  version.

## Building and testing

- Actions in `ci.yml` are pinned to commits (version in a comment); Dependabot
  proposes updates. Keep new ones pinned the same way.
- Tests: `git submodule update --init --depth 1 external/JUCE`, then
  `cmake -S tests -B build -DCMAKE_BUILD_TYPE=Release`, `cmake --build build --parallel`,
  `ctest --test-dir build --output-on-failure`. All must pass before a pull request.
- master.wasm: source `emsdk_env.sh` for the Emscripten version in `ci.yml`
  and run `wasm/build.sh`. Changing the Emscripten version can change the
  bytes: do it on purpose, and run Green Room's Node suites with the new file.
- StrumTests' whole-plugin checks only build with `ENCORE_STRUM_PLUGIN_TESTS=1`,
  in the plugin repo.
- New processing comes with its tests (a new `tests/<Name>Tests.cpp` without
  JUCE is picked up by itself) and, when the browser needs it, its functions in
  `wasm/api_*.cpp`.
- The reference set is how every product proves it uses these headers: a
  product builds `tests/ReferenceTests.cpp` with its own compiler and flags and
  runs it on `data/reference.json`, and the browser runs `wasm/reference.mjs`
  on the pinned master.wasm and reference.json. When a decided change moves a
  result, run `python3 tools/reference.py` on Linux x64 and commit the new
  `data/reference.json` in the same pull request: that diff is how the change
  shows, and a changed result means a new version (see Versioning). Never edit
  the file by hand; CI fails if it differs from what the script writes.

## Versioning and releases

- Semantic versioning, tags `vMAJOR.MINOR.PATCH`, starting at v1.0.0. MAJOR for
  anything that breaks a consumer (headers, classes, functions, the wasm
  exports in `wasm/api_*.cpp`, fields in `data/*.json`, or a sound or
  measurement change a product must re-check); MINOR for additions;
  PATCH for fixes. The README has the details.

## Release process

1. The version lives in exactly one place: `project(EncoreAudioProcessingTests VERSION x.y.z)`
   in `tests/CMakeLists.txt`.
2. Changing it is its own small pull request ("Release vX.Y.Z"), so a person sees the
   number before it's built. It adds the release notes to `CHANGELOG.md` (see
   Release notes below); CI fails until they're there.
3. Merging builds everything on `main`: the tests run and `master.wasm` and the
   `data/*.json` files are uploaded as a workflow artifact.
4. After the `main` build of that commit has passed, a person publishes the release in
   GitHub. Never a Claude session. Releases, Draft a new release; tag `vX.Y.Z` (Create
   new tag) with target `main` at the "Release vX.Y.Z" commit; leave the title and the
   description empty, don't click Generate release notes and don't attach files (the
   release job fills all of them in); Publish. Never publish an old draft release:
   publishing creates its tag on whatever its target is at that moment.
5. The tag's CI run doesn't rebuild. It checks that the tag matches the version, finds
   the successful `main` build of that exact commit and attaches its `master.wasm`,
   `data/*.json` files and their `.sha256` files to the Release, and sets its title
   (`ENCORE. Audio Processing vX.Y.Z`) and notes (the version's section of
   `CHANGELOG.md`). If either check fails, nothing is attached.
   If that commit was docs-only (its build was skipped), run CI by hand on `main`
   (Actions, CI, Run workflow) and then re-run the release job.

- Claude sessions can't push tags: the push is refused with HTTP 403. Don't try, and
  don't look for another way (the API, a workflow, a different remote). When a
  release is due, open the version pull request, then tell your human the version
  number, why that number, and what changed since the last release, and wait for
  them to publish it.
- Tags `v*` are protected: they can't be moved or deleted (an org admin can, in an
  emergency).

## Release notes

Every release's notes use this format, so they read the same in every ENCORE.
repository:

```
<One or two sentences: what this release is.>

### What's new        (a repository's first release: "What's in it")
### Downloads         (the files attached, and what each is for)
### Using it          (how products pin it)
### Built on          (exactly what the Pinned releases table pinned for this release)

Questions or problems: support@encoremusic.studio
```

- The title is `ENCORE. Audio Processing vX.Y.Z`; the tag stays `vX.Y.Z`.
- The notes describe the release as it is on the day it's published: no links
  to or mentions of later releases, and no promises ("coming soon", "will
  support"). If a limitation matters to people, state the limitation, not a plan.
- They live in `CHANGELOG.md`, newest first, one `## vX.Y.Z` section each,
  written once, in plain language, in the "Release vX.Y.Z" pull request. CI
  (`.github/release-notes.sh`) fails while the version has no section, or the
  section has no Downloads, Built on or Questions line. The release job copies
  the section onto the GitHub release, so nobody pastes notes.
- A published section isn't edited except to fix a factual error. Then fix it
  in a pull request and make the same fix on the GitHub release (Edit), so the
  two stay the same.
- "Built on" names each top-level pin's release, and what it brings in
  brackets when that matters to people (for example the JUCE version).

## Changes

- main only changes through pull requests. Never push to main directly.
