#pragma once

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>
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

    // A periodic wave made of harmonics: amps[k] is the (k+1)th harmonic's
    // amplitude, every one in sine phase, scaled so the wave's peak is full
    // scale (as the browser's PeriodicWave is). Harmonics at or above Nyquist
    // for the pitch played are left out.
    class HarmonicWave
    {
    public:
        HarmonicWave() = default;
        explicit HarmonicWave (std::vector<double> amplitudes) : amps (std::move (amplitudes))
        {
            double p = 0;
            for (int i = 0; i < 4096; ++i) p = std::max (p, std::abs (sum (i / 4096.0, 0.0)));
            scale = p > 0 ? 1.0 / p : 1.0;
        }

        // The wave at phase 0..1 when played at hz (0: every harmonic)
        double at (double phase, double hz = 0.0, double sampleRate = 0.0) const
        {
            return sum (phase, hz > 0 && sampleRate > 0 ? 0.5 * sampleRate / hz : 0.0) * scale;
        }

    private:
        std::vector<double> amps;
        double scale = 1.0;

        double sum (double phase, double below) const
        {
            double v = 0;
            for (size_t k = 0; k < amps.size(); ++k)
            {
                if (below > 0 && (double) (k + 1) >= below) break;
                v += amps[k] * std::sin (2 * 3.141592653589793 * (double) (k + 1) * phase);
            }
            return v;
        }
    };

    // A plucked note at hz, seconds long: each harmonic a sine (amplitude
    // amps[k]) dying away faster than the one below (falling 63% every
    // 1 / (decayBase + k x decayStep) seconds), those near Nyquist left out,
    // normalised to full scale, rising over riseSec and falling to silence over
    // the last fallSec. A plucked string's reference tone, a guitar's or a
    // piano's note.
    inline std::vector<float> renderPluck (double hz, double sampleRate, double seconds, const std::vector<double>& amps,
                                           double decayBase, double decayStep, double riseSec, double fallSec)
    {
        const int n = (int) std::floor (sampleRate * seconds);
        std::vector<float> buf ((size_t) std::max (0, n), 0.0f);
        for (size_t k = 0; k < amps.size(); ++k)
        {
            const double w = 2 * 3.141592653589793 * hz * (double) (k + 1) / sampleRate;
            if (w >= 3.141592653589793 * 0.9) break;
            const double decay = std::exp (-(decayBase + (double) k * decayStep) / sampleRate);
            double amp = amps[k];
            for (int t = 0; t < n; ++t) { buf[(size_t) t] += (float) (amp * std::sin (w * t)); amp *= decay; }
        }
        float peak = 0;
        for (float v : buf) peak = std::max (peak, std::abs (v));
        const int rise = (int) std::floor (sampleRate * riseSec), fall = (int) std::floor (sampleRate * fallSec);
        for (int t = 0; t < n; ++t)
        {
            float g = 1.0f / (peak > 0 ? peak : 1.0f);
            if (t < rise) g *= (float) t / (float) rise;
            if (t > n - fall) g *= (float) (n - t) / (float) fall;
            buf[(size_t) t] *= g;
        }
        return buf;
    }
}
