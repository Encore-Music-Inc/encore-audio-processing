// ENCORE. Audio Processing
// Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
//
// A song's tempo, two ways: from taps, and by listening to it.
//
//   fitTaps / tapTempo  a straight line fitted through a run of taps, so the
//                       answer gets more exact the longer you tap; a tap more
//                       than a third of a beat off the line is left out.
//   TapTempo            the same fit for a live tap button: a pause starts
//                       over, and only the latest taps count.
//   TempoFinder         listens to mono audio: how much new sound starts in
//                       each hundredth of a second (spectral flux), then the
//                       beat spacing that lines up with those starts best, with
//                       how sure it is and the half and double tempos.
//
// Plain C++ with no JUCE, so the desktop products and the browser
// (master.wasm) find the same tempo.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>
#include "dsp/Fft.h"
#include "dsp/Windows.h"
#include "dsp/Util.h"

namespace vs::tempo
{
    // A tempo fitted through a run of taps.
    struct TapFit
    {
        bool valid = false;  // false: too few taps
        double bpm = 0.0;
        int taps = 0;        // taps given
        int used = 0;        // taps the fit used (way-off taps are left out)
        double spreadMs = 0; // how far the used taps sit from the fitted line, RMS, in ms
    };

    namespace detail
    {
        struct TapLine { double a = 0.0, b = 0.0; };

        // The least-squares line through (beat number n[i], time t[i]); 500 ms per beat when every n is the same.
        inline TapLine tapLine (const double* t, const double* n, const int* idx, int count)
        {
            double mx = 0.0, my = 0.0;
            for (int i = 0; i < count; ++i) mx += n[idx[i]];
            mx /= count;
            for (int i = 0; i < count; ++i) my += t[idx[i]];
            my /= count;
            double sxy = 0.0, sxx = 0.0;
            for (int i = 0; i < count; ++i)
            {
                const double dn = n[idx[i]] - mx;
                sxy += dn * (t[idx[i]] - my);
                sxx += dn * dn;
            }
            const double b = sxx > 0 ? sxy / sxx : 500.0;
            return { my - b * mx, b };
        }

        // Rounds half up, as the web app does (2.5 is 3, -2.5 is -2)
        using vs::roundHalfUp;
    }

    // Tempo from tap times in ms: each tap numbered by the beats since the
    // first (from the median gap, so a missed tap still lands on the right
    // beat), a least-squares line through (beat, time), then again without
    // the taps more than a third of a beat off it (when 3 or more remain).
    // minTaps: the fewest taps that give a tempo (2 or more).
    inline TapFit fitTaps (const double* times, int count, int minTaps = 3)
    {
        TapFit r;
        r.taps = count;
        if (count < std::max (2, minTaps)) return r;
        std::vector<double> gaps ((size_t) count - 1);
        for (int i = 1; i < count; ++i) gaps[(size_t) i - 1] = times[i] - times[i - 1];
        std::sort (gaps.begin(), gaps.end());
        const double beat = gaps[gaps.size() >> 1];
        std::vector<double> n ((size_t) count);
        std::vector<int> all ((size_t) count), kept;
        for (int i = 0; i < count; ++i)
        {
            n[(size_t) i] = detail::roundHalfUp ((times[i] - times[0]) / beat);
            all[(size_t) i] = i;
        }
        auto fit = detail::tapLine (times, n.data(), all.data(), count);
        for (int i = 0; i < count; ++i)
            if (std::abs (times[i] - (fit.a + fit.b * n[(size_t) i])) < fit.b / 3) kept.push_back (i);
        const int k = (int) kept.size();
        if (k >= 3 && k < count) fit = detail::tapLine (times, n.data(), kept.data(), k);
        const std::vector<int>& used = k >= 3 ? kept : all;
        double s = 0.0;
        for (int i : used)
        {
            const double d = times[i] - (fit.a + fit.b * n[(size_t) i]);
            s += d * d;
        }
        r.valid = true;
        r.bpm = 60000.0 / fit.b;
        r.used = (int) used.size();
        r.spreadMs = std::sqrt (s / (double) used.size());
        return r;
    }

    // The same, needing 3 taps.
    inline TapFit tapTempo (const double* times, int count) { return fitTaps (times, count, 3); }

    // A live tap button: tap (ms) with each tap's time (a clock in ms) gives
    // the tempo so far, fitted as fitTaps does through the latest `keep` taps
    // (0: every tap since the last pause), from the second tap on. A gap
    // longer than resetMs starts over.
    class TapTempo
    {
    public:
        explicit TapTempo (double resetAfterMs = 2000.0, int keepTaps = 9) : resetMs (resetAfterMs), keep (keepTaps) {}

        TapFit tap (double ms)
        {
            if (! times.empty() && ms - times.back() > resetMs) times.clear();
            times.push_back (ms);
            if (keep > 0 && (int) times.size() > keep) times.erase (times.begin());
            return fitTaps (times.data(), (int) times.size(), 2);
        }

        void reset() { times.clear(); }
        int count() const { return (int) times.size(); }

    private:
        double resetMs;
        int keep;
        std::vector<double> times;
    };

    // What the tempo finder hears.
    struct TempoEstimate
    {
        bool valid = false;      // false until a few seconds have been heard, and when nothing in them starts (silence)
        double bpm = 0.0;
        double confidence = 0.0; // 0 to 1: how sure it is
        double strength = 0.0;   // how far the winning tempo stands above a typical one
        std::vector<double> others; // half and double the tempo, where they are plausible (40 to 300 BPM)
    };

