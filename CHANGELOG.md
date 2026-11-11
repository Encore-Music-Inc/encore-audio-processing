# Changes

Each release's notes, newest first. When a release is published, the release
job puts its section on the GitHub release (see "Release notes" in
`CLAUDE.md`).

## v2.1.0

Adds the browser functions the ENCORE. app still kept its own copies of, DSD
to PCM, the true-peak limiter on its own and a much faster resampler, and
fixes two defects. Nothing that worked before changes, except the two fixes.

### What's new
- Browser functions for the metronome's beat grid (groupings, accents, note
  values, subdivisions, tempo names, mending settings), the tuner's block
  averaging, spelled notes, and the major and minor key lists.
- DSD to PCM (`dsp/Dsd.h`): one-bit SACD audio, from .dsf and .dff files, to
  PCM, in the browser too.
- The mastering chain's true-peak limiter on its own, for previews and stream
  files.
- The file analysis reports each channel's DC offset in dBFS.
- The resampler is about ten times faster. Whole-number rate ratios (96 to
  48 kHz, 88.2 to 44.1, 192 to 48) give exactly the same samples as before.
  Other ratios (44.1 to 48 and the like) now keep exact time over long files,
  where they drifted before, and end at exactly the expected length. A new
  constructor takes the two sample rates.

### What's fixed
- The device previews (phone, laptop, earbuds) no longer add a bump where
  their low and high cuts begin (it reached +1.75 dB near 500 Hz on the
  phone).
- The tempo finder reports no tempo for silence (it reported 60 BPM).

### Downloads
- `master.wasm`: the browser engine, with `master.wasm.sha256`.
- `tunings.json`, `scales.json`: the tuner's instruments and tunings and the
  scale library for the browser, each with its `.sha256`.
- `reference.json`: the reference set, with `reference.json.sha256`.

### Using it
Pin `external/encore-audio-processing` to `v2.1.0`, then fetch its JUCE by
path:

```
git -C external/encore-audio-processing submodule update --init --depth 1 external/JUCE
```

On Apple systems, link the Accelerate framework (JUCE products already do).
Green Room pins `master.wasm`, `tunings.json`, `scales.json` and
`reference.json` by their SHA-256 and runs `wasm/reference.mjs` on them.

### Built on
JUCE 9.0.5. `master.wasm` is built without JUCE, with Emscripten 6.0.13.

Questions or problems: support@encoremusic.studio

## v2.0.0

All of ENCORE.'s audio processing, in one place, for the desktop and the
browser. Everything ENCORE. VST, ENCORE. Basement Studio and Green Room kept
their own copies of is here once, and every header builds without JUCE, so the
browser engine can use any of it. Some sounds and measurements change on
purpose (below), so products re-check them when they move to this release.

