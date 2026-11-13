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

        // What a live tuner listens to at a device rate: the newest
        // window x stride samples, averaged down by stride (boxDownsample) to
        // about 24 kHz, which covers 25 Hz to 2.1 kHz and stays cheap.
        struct Window { int stride, n; };
        static Window window (double sampleRate)
        {
            return { std::max (1, (int) std::round (sampleRate / 24000.0)), 4096 };
        }

        // The pitch of the newest window of a live input (oldest first, at
        // least window (sampleRate) stride x n samples): the tuner's whole
        // reading of a stream.
        static float estimateLatest (const float* x, int count, double sampleRate)
        {
            const auto w = window (sampleRate);
            const int need = w.n * w.stride;
            if (count < need) return 0.0f;
            const auto down = boxDownsample (x + (count - need), need, w.stride);
            return estimate (down.data(), w.n, sampleRate / w.stride);
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

    // Following a tuner's pitch readings, one per estimate: a reading close
    // to the last (within closeRatio) is smoothed into it, a new note jumps;
    // after more than clearAfter readings with no pitch the note clears.
    // steady() counts how many readings in a row a string has held within a
    // tolerance (the product decides the tolerance and when that is "in tune").
    class TunerFollow
    {
    public:
        struct Settings { float closeRatio = 0.03f, keep = 0.6f, take = 0.4f; int clearAfter = 8; };

        TunerFollow() = default;
        explicit TunerFollow (Settings s) : set (s) {}

        // An estimate (0 for no pitch); returns the pitch to show, 0 for none
        float push (float hz)
        {
            if (hz > 0.0f)
            {
                quiet = 0;
                smoothed = (smoothed > 0.0f && std::abs (hz / smoothed - 1.0f) < set.closeRatio) ? smoothed * set.keep + hz * set.take : hz;
                return smoothed;
            }
            if (++quiet > set.clearAfter) { smoothed = 0.0f; steadyCount = 0; lastString = -1; }
            return smoothed;
        }

        // After a reading: how many readings in a row the same string has
        // stayed within toleranceCents (0 when it hasn't)
        int steady (const TunerReading& r, float toleranceCents)
        {
            if (r.string >= 0 && std::abs (r.cents) <= toleranceCents && r.string == lastString) ++steadyCount;
            else steadyCount = 0;
            lastString = r.string;
            return steadyCount;
        }

        bool hasNote() const { return smoothed > 0.0f; }
        void reset() { smoothed = 0.0f; quiet = 0; steadyCount = 0; lastString = -1; }

    private:
        Settings set;
        float smoothed = 0.0f;
        int quiet = 0, steadyCount = 0, lastString = -1;
    };

    // TunerFollow's settings for a note readout (a voice's note on a plugin's
    // screen, one reading per frame): no smoothing, each reading shown as it
    // is, and the last note held through up to nine readings without one,
    // cleared on the tenth.
    inline constexpr TunerFollow::Settings noteReadout { 0.0f, 0.6f, 0.4f, 9 };
}
