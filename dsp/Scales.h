#pragma once

// Pitch correction's keys, scales and harmony intervals: the 12 keys (named by
// the note-name rule in Music.h), the 5 scales a voice can be corrected to
// (taken from the scale library, ScaleLibrary.h), and the harmony voices'
// intervals. The lists are plain C++; with JUCE they are also juce::StringArray
// (keyNames, scaleNames, intervalNames) for plugin parameters.

#include <array>
#include <cmath>
#include "Music.h"
#include "ScaleLibrary.h"
#if ! VS_NO_JUCE
 #include <juce_core/juce_core.h>
#endif

namespace vs::scales
{
    // The keys, C to B, named by the rule (C, C#, D, Eb, ...)
    inline constexpr std::array<const char*, 12> keyNameList { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    inline constexpr std::array<const char*, 5> scaleNameList { "Chromatic", "Major", "Minor", "Major Pent", "Minor Pent" };
    inline constexpr std::array<const char*, 12> intervalNameList { "Off", "Double", "Oct Down", "6th Down", "5th Down", "4th Down",
                                                                    "3rd Down", "3rd Up", "4th Up", "5th Up", "6th Up", "Oct Up" };

   #if VS_NO_JUCE
    inline constexpr const auto& keyNames = keyNameList;
    inline constexpr const auto& scaleNames = scaleNameList;
    inline constexpr const auto& intervalNames = intervalNameList;
   #else
    inline const juce::StringArray keyNames { keyNameList.data(), (int) keyNameList.size() };
    inline const juce::StringArray scaleNames { scaleNameList.data(), (int) scaleNameList.size() };
    inline const juce::StringArray intervalNames { intervalNameList.data(), (int) intervalNameList.size() };
   #endif

    struct Scale { const int* steps; int size; };

    // Pitch correction's scale (index into scaleNames): its semitones above the key
    inline Scale get (int index)
    {
        static constexpr std::array<const LibraryScale*, 5> table { find ("chromatic"), find ("ionian"), find ("aeolian"),
                                                                    find ("major_pent"), find ("minor_pent") };
        const auto* s = table[(size_t) (index >= 1 && index <= 4 ? index : 0)];
        return { s->semis.data(), (int) s->semis.size() };
    }

    inline int floorDiv (int a, int b) { return music::floorDiv (a, b); }
    inline int mod (int a, int b)      { return a - b * floorDiv (a, b); }

    inline bool inScale (int note, int key, Scale s)
    {
        const int pc = mod (note - key, 12);
        for (int i = 0; i < s.size; ++i)
            if (s.steps[i] == pc) return true;
        return false;
    }

    inline int nearest (float midi, int key, Scale s)
    {
        const int centre = (int) std::round (midi);
        int best = centre;
        float bestDist = 1.0e9f;
        for (int note = centre - 3; note <= centre + 3; ++note)
        {
            if (! inScale (note, key, s)) continue;
            const float dist = std::abs (midi - (float) note);
            if (dist < bestDist) { bestDist = dist; best = note; }
        }
        return best;
    }

    // Note a harmony singer would sing against leadNote. 7-note scales move by
    // scale degrees (so a "3rd up" is major or minor as the key demands);
    // other scales use fixed intervals.
    inline int harmonyNote (int leadNote, int intervalIndex, int key, int scaleIndex)
    {
        static const int degreeSteps[12] { 0, 0, -7, -5, -4, -3, -2, 2, 3, 4, 5, 7 };
        static const int majorSemis[12]  { 0, 0, -12, -9, -7, -5, -4, 4, 5, 7, 9, 12 };
        static const int minPentSemis[12] { 0, 0, -12, -8, -7, -5, -3, 3, 5, 7, 8, 12 };

        if (intervalIndex <= 1) return leadNote;

        const Scale s = get (scaleIndex);
        if (s.size != 7)
            return leadNote + (scaleIndex == 4 ? minPentSemis : majorSemis)[intervalIndex];

        const int rel = leadNote - key;
        int oct = floorDiv (rel, 12);
        const int pc = rel - 12 * oct;

        int idx = 0;
        for (int i = 0; i < 7; ++i)
            if (s.steps[i] <= pc) idx = i;
        const int offset = pc - s.steps[idx];

        int ni = idx + degreeSteps[intervalIndex];
        oct += floorDiv (ni, 7);
        ni = mod (ni, 7);
        return key + 12 * oct + s.steps[ni] + offset;
    }
}
