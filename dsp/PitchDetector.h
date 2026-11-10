#pragma once

#include <algorithm>
#include <cmath>
#include <vector>
#include "Fft.h"

namespace vs
{
    // YIN monophonic pitch detector (de Cheveigne & Kawahara), with the
    // difference function computed via FFT so it's cheap enough to run every
    // few milliseconds. Tuned for voice: 80 Hz to 1200 Hz.
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
            tauMax = std::min (n / 2, (int) (fs / minHz));
            tauMin = std::max (2, (int) (fs / maxHz));
            w = n - tauMax;

            const int m = n * 2;
            fft.setSize (m);
            a.assign ((size_t) m, 0.0);
            b.assign ((size_t) m, 0.0);
            aRe.assign ((size_t) m / 2 + 1, 0.0); aIm.assign ((size_t) m / 2 + 1, 0.0);
            bRe.assign ((size_t) m / 2 + 1, 0.0); bIm.assign ((size_t) m / 2 + 1, 0.0);
            d.assign ((size_t) tauMax + 2, 0.0f);
            prefix.assign ((size_t) n + 1, 0.0);
        }

        int windowSize() const { return n; }
        float maxPeriod() const { return (float) tauMax; }
        float minPeriod() const { return (float) tauMin; }

        // x holds the windowSize() most recent samples, oldest first.
        Result analyse (const float* x)
        {
            Result r;

            prefix[0] = 0.0;
            for (int i = 0; i < n; ++i)
                prefix[(size_t) i + 1] = prefix[(size_t) i] + (double) x[i] * x[i];

            const double e0 = prefix[(size_t) w];
            if (e0 / w < 1.0e-6) // below ~ -60 dBFS: silence, not a note
                return r;

            const int m = n * 2;
            std::fill (a.begin(), a.end(), 0.0);
            std::fill (b.begin(), b.end(), 0.0);
            std::copy (x, x + w, a.begin());
            std::copy (x, x + n, b.begin());

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

            // cumulative mean normalised difference
            d[0] = 1.0f;
            double running = 0.0;
            for (int tau = 1; tau <= tauMax; ++tau)
            {
                const double et = prefix[(size_t) (tau + w)] - prefix[(size_t) tau];
                const double diff = std::max (0.0, e0 + et - 2.0 * (double) a[(size_t) tau]);
                running += diff;
                d[(size_t) tau] = running > 0.0 ? (float) (diff * tau / running) : 1.0f;
            }

            int best = -1;
            for (int tau = tauMin; tau < tauMax; ++tau)
            {
                if (d[(size_t) tau] < threshold)
                {
                    while (tau + 1 < tauMax && d[(size_t) tau + 1] < d[(size_t) tau])
                        ++tau;
                    best = tau;
                    break;
                }
            }

            if (best < 0)
            {
                // no clear dip: accept the global minimum only if it's decent
                int minTau = tauMin;
                for (int tau = tauMin; tau < tauMax; ++tau)
                    if (d[(size_t) tau] < d[(size_t) minTau]) minTau = tau;
                if (d[(size_t) minTau] > 0.3f)
                    return r;
                best = minTau;
            }

            // parabolic interpolation for sub-sample accuracy
            float tauF = (float) best;
            if (best > 1 && best < tauMax)
            {
                const float y0 = d[(size_t) best - 1], y1 = d[(size_t) best], y2 = d[(size_t) best + 1];
                const float denom = y0 - 2.0f * y1 + y2;
                if (std::abs (denom) > 1.0e-9f)
                    tauF += 0.5f * (y0 - y2) / denom;
            }

            r.voiced = true;
            r.f0 = (float) fs / tauF;
            r.confidence = 1.0f - d[(size_t) best];
            return r;
        }

    private:
        static constexpr double minHz = 80.0, maxHz = 1200.0;
        static constexpr float threshold = 0.15f;

        double fs = 44100.0;
        int n = 2048, w = 1024, tauMin = 2, tauMax = 512;
        RealFft fft;
        std::vector<double> a, b, aRe, aIm, bRe, bIm;
        std::vector<float> d;
        std::vector<double> prefix;
    };
}
