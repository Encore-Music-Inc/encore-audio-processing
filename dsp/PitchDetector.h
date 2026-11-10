#pragma once

#include "Yin.h"

namespace vs
{
    // The pitch engine's detector: vs::Yin with Yin::follow(), tuned for
    // following a voice (80 Hz to 1200 Hz) every few milliseconds on a fixed
    // window, without allocating once prepared.
    class PitchDetector
    {
    public:
        struct Result
        {
            bool voiced = false;
            float f0 = 0.0f;         // Hz
            float confidence = 0.0f; // 0..1
        };

        void prepare (double sampleRate)
        {
            fs = sampleRate;
            n = sampleRate > 60000.0 ? 4096 : 2048;
            yin.prepare (fs, n);
        }

        int windowSize() const { return n; }
        float maxPeriod() const { return (float) yin.maxLag(); }
        float minPeriod() const { return (float) yin.minLag(); }

        // x holds the windowSize() most recent samples, oldest first.
        Result analyse (const float* x)
        {
            Result r;
            const auto& y = yin.analyse (x, n, fs);
            if (! y.voiced) return r;
            r.voiced = true;
            r.f0 = (float) fs / (float) y.period;
            r.confidence = 1.0f - y.dip;
            return r;
        }

    private:
        double fs = 44100.0;
        int n = 2048;
        Yin yin { Yin::follow() };
    };
}
