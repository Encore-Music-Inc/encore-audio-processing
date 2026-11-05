# Third-party notices

This repository's own code is Copyright (c) Encore Music, Inc. and licensed
under the GNU Affero General Public License v3 (see `LICENSE`).

## JUCE

- What: the JUCE framework, version 9, at commit `7ff644f65d7daa1428c34b9738ebb5c449dc9172`
- Where: git submodule at `external/JUCE` (https://github.com/juce-framework/JUCE)
- Used by: the tests in `tests/` and the desktop build of the headers in `dsp/`
  (through `juce_dsp`). The WebAssembly build (`master.wasm`) does not use JUCE:
  `dsp/lite/JuceLite.h` is ENCORE.'s own stand-in, written from scratch.
- Licence: dual licensed under the GNU AGPLv3 and the commercial JUCE 9 licence.
  This repository uses JUCE under the AGPLv3. The full terms are in
  `external/JUCE/LICENSE.md` once the submodule is checked out.

Products that build these headers with JUCE take on JUCE's licence terms
themselves, under whichever of the two licences they choose.