### What's new
- Building blocks: one FFT (on Apple's Accelerate framework on Apple systems),
  windows, random generators, statistics, linear prediction, filters,
  oversampling, convolution, a delay line and a ramp.
- Measurement: one true-peak detector (the limiter's), the loudness report,
  the file analysis behind Last Call, the spectrum analyser, the long-term
  spectrum, waveform overviews and one clip rule.
- Processing and mastering: resampling, dither, fades, channel handling, the
  playback device filters, Vox's EQ, one A/B, and one whole-song mastering
  driver that renders until the master is within 0.1 LU of its target.
- Pitch and rhythm: one pitch detector behind the pitch follower, the tuner
  and the voice exercises; the voice tracker; drum tuning; tap tempo, the
  tempo finder, onsets and the timing check.
- Sound: one metronome engine with every click set (now with Clave and Soft
  tick) and one drum engine with every kit (ENCORE., Boom, Club, Studio, 808,
  Rock and Lo-fi).
- Music: note names, keys, the GM drum map and the scale library
  (`data/scales.json`).
- The reference set (`data/reference.json`): inputs every system makes the
  same way, with this release's results and their tolerances. A product runs
  it to prove it uses this release.
- The browser engine has one set of functions per area (`mc_`, `mt_`, `lm_`,
  `ms_`, `pr_`, `pi_`, `tp_`, `rh_`, `sd_`, `mu_`, `rf_`).

### What changes how things sound or measure
- The mastering limiter's true-peak detector is longer, the same on the
  desktop and in the browser. Browser masters no longer go over the -1 dBTP
  ceiling (they reached -0.74 dBTP).
- One mastering driver: it learns from the loudest 30 seconds and keeps
  rendering until the master is within 0.1 LU of the target, or the drive is
  at its limit.
- One true-peak meter: readings that used another detector move by up to
  1.3 dB, closer to the true value. Mono files are measured as one channel.
- One dither (triangular) for 16 and 24-bit files.
- The Space reverb is a setting of the one reverb, and its 8 kHz low-pass now
  works at 44.1 and 48 kHz.
- Basement Studio's drum kits and its Clave and Soft tick clicks are rebuilt on
  the shared engines. "Louder click on beat 1" plays the other beats at 0.4 of
  beat one.
- The Hi-hat and Kick and hat click sets no longer break at sample rates under
  24 kHz.

### Downloads
- `master.wasm`: the browser engine, with `master.wasm.sha256`.
- `tunings.json`, `scales.json`: the tuner's instruments and tunings and the
  scale library for the browser, each with its `.sha256`.
- `reference.json`: the reference set, with `reference.json.sha256`.

### Using it
Pin `external/encore-audio-processing` to `v2.0.0`, then fetch its JUCE by
path:

```
git -C external/encore-audio-processing submodule update --init --depth 1 external/JUCE
```

On Apple systems, link the Accelerate framework (JUCE products already do).
Build `tests/ReferenceTests.cpp` and run it on `data/reference.json` to check
the product gets this release's results. Green Room pins `master.wasm`,
`tunings.json`, `scales.json` and `reference.json` by their SHA-256 and runs
`wasm/reference.mjs` on them.

### Built on
JUCE 9.0.5. `master.wasm` is built without JUCE, with Emscripten 6.0.13.

Questions or problems: support@encoremusic.studio

## v1.0.1

No change to how anything sounds. JUCE moves to a release, and this
repository's JUCE becomes the JUCE version for all of ENCORE.

### What's new
- JUCE moves to its 9.0.5 release (from a commit between 9.0.4 and 9.0.5). The
  part the audio processing uses (`juce_dsp`) is unchanged; the rest is JUCE's
  security and crash fixes.
- This repository's JUCE is the JUCE version for all of ENCORE.: products build
  with it instead of a JUCE of their own, so what is tested here is what ships.
- The tests run on Linux, Windows and macOS on every pull request, with weekly
  checks for memory errors and undefined behaviour.
- CI checks that every pin is a published release.

### Downloads
- `master.wasm`, byte-identical to v1.0.0
  (SHA-256 `108ba14b5f587dee2f412a146186c83eba2ca527f194bc0493b5c4ec78f845e0`),
  with `master.wasm.sha256`.

### Using it
Pin `external/encore-audio-processing` to `v1.0.1`, then fetch its JUCE by path:

```
git -C external/encore-audio-processing submodule update --init --depth 1 external/JUCE
```

Green Room: nothing to change (same `master.wasm`).

### Built on
JUCE 9.0.5. `master.wasm` is built without JUCE, with Emscripten 6.0.13.

Questions or problems: support@encoremusic.studio

## v1.0.0

The first release of ENCORE. Audio Processing: the sound engine shared by
ENCORE. VST, ENCORE. Basement Studio and Green Room (the artist tools in the
ENCORE. app).

### What's in it
- The mastering chain, EQ, compressor, de-esser, gate, rider, reverb, pitch
  detection and correction, and the acoustic guitar fixes, as header-only C++20.
- `master.wasm`: the browser build of the mastering chain, which Green Room uses.
- Six test suites.

### Downloads
- `master.wasm` (SHA-256 `108ba14b5f587dee2f412a146186c83eba2ca527f194bc0493b5c4ec78f845e0`),
  with `master.wasm.sha256`.

### Using it
Pin `external/encore-audio-processing` to `v1.0.0` and put that folder on the
include path, so `#include "dsp/MasterChain.h"` resolves. Green Room pins
`master.wasm` by its SHA-256.

### Built on
JUCE 9 at commit `7ff644f` (between the 9.0.4 and 9.0.5 releases), for the
tests. `master.wasm` is built without JUCE, with Emscripten 6.0.13.

Questions or problems: support@encoremusic.studio
