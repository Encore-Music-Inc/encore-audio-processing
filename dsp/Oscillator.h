#pragma once

#include <cmath>
#include <cstdint>
#include "Random.h"

namespace vs
{
    // The polyBLEP correction at phase t (0..1) for a step of dt per sample:
    // smooths a waveform's jump so it doesn't alias (Valimaki's polynomial)
    inline double polyBlep (double t, double dt)
    {
        if (t < dt) { t /= dt; return t + t - t * t - 1; }
        if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
        return 0.0;
    }

    // An oscillator: one sample at a time at any frequency, band-limited
    // (polyBLEP) where its wave has a jump or corner.
    class Oscillator
    {
    public:
        enum class Wave { sine, triangle, sawUp, sawDown, square, noise };

        void setWave (Wave w) { wave = w; }
        // Moves to a point in the cycle (0..1); a triangle starts there on its
        // own line, so a note starting part-way through the cycle has no offset
        void setPhase (double p) { phase = p - std::floor (p); triFresh = true; }
        // A pulse's width for the square wave, 0.05 .. 0.95 (0.5 is square)
        void setPulseWidth (double pw) { width = pw < 0.05 ? 0.05 : pw > 0.95 ? 0.95 : pw; }
        void seed (std::uint32_t s) { rng = Xorshift32 (s); }
        double getPhase() const { return phase; }

        // The next sample, -1..1, at hz for a sample rate of fs
        float next (double hz, double fs) { return (float) nextValue (hz, fs); }

        // The same, unrounded (an LFO that steers something in double)
        double nextValue (double hz, double fs)
        {
            const double dt = std::fabs (hz) / fs;
            double v = 0.0;
            switch (wave)
            {
                case Wave::sine:     v = std::sin (6.283185307179586 * phase); break;
                case Wave::sawUp:    v = 2.0 * phase - 1.0 - polyBlep (phase, dt); break;
                case Wave::sawDown:  v = 1.0 - 2.0 * phase + polyBlep (phase, dt); break;
                case Wave::square:
                {
                    v = phase < width ? 1.0 : -1.0;
                    v += polyBlep (phase, dt);
                    v -= polyBlep (std::fmod (phase + 1.0 - width, 1.0), dt);
                    break;
                }
                case Wave::triangle:
                {
                    // a band-limited square, integrated (leaky, so it never drifts)
                    double sq = phase < 0.5 ? 1.0 : -1.0;
                    sq += polyBlep (phase, dt);
                    sq -= polyBlep (std::fmod (phase + 0.5, 1.0), dt);
                    // the integral follows the triangle half a step ahead: start it there
                    if (triFresh) { tri = triangleAt (std::fmod (phase + 0.5 * dt, 1.0)) - dt * sq * 4.0; triFresh = false; }
                    tri = dt * sq * 4.0 + (1.0 - dt * 0.01) * tri;
                    v = tri;
                    break;
                }
                case Wave::noise:    v = rng.uniform() * 2.0 - 1.0; break;
            }
            phase += dt;
            if (phase >= 1.0) phase -= std::floor (phase);
            return v;
        }

    private:
        Wave wave = Wave::sine;
        // The triangle the integrated square follows, at a point in its cycle: -1 at 0, 1 at a half
        static double triangleAt (double p) { return p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p; }

        double phase = 0.0, width = 0.5, tri = -1.0;
        bool triFresh = true;
        Xorshift32 rng { 1 };
    };
}
