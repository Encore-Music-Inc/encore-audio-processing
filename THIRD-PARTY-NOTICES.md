# Third-party notices

This repository's own code is Copyright (c) Encore Music, Inc. and licensed
under the GNU Affero General Public License v3 (see `LICENSE`).

## JUCE

- What: the JUCE framework, release 9.0.5 (commit `d60b7a78030661574a615b586d9325ba7a7fa7dd`)
- Where: git submodule at `external/JUCE` (https://github.com/juce-framework/JUCE)
- Used by: the tests in `tests/` and the desktop build of the headers in `dsp/`
  (through `juce_dsp`). The WebAssembly build (`master.wasm`) does not use JUCE:
  `dsp/lite/JuceLite.h` is ENCORE.'s own stand-in, written from scratch.
- Licence: dual licensed under the GNU AGPLv3 and the commercial JUCE 9 licence.
  This repository uses JUCE under the AGPLv3. The full terms are in
  `external/JUCE/LICENSE.md` once the submodule is checked out.

Products that build these headers with JUCE take on JUCE's licence terms
themselves, under whichever of the two licences they choose.

## Signalsmith Stretch and Signalsmith Linear

- What: Signalsmith Stretch, release 1.4.0, and the parts of Signalsmith
  Linear it uses (`stft.h`, `fft.h`), release 0.6.4: the time stretching and
  pitch shifting in `dsp/TimeStretch.h`
- Where: vendored, unchanged, in `dsp/third_party/signalsmith`
  (https://github.com/Signalsmith-Audio/signalsmith-stretch,
  https://github.com/Signalsmith-Audio/linear). `dsp/TimeStretch.h` replaces
  their complex FFT with this repository's (`dsp/Fft.h`).
- Licence: MIT, Copyright (c) 2022 Geraint Luff / Signalsmith Audio Ltd. The
  full terms are in `dsp/third_party/signalsmith/LICENSE-stretch.txt` and
  `dsp/third_party/signalsmith/signalsmith-linear/LICENSE.txt`.

## Tracktion Engine (the effects and instruments rewritten from it)

- What: the controls and behaviour of Tracktion Engine's chorus, phaser,
  4OSC synth and sampler, rewritten in `dsp/Chorus.h`, `dsp/Phaser.h`,
  `dsp/Synth.h` and `dsp/Sampler.h` (no Tracktion code is copied).
- Where: https://github.com/Tracktion/tracktion_engine
- Licence: GNU GPLv3 (or Tracktion's commercial licence). GPLv3 code can be
  combined with this repository's AGPLv3 code.
