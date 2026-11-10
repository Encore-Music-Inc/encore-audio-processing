#pragma once

#include <algorithm>
#include <cmath>
#include <vector>
#include "Util.h"
#include "Windows.h"

namespace vs
{
    // A fixed delay per channel, in samples (fractional, and may be negative),
    // as a normalised windowed-sinc FIR (the shared SincTable, 128 taps),
    // streaming. Every channel comes out latency() samples late on top of its
    // own delay, and that latency includes the look-ahead a fractional delay's
    // kernel needs, so a caller trims latency() samples from the start and
    // feeds that many more at the end. A whole-number delay is a plain copy (no
    // filtering). For lining up channels (a tape head's azimuth, a room mic).
    class FracDelay
    {
    public:
        explicit FracDelay (const std::vector<double>& delays)
        {
            const SincTable& k = sincTable();
            const int half = k.halfWidth();
            // Late enough for every channel to be causal: a negative delay needs that many samples, and
            // a fractional one's kernel reaches half - 1 samples ahead of floor (delay). With integer
            // latency L, floor (L + d) = L + floor (d), so L >= half - 1 - floor (d) keeps its lo >= 0.
            auto isWhole = [] (double d) { return std::abs (d - roundHalfUp (d)) < 1e-6; };
            lat = 0;
            for (double d : delays)
                lat = std::max (lat, (int) (isWhole (d) ? std::ceil (-d - 1e-6) : half - 1 - std::floor (d)));

            for (double d : delays)
            {
                Channel c;
                const double total = lat + d, whole = roundHalfUp (total);
                if (std::abs (total - whole) < 1e-6)
                {
                    c.shift = (int) whole;
                    c.n = c.shift + 1;
                }
                else
                {
                    // y[n] = sum_j h[j] x[n - j - lo], kernel centred on total
                    c.lo = (int) std::floor (total) - half + 1;
                    const int len = 2 * half;
                    c.h.resize ((size_t) len);
                    double s = 0.0;
                    for (int j = 0; j < len; ++j) { c.h[(size_t) j] = k.at (c.lo + j - total); s += c.h[(size_t) j]; }
                    for (auto& v : c.h) v /= s;
                    c.n = std::max (1, c.lo + len);
                }
                c.hist.assign ((size_t) (c.n + 2 * half + 2), 0.0);
                chans.push_back (std::move (c));
            }
        }

        int latency() const { return lat; }
        int channels() const { return (int) chans.size(); }

        // Delays n samples of channel ch in place.
        template <typename T>
        void process (int ch, T* x, int n)
        {
            auto& c = chans[(size_t) ch];
            const int H = (int) c.hist.size();
            ext.resize ((size_t) (H + n));
            std::copy (c.hist.begin(), c.hist.end(), ext.begin());
            for (int i = 0; i < n; ++i) ext[(size_t) (H + i)] = (double) x[i];
            if (c.h.empty())
            {
                for (int i = 0; i < n; ++i) x[i] = (T) ext[(size_t) (H + i - c.shift)];
            }
            else
            {
                const int len = (int) c.h.size();
                for (int i = 0; i < n; ++i)
                {
                    double s = 0.0;
                    const int b = H + i - c.lo;
                    for (int j = 0; j < len; ++j) s += c.h[(size_t) j] * ext[(size_t) (b - j)];
                    x[i] = (T) s;
                }
            }
            std::copy (ext.end() - H, ext.end(), c.hist.begin());
        }

    private:
        struct Channel
        {
            int shift = 0, lo = 0, n = 1;
            std::vector<double> h, hist;
        };

        int lat = 0;
        std::vector<Channel> chans;
        std::vector<double> ext;
    };
}
