// ENCORE. Audio Processing
// Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
//
// The tuner's instruments, tunings, temperaments and reference pitches.
// Made by tools/tunings.py (edit the list there and run it, not this file).
// Names are UTF-8.
#pragma once

#include <array>
#include <vector>

namespace vs::tunings
{
    // strings: every string's MIDI note (60 = C4), low side first. courses: how many strings sound together
    // in each pair or group, in the same order (empty: every string on its own)
    struct Tuning     { const char* name; std::vector<int> strings; std::vector<int> courses; const char* note; };
    // transpose: written = concert + this. part: what the tuner numbers ("string" counts from the high side,
    // "tine" or "drum" from the left, "" names nothing)
    struct Instrument { const char* name; int transpose; const char* part; std::vector<Tuning> tunings; };
    struct Family     { const char* name; std::vector<Instrument> instruments; };
    struct Temperament { const char* name; std::array<float, 12> cents; };                  // offsets from equal temperament, built on C
    struct Reference  { const char* name; int hz; };

    inline const std::vector<Family>& families()
    {
        static const std::vector<Family> list {
@CATALOG@
        };
        return list;
    }

    inline const std::vector<Temperament>& temperaments()
    {
        static const std::vector<Temperament> list {
@TEMPERAMENTS@
        };
        return list;
    }

    inline const std::vector<Reference>& references()
    {
        static const std::vector<Reference> list { @REFERENCES@ };
        return list;
    }
}
