#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include "Music.h"
#include "Yin.h"

namespace vs
{
    // The tuner: its pitch, vs::Yin with Yin::tuner() on one block of
    // samples, from a bass dropped to low A to a piccolo's top, and
    // TunerReading below, what that pitch means on the instrument. Plain C++
    // with no JUCE, so the desktop tuner and the browser (master.wasm) run the
    // same code.
    struct TunerPitch
    {
        // The pitch in Hz of x[0..n), or 0 when there's no clear note.
        static float estimate (const float* x, int n, double sampleRate, float minHz = 25.0f, float maxHz = 2100.0f)
        {
            Yin yin (Yin::tuner (minHz, maxHz));
            const auto& r = yin.analyse (x, n, sampleRate);
            return r.voiced ? (float) r.hz : 0.0f;
        }
    };

    // What a heard pitch means on the instrument: the string it's nearest
    // (within 3.5 semitones), or else the nearest note, and how many cents off,
    // in the chosen temperament and reference pitch.
    struct TunerReading
    {
        int note = -1;          // the target, as a MIDI note (concert pitch)
        int string = -1;        // which string of the tuning, or -1 when it's chromatic
        float cents = 0;        // + sharp, - flat, from the target (in the chosen temperament)
        float targetHz = 0;

        // hz: what was heard. strings: the tuning's notes (empty: chromatic). offsets: the temperament on C, rotated to root
        static TunerReading of (float hz, double a4, const std::vector<int>& strings, const std::array<float, 12>& offsets, int root)
        {
            TunerReading r;
            if (! (hz > 0)) return r;
            const double m = music::hzToMidi ((double) hz, a4);
            auto target = [&] (int n) { return n + offsets[(size_t) (((n - root) % 12 + 12) % 12)] / 100.0; };
            if (! strings.empty())
            {
                double best = 1.0e9;
                for (int i = 0; i < (int) strings.size(); ++i)
                {
                    const double c = (m - target (strings[(size_t) i])) * 100.0;
                    if (std::abs (c) < std::abs (best)) { best = c; r.string = i; }
                }
                if (std::abs (best) <= 350.0) { r.note = strings[(size_t) r.string]; r.cents = (float) best; }
                else r.string = -1;
            }
            if (r.string < 0)
            {
                int n = (int) std::round (m);
                for (int k : { n - 1, n + 1 }) if (std::abs (m - target (k)) < std::abs (m - target (n))) n = k;
                r.note = n;
                r.cents = (float) ((m - target (n)) * 100.0);
            }
            r.targetHz = (float) music::midiToHz (target (r.note), a4);
            return r;
        }
    };
}
