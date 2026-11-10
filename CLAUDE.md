# Notes for Claude (and whoever is driving it)

This is ENCORE.'s shared audio processing code: header-only C++20 used by ENCORE.'s
products.

## What uses it

- ENCORE. VST (encore-vst): the plugins, built with JUCE (`juce_dsp`).
- ENCORE. Basement Studio (encore-basement-studio): the desktop DAW, built with JUCE.
- Green Room, the artist tools in the ENCORE. app (encoremusic.studio): through `master.wasm` (built from
  `wasm/api_*.cpp` and the headers, without JUCE).

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
| `dsp/` | the headers (`MasterChain.h`, `EqBand.h`, `ReverbEngine.h`, ...) |
| `dsp/lite/JuceLite.h` | stand-in for the JUCE pieces the headers use, for builds without JUCE (`VS_NO_JUCE=1`). Don't delete it: the wasm build needs it |
| `tools/tunings.py` | the one list of the tuner's instruments, tunings, temperaments and reference pitches; writes `dsp/Tunings.h` and `data/tunings.json` (edit the list and run it, never the two files; CI fails if they differ from what it writes) |
| `wasm/api_*.cpp`, `wasm/build.sh` | the browser engine, one file per area with its own prefix (the mastering chain `mc_*`, the loudness meter `lm_*`, the tuner `tp_*`, ...), and its Emscripten build, which exports every `EMSCRIPTEN_KEEPALIVE` function |
| `tests/` | the offline test suites and their CMake build |
| `external/JUCE` | JUCE, pinned to a release (the README's Pinned releases table): the JUCE version for all of ENCORE. (see below) |
| `CHANGELOG.md` | every release's notes, newest first; the release job copies the version's section onto the GitHub release |
| `.github/release-notes.sh` | prints a version's notes from `CHANGELOG.md`, and fails if they're missing (CI runs it on every build) |
| `.github/pins/check.sh` | CI's check that every pin is a release (the README's Pinned releases table) |
| `.github/sanitizers/` | the scheduled sanitizer run's exceptions (`ubsan.supp`, each with its reason; keep it short) and `report.sh` |
| `.github/workflows/ci.yml` | tests on Linux, Windows and macOS and master.wasm on pull requests and main (skipped when only docs change, and for a pull request that only changes the version line and `CHANGELOG.md`); on a schedule: the tests with sanitizers; `v*` tags attach main's master.wasm and tunings.json to the Release, no rebuild; "CI passed" is the one check to require |

## Consumers

- They add this repo as a submodule at `external/encore-audio-processing` and put
  that folder on their include path, so `#include "dsp/MasterChain.h"` resolves.
  Keep includes in that form: never move or rename headers without a major version.
- `external/JUCE` here is the JUCE version for all of ENCORE.: products that
  use this repo build with it instead of a JUCE of their own, so the JUCE these
  tests run on is the JUCE that ships. Consumers fetch it by its path
  (`git -C external/encore-audio-processing submodule update --init --depth 1 external/JUCE`),
  never with `--recursive`. Move JUCE only to a JUCE release.
- They pin the submodule to a release tag. Green Room pins `master.wasm` and `tunings.json` by their SHA-256.

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

## Versioning and releases

- Semantic versioning, tags `vMAJOR.MINOR.PATCH`, starting at v1.0.0. MAJOR for
  anything that breaks a consumer (headers, classes, functions, the `mc_*` wasm
  exports, or a sound change a product must re-check); MINOR for additions;
  PATCH for fixes. The README has the details.

## Release process

1. The version lives in exactly one place: `project(EncoreAudioProcessingTests VERSION x.y.z)`
   in `tests/CMakeLists.txt`.
2. Changing it is its own small pull request ("Release vX.Y.Z"), so a person sees the
   number before it's built. It adds the release notes to `CHANGELOG.md` (see
   Release notes below); CI fails until they're there.
3. Merging builds everything on `main`: the tests run and `master.wasm` and `tunings.json`
   are uploaded as a workflow artifact.
4. After the `main` build of that commit has passed, a person publishes the release in
   GitHub. Never a Claude session. Releases, Draft a new release; tag `vX.Y.Z` (Create
   new tag) with target `main` at the "Release vX.Y.Z" commit; leave the title and the
   description empty, don't click Generate release notes and don't attach files (the
   release job fills all of them in); Publish. Never publish an old draft release:
   publishing creates its tag on whatever its target is at that moment.
5. The tag's CI run doesn't rebuild. It checks that the tag matches the version, finds
   the successful `main` build of that exact commit and attaches its `master.wasm`,
   `tunings.json` and their `.sha256` files to the Release, and sets its title
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
