// ENCORE. Audio Processing
// Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
//
// The pitch detector for a single voice or instrument: YIN (de Cheveigne &
// Kawahara) on one window of samples. Every pitch reading in ENCORE. Audio
// Processing comes from this one class, with one of three sets of settings:
//
//   Yin::follow()  following a voice every few milliseconds, as the pitch
//                  engine does (vs::PitchDetector): the difference function
//                  through the FFT, so a fixed window is cheap to analyse.
//   Yin::tuner()   the tuner (vs::TunerPitch), from a bass dropped to low A to
//                  a piccolo's top.
//   Yin::voice()   a singing voice (vs::VoiceTracker): longer lags, every dip
//                  kept as a candidate period, and octave slips caught.
//
// Plain C++ with no JUCE, so the desktop products and the browser
// (master.wasm) read pitch the same way.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>
#include "Fft.h"
#include "Stats.h"

namespace vs
{
    class Yin
    {
    public:
        // How the difference function d(tau) = sum (x[i] - x[i + tau])^2 is found:
        // summed directly, or through a cross-correlation by FFT (cheap for a
        // fixed window analysed often).
        enum class Difference { direct, fft };

        // The lags searched. half: up to half the window or the longest period
        // (whichever is shorter), d computed up to it. voice: up to 5/8 of the
        // window or one lag past the longest period, d computed one lag further
        // so every searched lag has a neighbour on each side.
        enum class Lags { half, voice };

        // What counts as silence. windowEnergy: the mean square of the samples
        // compared (the first n - longest lag) under 1e-6. rms: the RMS of the
        // whole window under 1e-3. Both are about -60 dB.
        enum class Gate { windowEnergy, rms };

        // Which dip is the period. firstDip: YIN's rule, the first dip under the
        // threshold followed down to its bottom, else the deepest dip if it is
        // clear enough. voice: among the local dips under 0.5, the first under the
        // threshold (else the deepest), then a dip at half the period that is
        // nearly as deep wins (an octave-down slip), and a far deeper dip at twice
        // the period wins over a shallow one (an octave-up slip).
        enum class Pick { firstDip, voice };

        // The precision of the sample differences, the normalisation's product
        // and the parabolic step: single (float) or double.
        enum class Arithmetic { single, doublePrecision };

        struct Settings
        {
            double minHz = 65.0, maxHz = 1100.0;
            float threshold = 0.15f;
            Difference difference = Difference::direct;
            Lags lags = Lags::half;
            Gate gate = Gate::windowEnergy;
            Pick pick = Pick::firstDip;
            bool searchLongestLag = true;  // firstDip: whether the longest lag itself can be the period
            float deepestLimit = 0.3f;     // firstDip: the deepest dip counts only under this
            bool deepestAtLimit = false;   // firstDip: ... or exactly at it
            Arithmetic arithmetic = Arithmetic::single;
        };

        // Following a voice (the pitch engine's detector), 80 Hz to 1.2 kHz.
        static Settings follow()
        {
            Settings s;
            s.minHz = 80.0; s.maxHz = 1200.0; s.threshold = 0.15f;
            s.difference = Difference::fft;
            s.searchLongestLag = false;
            s.deepestAtLimit = true;
            return s;
        }

        // The tuner, 25 Hz to 2.1 kHz unless asked otherwise.
        static Settings tuner (float minHz = 25.0f, float maxHz = 2100.0f)
        {
            Settings s;
            s.minHz = minHz; s.maxHz = maxHz; s.threshold = 0.12f;
            return s;
        }

        // A singing voice, 65 Hz to 1.1 kHz unless asked otherwise.
        static Settings voice (double minHz = 65.0, double maxHz = 1100.0)
        {
            Settings s;
            s.minHz = minHz; s.maxHz = maxHz; s.threshold = 0.15f;
            s.lags = Lags::voice;
            s.gate = Gate::rms;
            s.pick = Pick::voice;
            s.arithmetic = Arithmetic::doublePrecision;
            return s;
        }

        // A possible period: its frequency and how deep its dip is (0 = perfectly periodic).
        struct Candidate
        {
            double hz = 0.0;
            float d = 1.0f;
        };

        struct Result
        {
            bool voiced = false;
            double hz = 0.0;       // 0 when there's no clear note
            double period = 0.0;   // in samples, with its fraction
            float dip = 1.0f;      // the normalised difference at the period
            double clarity = 0.0;  // 1 - dip: 1 is perfectly periodic
            double rms = 0.0;      // of the whole window (0 when it is too short to analyse)
            std::vector<Candidate> candidates; // Pick::voice: every dip, shortest period first
        };

        Yin() = default;
        explicit Yin (const Settings& s) : settings (s) {}

        const Settings& getSettings() const { return settings; }

        // Sets up for windows of n samples at this rate, so analyse() on them
        // doesn't allocate. Call it off the audio thread.
        void prepare (double sampleRate, int n) { setup (sampleRate, n); }

