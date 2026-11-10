// ENCORE. Audio Processing
// Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
//
// Finds note onsets in a live stream of input (a mic, an instrument), and
// where each one starts to a fraction of a sample. Used to check how well
// someone plays in time and to measure the round trip from the speakers to
// the mic.
//
// Detection runs in short hops (sampleRate / 750, at least 16 samples), on
// either of two signs of a new note:
//   - a high-frequency-weighted energy (x^2 + 4 dx^2: attacks are bright)
//     jumping `ratio` times above its level 10 to 50 ms before (drums, claps,
//     anything after a gap);
//   - the same jump in what a linear predictor (refitted every 8 hops on the
//     last 30 ms) could not see coming: a new note while the last one still
//     rings barely changes the level, but it isn't predictable from what was
//     ringing.
// The level must be above the noise floor (the quietest it gets, creeping up
// slowly) and a fixed floor, the last onset's level must have fallen back by
// a quarter, and a refractory gap must have passed. Then refine() finds the
// start in the samples around it, to a fraction of a sample.
//
// Plain C++ with no JUCE. push() allocates nothing once made, except the
// list of found onsets and a predictor's working arrays.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>
#include "dsp/Lpc.h"
#include "dsp/Windows.h"
#include "dsp/Util.h"

namespace vs
{
    class OnsetDetector
    {
    public:
        // sensitivity: 0 to 100. refractoryMs: the shortest time between two onsets.
        explicit OnsetDetector (double sampleRate, double sensitivity = 50, double refractoryMs = 45)
            : sr (sampleRate)
        {
            const double s = std::min (100.0, std::max (0.0, sensitivity)) / 100;
            ratio = 5 - 3.5 * s;                      // 5x (7 dB) at 0, 1.5x at 100
            absMin = std::pow (10.0, -(5.5 + 2 * s)); // energy below about -55 to -75 dB is never a note
            hop = std::max (16, (int) roundHalfUp (sampleRate / 750));
            refractory = (std::int64_t) roundHalfUp ((sampleRate * refractoryMs) / 1000);
            size = 1 << (int) std::ceil (std::log2 (sampleRate * 0.5));
            mask = size - 1;
            x.assign ((size_t) size, 0.0f);
            smooth = std::max (4, (int) roundHalfUp (sampleRate * 0.0005));
            pre = (int) roundHalfUp (sampleRate * 0.025);
            post = (int) roundHalfUp (sampleRate * 0.025);
            base = (int) roundHalfUp (sampleRate * 0.01);
            fitLength = (int) roundHalfUp (sampleRate * 0.03);
        }

        // Feeds n samples whose first is at sample frame `frame` (a running
        // count of whole frames; a jump forward is a dropout, carried over).
        // Appends to `found` each onset whose surroundings are now known, as
        // a fractional frame on the same count.
        void push (const float* in, int n, std::int64_t frame, std::vector<double>& found)
        {
            if (! hasOrigin) { origin = frame; hasOrigin = true; }
            const std::int64_t at = frame - origin;
            if (at > count) count = at;
            for (int i = 0; i < n; ++i)
            {
                const double v = in[i];
                const double d = v - prev;
                prev = v;
                const double e = v * v + 4 * d * d;
                x[(size_t) (count & mask)] = in[i];
                acc += e;
                ++count;
                if (++accN == hop)
                {
                    hopDone();
                    acc = 0.0;
                    accN = 0;
                }
            }
            while (! pending.empty() && count >= pending.front() + post + smooth)
            {
                const std::int64_t t = pending.front();
                pending.pop_front();
                found.push_back ((double) origin + refine (t));
            }
        }

        double sampleRate() const { return sr; }
        int hopSize() const { return hop; }

        // Where the note that tripped detection at frame t (counted from the
        // first frame pushed) really starts. A linear predictor is fitted to
        // the 30 ms before it, so whatever was already ringing (the last
        // chord, a held note) is predicted away and what's left (the
        // residual) jumps exactly where something new starts, even a soft
        // note under a loud one. t itself when the stretch is no longer held.
        double refine (std::int64_t t) const
        {
            constexpr int P = 24;
            const std::int64_t ws = t - pre, we = t + post;
            const std::int64_t f0 = ws - fitLength;
            const int L = smooth, half = L >> 1;
            const std::int64_t b0 = ws - base, lo = b0 - half, hi = we + half;
            const std::int64_t oldest = std::max<std::int64_t> (0, count - size + 1);
            if (f0 - P < oldest) return (double) t;
            double fit[P + 1];
            predictor (f0, ws, P, fit);
            // the residual's energy, smoothed over L samples (centred)
            const std::int64_t len = hi - lo + 1;
            std::vector<double> cum ((size_t) len + 1, 0.0);
            for (std::int64_t i = 0; i < len; ++i)
            {
                double v = X (lo + i);
                for (int k = 1; k <= P; ++k) v += fit[k] * X (lo + i - k);
                cum[(size_t) i + 1] = cum[(size_t) i] + v * v;
            }
            auto s = [&] (std::int64_t i)
            {
                const std::int64_t p = std::max (lo, i - half);
                const std::int64_t q = std::min (hi, i - half + L);
                return (cum[(size_t) (q - lo)] - cum[(size_t) (p - lo)]) / (double) std::max<std::int64_t> (1, q - p);
            };
            const double baseLevel = (cum[(size_t) (ws - lo)] - cum[(size_t) (b0 - lo)]) / (double) std::max<std::int64_t> (1, ws - b0);
            // the note's level: its loudest 5 ms (not one spike, where a chord's strings happen to line up)
            const int W = (int) roundHalfUp (sr * 0.005);
            double peak = -1.0, level = 0.0;
            std::int64_t ip = t;
            for (std::int64_t i = ws; i <= we; ++i)
            {
                const double v = s (i);
                if (v > peak) { peak = v; ip = i; }
            }
            for (std::int64_t i = ws; i + W <= hi; i += 16)
                level = std::max (level, (cum[(size_t) (i + W - lo)] - cum[(size_t) (i - lo)]) / W);
            const double th = std::max (baseLevel * 3, baseLevel + 0.04 * (std::min (peak, level) - baseLevel));
            if (! (peak > th)) return (double) t;
            // back from the peak, over short dips (a clap's bursts), to the last sample above the threshold
            const int maxGap = (int) roundHalfUp (sr * 0.004);
            std::int64_t on = ip;
            int gap = 0;
            for (std::int64_t i = ip; i > ws; --i)
            {
                if (s (i) >= th) { on = i; gap = 0; }
                else if (++gap > maxGap) break;
            }
            const double y0 = s (on - 1), y1 = s (on);
            return (double) (on - 1) + (y1 > y0 ? std::min (1.0, std::max (0.0, (th - y0) / (y1 - y0))) : 1.0);
        }

