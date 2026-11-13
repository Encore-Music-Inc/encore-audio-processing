#pragma once

#include <algorithm>
#include <cmath>
#include <vector>
#include "Util.h"

namespace vs
{
    // Window functions and the windowed-sinc kernel, in double precision.
    // Each fills a vector of n values for i = 0..n-1.
    namespace windows
    {
        constexpr double pi = 3.141592653589793;

        // Periodic Hann, 0.5 - 0.5 cos (2 pi i / n): overlapped by half, the windows sum flat.
        inline std::vector<double> hannPeriodic (int n)
        {
            std::vector<double> w ((size_t) n);
            for (int i = 0; i < n; ++i) w[(size_t) i] = 0.5 - 0.5 * std::cos ((2 * pi * i) / n);
            return w;
        }

        // Periodic Hann worked out in float, for i = 0..count - 1 (count
        // defaults to n; n + 1 adds the closing zero, for a table read between
        // its points). The live processors window their FFT frames with it.
        inline std::vector<float> hannPeriodicFloat (int n, int count = -1)
        {
            constexpr float twoPi = (float) (2 * 3.141592653589793238L);
            std::vector<float> w ((size_t) (count < 0 ? n : count));
            for (size_t i = 0; i < w.size(); ++i) w[i] = 0.5f - 0.5f * std::cos (twoPi * (float) i / (float) n);
            return w;
        }

        // Symmetric Hann, 0.5 - 0.5 cos (2 pi i / (n - 1)): zero at both ends.
        inline std::vector<double> hannSymmetric (int n)
        {
            std::vector<double> w ((size_t) n, 1.0);
            if (n < 2) return w;
            for (int i = 0; i < n; ++i) w[(size_t) i] = 0.5 - 0.5 * std::cos ((2 * pi * i) / (n - 1));
            return w;
        }

        // Hann sampled at the middle of each sample, 0.5 - 0.5 cos (2 pi (i + 0.5) / n):
        // never exactly zero, so every sample of a short stretch counts.
        inline std::vector<double> hannCentred (int n)
        {
            std::vector<double> w ((size_t) n);
            for (int i = 0; i < n; ++i) w[(size_t) i] = 0.5 - 0.5 * std::cos ((2 * pi * (i + 0.5)) / n);
            return w;
        }

        // Tukey: flat in the middle, a half-cosine taper over a fraction `a` of
        // the length (a / 2 at each end). Tukey (0.5) is the FLAC encoder's usual
        // window for linear prediction.
        inline std::vector<double> tukey (int n, double a = 0.5)
        {
            std::vector<double> w ((size_t) n);
            const int edge = (int) std::floor ((a * (n - 1)) / 2);
            for (int i = 0; i < n; ++i)
                w[(size_t) i] = i < edge             ? 0.5 * (1 - std::cos ((pi * i) / edge))
                              : i > n - 1 - edge     ? 0.5 * (1 - std::cos ((pi * (n - 1 - i)) / edge))
                                                     : 1.0;
            return w;
        }

        // Blackman, periodic: a0 - a1 cos (2 pi i / n) + a2 cos (4 pi i / n),
        // a0 = (1 - alpha) / 2, a1 = 1 / 2, a2 = alpha / 2. alpha 0.16 is the
        // classic Blackman, the window the Web Audio AnalyserNode uses.
        inline std::vector<double> blackman (int n, double alpha = 0.16)
        {
            const double a0 = (1.0 - alpha) / 2.0, a1 = 0.5, a2 = alpha / 2.0;
            std::vector<double> w ((size_t) n);
            for (int i = 0; i < n; ++i) w[(size_t) i] = a0 - a1 * std::cos ((2 * pi * i) / n) + a2 * std::cos ((4 * pi * i) / n);
            return w;
        }

        // Four-term Blackman-Harris, symmetric (zero-ish at both ends, about
        // 92 dB side lobes): for long low-pass FIR designs.
        inline double blackmanHarrisAt (int i, int n)
        {
            return 0.35875
                 - 0.48829 * std::cos ((2 * pi * i) / (n - 1))
                 + 0.14128 * std::cos ((4 * pi * i) / (n - 1))
                 - 0.01168 * std::cos ((6 * pi * i) / (n - 1));
        }

        inline std::vector<double> blackmanHarris (int n)
        {
            std::vector<double> w ((size_t) n);
            for (int i = 0; i < n; ++i) w[(size_t) i] = blackmanHarrisAt (i, n);
            return w;
        }

        // The modified Bessel function I0 by its power series, sum of
        // ((x/2)^k / k!)^2. Stops after `terms` terms, or once a term is below
        // `relStop` of the sum (relStop 0: always runs all the terms).
        inline double besselI0 (double x, int terms = 40, double relStop = 1e-14)
        {
            double s = 1.0, t = 1.0;
            for (int k = 1; k < terms; ++k)
            {
                t *= (x / (2 * k)) * (x / (2 * k));
                s += t;
                if (relStop > 0.0 && t < relStop * s) break;
            }
            return s;
        }

        // Kaiser window with shape beta, symmetric: I0 (beta sqrt (1 - r^2)) / I0 (beta),
        // r from -1 to 1 across the window.
        // The Kaiser taper at r (-1 .. 1 across the window), not normalized:
        // I0 (beta sqrt (1 - r^2)). The window, the oversampler's and the
        // sinc table's kernels all use it.
        inline double kaiserTaper (double r, double beta, int terms = 40, double relStop = 1e-14)
        {
            return besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r)), terms, relStop);
        }

        inline std::vector<double> kaiser (int n, double beta)
        {
            std::vector<double> w ((size_t) n, 1.0);
            if (n < 2) return w;
            const double norm = besselI0 (beta);
            for (int i = 0; i < n; ++i)
            {
                const double r = 2.0 * i / (n - 1) - 1.0;
                w[(size_t) i] = kaiserTaper (r, beta) / norm;
            }
            return w;
        }
    }

    // A Kaiser-windowed sinc kernel, tabulated: kernel (t) for |t| below `half`
    // zero crossings, `res` points per crossing, read with linear
    // interpolation. For band-limited interpolation (resampling, fractional delays).
    class SincTable
    {
    public:
        explicit SincTable (int halfCrossings = 64, double beta = 9.5, int resolution = 4096)
            : half (halfCrossings), res (resolution)
        {
            const int n = half * res + 2;
            t.resize ((size_t) n);
            const double i0b = windows::besselI0 (beta);
            for (int i = 0; i < n; ++i)
            {
                const double x = (double) i / res;
                const double sinc = x == 0.0 ? 1.0 : std::sin (windows::pi * x) / (windows::pi * x);
                const double r = x / half;
                t[(size_t) i] = r >= 1.0 ? 0.0 : (sinc * windows::kaiserTaper (r, beta)) / i0b;
            }
        }

        // The kernel at x, in zero crossings.
        double at (double x) const
        {
            const double u = std::abs (x) * res;
            if (! (u < (double) (t.size() - 1))) return 0.0;
            const int i = (int) u;
            const double f = u - i;
            return lerp (t[(size_t) i], t[(size_t) i + 1], f);
        }

        int halfWidth() const { return half; }
        int resolution() const { return res; }
        const std::vector<double>& table() const { return t; }

    private:
        int half, res;
        std::vector<double> t;
    };

    // The one default SincTable (64 crossings each side, beta 9.5, 4096 points
    // per crossing), made the first time it is asked for.
    inline const SincTable& sincTable()
    {
        static const SincTable table;
        return table;
    }
}