        // The lags searched for the prepared window (shortest and longest period, in samples).
        int minLag() const { return tauMin; }
        int maxLag() const { return tauMax; }

        // x[0..n), oldest first. Another n or rate than the prepared one sets up
        // again (and allocates). The result stays valid until the next call.
        const Result& analyse (const float* x, int n, double sampleRate)
        {
            if (n != size || sampleRate != fs) setup (sampleRate, n);
            out.voiced = false;
            out.hz = out.period = out.clarity = out.rms = 0.0;
            out.dip = 1.0f;
            out.candidates.clear();
            if (w <= 0) return out;

            double windowEnergy = 0.0, energy = 0.0;
            if (settings.difference == Difference::fft)
            {
                prefix[0] = 0.0;
                for (int i = 0; i < n; ++i)
                    prefix[(size_t) i + 1] = prefix[(size_t) i] + (double) x[i] * x[i];
                windowEnergy = prefix[(size_t) w];
                energy = prefix[(size_t) n];
            }
            else
            {
                for (int i = 0; i < n; ++i)
                {
                    if (i == w) windowEnergy = energy;
                    energy += (double) x[i] * x[i];
                }
            }
            out.rms = std::sqrt (energy / n);
            if (settings.gate == Gate::rms ? out.rms < 1.0e-3 : windowEnergy / w < 1.0e-6)
                return out;

            if (settings.difference == Difference::fft) fftDifference (x, windowEnergy);
            else directDifference (x);

            const int best = settings.pick == Pick::voice ? pickVoice() : pickFirstDip();
            if (best < 0)
            {
                out.candidates.clear();
                return out;
            }
            out.voiced = true;
            out.period = refine (best);
            out.hz = fs / out.period;
            out.dip = d[(size_t) best];
            out.clarity = 1.0 - (double) out.dip;
            return out;
        }

    private:
        Settings settings;
        double fs = 0.0;
        int size = -1, tauMin = 2, tauMax = 0, last = 0, w = 0, m = 0;
        std::vector<float> d;
        std::vector<int> dips;
        std::vector<double> prefix, a, b, aRe, aIm, bRe, bIm;
        RealFft fft;
        Result out;

        void setup (double sampleRate, int n)
        {
            fs = sampleRate;
            size = n;
            tauMin = std::max (2, (int) (fs / settings.maxHz));
            if (settings.lags == Lags::voice)
            {
                tauMax = std::min ((n >> 1) + (n >> 3), (int) std::ceil (fs / settings.minHz) + 1);
                last = tauMax + 1;
            }
            else
            {
                tauMax = std::min (n / 2, (int) (fs / settings.minHz));
                last = tauMax;
            }
            w = n - last;
            d.assign ((size_t) std::max (0, last) + 2, 0.0f);
            dips.reserve ((size_t) std::max (0, tauMax) + 1);
            out.candidates.reserve ((size_t) std::max (0, tauMax) + 1);
            if (settings.difference == Difference::fft)
            {
                m = 2;
                while (m < 2 * n) m <<= 1;
                fft.setSize (m);
                a.assign ((size_t) m, 0.0);
                b.assign ((size_t) m, 0.0);
                aRe.assign ((size_t) m / 2 + 1, 0.0); aIm.assign ((size_t) m / 2 + 1, 0.0);
                bRe.assign ((size_t) m / 2 + 1, 0.0); bIm.assign ((size_t) m / 2 + 1, 0.0);
                prefix.assign ((size_t) n + 1, 0.0);
            }
        }

        // d(tau) summed directly, then the cumulative mean normalised difference.
        void directDifference (const float* x)
        {
            const bool single = settings.arithmetic == Arithmetic::single;
            for (int tau = 1; tau <= last; ++tau)
            {
                double s = 0.0;
                if (single)
                    for (int i = 0; i < w; ++i) { const double diff = x[i] - x[i + tau]; s += diff * diff; }
                else
                    for (int i = 0; i < w; ++i) { const double diff = (double) x[i] - (double) x[i + tau]; s += diff * diff; }
                d[(size_t) tau] = (float) s;
            }
            double running = 0.0;
            d[0] = 1.0f;
            for (int tau = 1; tau <= last; ++tau)
            {
                running += d[(size_t) tau];
                const double scaled = single ? (double) (d[(size_t) tau] * (float) tau) : (double) d[(size_t) tau] * tau;
                d[(size_t) tau] = running > 0.0 ? (float) (scaled / running) : 1.0f;
            }
        }