    private:

        static constexpr int order = 20;   // the detection predictor's order
        double sr;
        double ratio = 0, absMin = 0;
        int hop = 16, size = 0, smooth = 4, pre = 0, post = 0, base = 0, fitLength = 0;
        std::int64_t mask = 0, refractory = 0;
        std::vector<float> x;              // the last `size` samples, a ring
        double hops[64] {};                // energy per hop, a ring
        double res[64] {};                 // the predictor's miss (residual energy) per hop, a ring
        double a[order + 1] {};
        bool haveA = false;
        double nf = std::numeric_limits<double>::infinity();
        double nf2 = std::numeric_limits<double>::infinity();
        bool armed = true;
        double peakSince = 0;
        std::int64_t hopCount = 0;        // hops done
        double acc = 0;
        int accN = 0;
        std::int64_t count = 0;            // frames taken in, counted from the first frame pushed
        double prev = 0;
        std::int64_t origin = 0;
        bool hasOrigin = false;
        std::int64_t lastTrig = -1000000000000LL;
        std::deque<std::int64_t> pending;

        double X (std::int64_t i) const { return x[(size_t) (i & mask)]; }

        // A prediction-error filter of order p (a[0] = 1) for frames [f0, f1):
        // the autocorrelation of the stretch under a centred Hann window, then Levinson-Durbin.
        void predictor (std::int64_t f0, std::int64_t f1, int p, double* out) const
        {
            const int N = (int) (f1 - f0);
            std::vector<double> xw = windows::hannCentred (N);
            for (int i = 0; i < N; ++i) xw[(size_t) i] = X (f0 + i) * xw[(size_t) i];
            double r[32];
            lpc::autocorrelation (xw.data(), N, p, r);
            lpc::errorFilter (r, p, out);
        }

        void hopDone()
        {
            const std::int64_t kk = hopCount++;
            const std::int64_t n = count;
            const int slot = (int) (kk & 63);
            hops[slot] = acc / hop;
            // the predictor's miss over this hop (with the coefficients fitted before it)
            double miss = 0.0;
            if (haveA)
            {
                for (std::int64_t i = n - hop; i < n; ++i)
                {
                    double v = X (i);
                    for (int j = 1; j <= order; ++j) v += a[j] * X (i - j);
                    miss += v * v;
                }
                miss /= hop;
            }
            else miss = hops[slot];
            res[slot] = miss;
            if (kk % 8 == 0 && (double) n > sr * 0.03 + order)
            {
                predictor (n - fitLength, n, order, a);
                haveA = true;
            }
            auto avg = [kk] (const double* arr, int from, int to)
            {
                double s = 0.0;
                int c = 0;
                for (int j = from; j <= to; ++j)
                    if (kk - j >= 0) { s += arr[(kk - j) & 63]; ++c; }
                return c ? s / c : 0.0;
            };
            const double S = avg (hops, 0, 3), R = avg (hops, 8, 38);
            const double S2 = avg (res, 0, 3), R2 = avg (res, 8, 38);
            nf = S < nf ? S : nf * 1.00004;   // the quietest it gets, creeping up slowly
            nf2 = S2 < nf2 ? S2 : nf2 * 1.00004;
            const double floor = std::max (absMin, nf * 5);
            // (the predictor path also wants the level rising a little: a sound dying away
            // while it bends in pitch is not a new note)
            const bool jump = S > ratio * R || (S2 > ratio * 2 * R2 && S2 > nf2 * 5 && S > 1.1 * R);
            // after a note, wait for its level to stop climbing before the next (a slow swell is one note)
            if (S > peakSince) peakSince = S;
            else if (S < peakSince * 0.75) armed = true;
            if (jump && S > floor && n - lastTrig > refractory && armed)
            {
                lastTrig = n;
                armed = false;
                peakSince = S;
                pending.push_back (n);
            }
        }
    };
}
