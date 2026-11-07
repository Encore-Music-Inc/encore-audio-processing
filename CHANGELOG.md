# Changes

Each release's notes, newest first. When a release is published, the release
job puts its section on the GitHub release (see "Release notes" in
`CLAUDE.md`).

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