        // d(tau) = e(0) + e(tau) - 2 r(tau): the energies from a running sum, the
        // cross-correlation r through the FFT; then the normalisation.
        void fftDifference (const float* x, double e0)
        {
            std::fill (a.begin(), a.end(), 0.0);
            std::fill (b.begin(), b.end(), 0.0);
            std::copy (x, x + w, a.begin());
            std::copy (x, x + size, b.begin());

            fft.forward (a.data(), aRe.data(), aIm.data());
            fft.forward (b.data(), bRe.data(), bIm.data());

            // cross-correlation: conj(A) * B
            for (int k = 0; k <= m / 2; ++k)
            {
                const double ar = aRe[(size_t) k], ai = aIm[(size_t) k];
                const double br = bRe[(size_t) k], bi = bIm[(size_t) k];
                aRe[(size_t) k] = ar * br + ai * bi;
                aIm[(size_t) k] = ar * bi - ai * br;
            }
            fft.inverse (aRe.data(), aIm.data(), a.data());

            d[0] = 1.0f;
            double running = 0.0;
            for (int tau = 1; tau <= last; ++tau)
            {
                const double et = prefix[(size_t) (tau + w)] - prefix[(size_t) tau];
                const double diff = std::max (0.0, e0 + et - 2.0 * a[(size_t) tau]);
                running += diff;
                d[(size_t) tau] = running > 0.0 ? (float) (diff * tau / running) : 1.0f;
            }
        }

        int pickFirstDip() const
        {
            const int top = settings.searchLongestLag ? tauMax : tauMax - 1;
            if (tauMin > tauMax) return -1; // the window is too short for even the highest note
            for (int tau = tauMin; tau <= top; ++tau)
            {
                if (d[(size_t) tau] < settings.threshold)
                {
                    while (tau + 1 <= top && d[(size_t) tau + 1] < d[(size_t) tau]) ++tau;
                    return tau;
                }
            }
            int lowest = tauMin;
            for (int tau = tauMin; tau <= top; ++tau)
                if (d[(size_t) tau] < d[(size_t) lowest]) lowest = tau;
            const float v = d[(size_t) lowest];
            const bool clear = settings.deepestAtLimit ? ! (v > settings.deepestLimit) : v < settings.deepestLimit;
            return clear ? lowest : -1;
        }

        int pickVoice()
        {
            dips.clear();
            for (int tau = tauMin; tau <= tauMax; ++tau)
            {
                const float v = d[(size_t) tau];
                if (v < 0.5f && v < d[(size_t) tau - 1] && v <= d[(size_t) tau + 1]) dips.push_back (tau);
            }
            if (dips.empty()) return -1;
            auto dv = [this] (int t) { return (double) d[(size_t) t]; };

            int pick = -1;
            for (int t : dips) if (dv (t) < (double) settings.threshold) { pick = t; break; }
            if (pick < 0)
            {
                pick = dips[0];
                for (int t : dips) if (dv (t) < dv (pick)) pick = t;
            }
            // an octave-down slip: a dip at half the period that's nearly as deep is the real note
            for (int guard = 0; guard < 3; ++guard)
            {
                const int p = pick;
                int half = -1;
                for (int t : dips)
                    if (std::abs (t - p / 2.0) <= std::max (1.5, p * 0.03)) { half = t; break; }
                if (half >= 0 && dv (half) < std::min (0.35, dv (p) + 0.1)) pick = half;
                else break;
            }
            // an octave-up slip: a weak fundamental under a strong 2nd harmonic nearly
            // repeats at half the period; the true period's dip is then far deeper
            int dbl = -1;
            for (int t : dips)
                if (std::abs (t - pick * 2.0) <= std::max (1.5, pick * 0.06)) { dbl = t; break; }
            if (dbl >= 0 && dv (pick) > 0.08 && dv (dbl) < dv (pick) * 0.3) pick = dbl;

            for (int t : dips) out.candidates.push_back ({ fs / refine (t), d[(size_t) t] });
            return pick;
        }

        // The lag with a parabola through the dip and its neighbours.
        double refine (int tau) const
        {
            if (! (tau > 1 && tau < last)) return (double) tau;
            if (settings.arithmetic == Arithmetic::single)
            {
                const float y0 = d[(size_t) tau - 1], y1 = d[(size_t) tau], y2 = d[(size_t) tau + 1];
                return (double) ((float) tau + stats::parabolicOffset (y0, y1, y2, 1.0e-9f));
            }
            const double y0 = d[(size_t) tau - 1], y1 = d[(size_t) tau], y2 = d[(size_t) tau + 1];
            return tau + stats::parabolicOffset (y0, y1, y2, 1.0e-9);
        }
    };

    // A block averaged down by `stride` (every `stride` samples become their
    // mean; a remainder at the end is dropped): a cheaper pitch reading, and
    // plenty for notes up to 2 kHz from 48 kHz audio.
    inline std::vector<float> boxDownsample (const float* x, int n, int stride)
    {
        if (stride <= 1) return std::vector<float> (x, x + std::max (0, n));
        std::vector<float> y ((size_t) std::max (0, n / stride));
        for (size_t i = 0; i < y.size(); ++i)
        {
            double s = 0.0;
            for (int k = 0; k < stride; ++k) s += x[i * (size_t) stride + (size_t) k];
            y[i] = (float) (s / stride);
        }
        return y;
    }
}
