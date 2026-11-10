#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace vs
{
    // The tuner: its pitch detector, YIN (de Cheveigne & Kawahara) on one
    // block of samples, from a bass dropped to low A to a piccolo's top, and
    // TunerReading below, what that pitch means on the instrument. Plain C++
    // with no JUCE, so the desktop tuner and the browser (master.wasm) run the
    // same code. PitchDetector.h is the other YIN here, made for following a voice.
    struct TunerPitch
    {
        // The pitch in Hz of x[0..n), or 0 when there's no clear note.
        static float estimate (const float* x, int n, double sampleRate, float minHz = 25.0f, float maxHz = 2100.0f)
        {
            const int tauMin = std::max (2, (int) (sampleRate / maxHz)), tauMax = std::min (n / 2, (int) (sampleRate / minHz));
            const int w = n - tauMax;
            if (w <= 0) return 0.0f;
            double energy = 0;
            for (int i = 0; i < w; ++i) energy += (double) x[i] * x[i];
            if (energy / w < 1.0e-6) return 0.0f; // quieter than about -60 dB: no note
            std::vector<float> d ((size_t) tauMax + 1, 0.0f);
            for (int tau = 1; tau <= tauMax; ++tau)
            {
                double s = 0;
                for (int i = 0; i < w; ++i) { const double diff = x[i] - x[i + tau]; s += diff * diff; }
                d[(size_t) tau] = (float) s;
            }
            // cumulative mean normalised difference
            double running = 0;
            d[0] = 1.0f;
            for (int tau = 1; tau <= tauMax; ++tau)
            {
                running += d[(size_t) tau];
                d[(size_t) tau] = running > 0 ? (float) (d[(size_t) tau] * tau / running) : 1.0f;
            }
            // the first dip under the threshold, followed down to its bottom (else the deepest dip, if it's clear enough)
            int best = -1;
            for (int tau = tauMin; tau <= tauMax; ++tau)
                if (d[(size_t) tau] < 0.12f)
                {
                    while (tau + 1 <= tauMax && d[(size_t) tau + 1] < d[(size_t) tau]) ++tau;
                    best = tau;
                    break;
                }
            if (best < 0)
            {
                int lowest = tauMin;
                for (int tau = tauMin; tau <= tauMax; ++tau) if (d[(size_t) tau] < d[(size_t) lowest]) lowest = tau;
                if (d[(size_t) lowest] < 0.3f) best = lowest;
            }
            if (best < 0) return 0.0f;
            // parabolic interpolation around the dip
            float t = (float) best;
            if (best > 1 && best < tauMax)
            {
                const float a = d[(size_t) best - 1], b = d[(size_t) best], c = d[(size_t) best + 1];
                const float denom = a - 2 * b + c;
                if (std::abs (denom) > 1.0e-9f) t += 0.5f * (a - c) / denom;
            }
            return (float) (sampleRate / t);
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
            const double m = 69.0 + 12.0 * std::log2 (hz / a4);
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
            r.targetHz = (float) (a4 * std::pow (2.0, (target (r.note) - 69.0) / 12.0));
            return r;
        }
    };
}