    // Listens to music and finds its tempo. push() any amount of mono audio at
    // a time; estimate() whenever you like. Allocates as it listens: not for
    // the audio thread.
    class TempoFinder
    {
    public:
        static constexpr int size = 2048;          // the analysis window
        static constexpr double confLow = 2.5;     // strength that is not sure at all
        static constexpr double confHigh = 6.0;    // strength that is sure

        explicit TempoFinder (double sampleRate)
            : sr (sampleRate),
              hop ((int) detail::roundHalfUp (sampleRate / 100)),
              fps (sampleRate / hop),
              win (windows::hannSymmetric (size)),
              fft (size)
        {
        }

        double sampleRate() const { return sr; }
        // Seconds of audio analysed so far
        double seconds() const { return (double) env.size() / fps; }
        // How much new sound starts in each hop (one value per hop)
        const std::vector<double>& envelope() const { return env; }

        void push (const float* x, int n)
        {
            tail.insert (tail.end(), x, x + n);
            size_t pos = 0;
            for (; pos + size <= tail.size(); pos += (size_t) hop) frame (tail.data() + pos);
            tail.erase (tail.begin(), tail.begin() + (std::ptrdiff_t) std::min (pos, tail.size()));
        }

        // The tempo, how sure, and half and double, once 4 seconds are in and
        // something in them starts (no rhythm, such as silence: not valid).
        TempoEstimate estimate (double minBpm = 60, double maxBpm = 200) const
        {
            TempoEstimate out;
            if (seconds() < 4) return out;
            // keep only the starts: take off a running average, and the negative part
            const int n = (int) env.size();
            std::vector<double> e ((size_t) n);
            const int w = (int) detail::roundHalfUp (fps * 0.5);
            double acc = 0.0;
            for (int i = 0; i < n; ++i)
            {
                acc += env[(size_t) i] - (i >= w ? env[(size_t) (i - w)] : 0.0);
                e[(size_t) i] = std::max (0.0, env[(size_t) i] - acc / std::min (i + 1, w));
            }
            // no starts at all: every tempo scores 0, so none is the tempo
            if (std::none_of (e.begin(), e.end(), [] (double v) { return v > 0.0; })) return out;
            // score every spacing on a fine grid, at its best phase, with all the beats of the song
            auto score = [&] (double period)
            {
                double best = 0.0;
                const double phases = std::ceil (period);
                for (int ph = 0; ph < phases; ++ph)
                {
                    double s = 0.0;
                    int c = 0;
                    for (double t = ph; t < n - 1; t += period)
                    {
                        const int i = (int) std::floor (t);
                        const double f = t - i;
                        s += e[(size_t) i] * (1 - f) + e[(size_t) i + 1] * f;
                        ++c;
                    }
                    if (c && s / c > best) best = s / c;
                }
                return best;
            };
            // the beat plus the off-beats between (a real tempo has sound on both; at half the
            // tempo the "off-beats" are the real beats, so that doesn't favour counting it slow)
            auto total = [&] (double bpm)
            {
                const double p = (fps * 60) / bpm;
                return score (p) + score (p / 2);
            };
            // a lean toward the tempos songs are usually counted in (about 90 to 160)
            auto prior = [] (double bpm)
            {
                const double z = std::log2 (bpm / 120) / 0.9;
                return std::exp (-0.5 * (z * z));
            };
            double best = minBpm, bestS = -1.0;
            std::vector<double> all;
            for (double bpm = minBpm; bpm <= maxBpm; bpm += 0.5)
            {
                const double raw = total (bpm);
                const double s = raw * prior (bpm);
                all.push_back (raw);
                if (s > bestS) { bestS = s; best = bpm; }
            }
            // then finer around the winner (the search follows it if it moves)
            for (double bpm = best - 0.5; bpm <= best + 0.5; bpm += 0.02)
            {
                const double s = total (bpm) * prior (bpm);
                if (s > bestS) { bestS = s; best = bpm; }
            }
            // how sure: how far the winner stands above a typical tempo (a steady beat makes one
            // clear peak; random hits or slow chords score about the same at every tempo)
            std::sort (all.begin(), all.end());
            double typical = all.empty() ? 0.0 : all[all.size() >> 1];
            if (typical == 0.0 || typical != typical) typical = 1e-9;
            out.valid = true;
            out.bpm = best;
            out.strength = total (best) / typical;
            out.confidence = std::max (0.0, std::min (1.0, (out.strength - confLow) / (confHigh - confLow)));
            for (double b : { best / 2, best * 2 })
                if (b >= 40 && b <= 300) out.others.push_back (b);
            return out;
        }

    private:
        double sr;
        int hop;
        double fps;
        std::vector<double> win;
        Fft fft;
        std::vector<float> tail;
        std::vector<double> prev, env;
        bool hasPrev = false;

        void frame (const float* x)
        {
            std::vector<double> re (size), im (size, 0.0);
            for (int i = 0; i < size; ++i) re[(size_t) i] = (double) x[i] * win[(size_t) i];
            fft.forward (re.data(), im.data());
            // log magnitudes up to about 10 kHz: louder starts anywhere count, quiet ones still register
            const int bins = (int) std::min ((double) (size / 2), std::ceil ((10000.0 * size) / sr));
            std::vector<double> mag ((size_t) bins, 0.0);
            for (int k = 1; k < bins; ++k) mag[(size_t) k] = std::log (1 + 1000 * std::hypot (re[(size_t) k], im[(size_t) k]));
            double flux = 0.0;
            if (hasPrev)
                for (int k = 1; k < bins; ++k) flux += std::max (0.0, mag[(size_t) k] - prev[(size_t) k]);
            prev.swap (mag);
            hasPrev = true;
            env.push_back (flux);
        }
    };
}
