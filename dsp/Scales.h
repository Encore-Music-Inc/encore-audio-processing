#pragma once

#include <cmath>
#include <juce_core/juce_core.h>

namespace vs::scales
{
    inline const juce::StringArray keyNames { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    inline const juce::StringArray scaleNames { "Chromatic", "Major", "Minor", "Major Pent", "Minor Pent" };
    inline const juce::StringArray intervalNames { "Off", "Double", "Oct Down", "6th Down", "5th Down", "4th Down",
                                                   "3rd Down", "3rd Up", "4th Up", "5th Up", "6th Up", "Oct Up" };

    struct Scale { const int* steps; int size; };

    inline Scale get (int index)
    {
        static const int chromatic[12] { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
        static const int major[7] { 0, 2, 4, 5, 7, 9, 11 };
        static const int minor[7] { 0, 2, 3, 5, 7, 8, 10 };
        static const int majPent[5] { 0, 2, 4, 7, 9 };
        static const int minPent[5] { 0, 3, 5, 7, 10 };

        switch (index)
        {
            case 1:  return { major, 7 };
            case 2:  return { minor, 7 };
            case 3:  return { majPent, 5 };
            case 4:  return { minPent, 5 };
            default: return { chromatic, 12 };
        }
    }

    inline int floorDiv (int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }
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
