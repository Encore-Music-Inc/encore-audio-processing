#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace vs
{
    struct Resonance { float freq = 0.0f, prominenceDb = 0.0f, relDb = 0.0f; };

    // Finds resonances in a long-term average power spectrum: narrow bumps
    // that stand well above their surroundings (ringing drums, boxy guitars,
    // a harsh room mode, a whistly vocal frequency).
    //   power: linear power per FFT bin (fftSize/2 + 1 values)
    //   prominenceDb: how far the bump stands above the 2/3-octave average
    //   relDb: level of a Q~5 band at that frequency relative to the whole
    //          signal (what a smart band's threshold compares against)
    inline std::vector<Resonance> findResonances (const std::vector<float>& power, double fs, int fftSize,
                                                  int maxResults = 5, float minProminenceDb = 4.0f)
    {
        const size_t nb = power.size();
        std::vector<double> prefix (nb + 1, 0.0);
        for (size_t b = 0; b < nb; ++b) prefix[b + 1] = prefix[b] + power[b];
        const double total = prefix[nb];
        if (total < 1e-12) return {};

        auto binFor = [&] (float f) { return std::clamp ((int) (f / (float) fs * (float) fftSize), 1, (int) nb - 1); };
        auto avgDb = [&] (float f, float octaves)
        {
            const int b1 = binFor (f * std::pow (2.0f, -octaves * 0.5f));
            const int b2 = std::max (b1, binFor (f * std::pow (2.0f, octaves * 0.5f)));
            return 10.0 * std::log10 ((prefix[(size_t) b2 + 1] - prefix[(size_t) b1]) / (b2 - b1 + 1) + 1e-20);
        };

        constexpr int points = 220;
        std::vector<float> freqs (points), prom (points), level (points);
        double maxLevel = -300.0;
        for (int i = 0; i < points; ++i)
        {
            const float f = 80.0f * std::pow (12000.0f / 80.0f, (float) i / (points - 1));
            const double narrow = avgDb (f, 1.0f / 12.0f), wide = avgDb (f, 2.0f / 3.0f);
            freqs[(size_t) i] = f;
            prom[(size_t) i] = (float) (narrow - wide);
            level[(size_t) i] = (float) narrow;
            maxLevel = std::max (maxLevel, narrow);
        }

        std::vector<Resonance> found;
        for (int i = 1; i < points - 1; ++i)
        {
            const float p = prom[(size_t) i];
            if (p < minProminenceDb || p < prom[(size_t) i - 1] || p < prom[(size_t) i + 1]) continue;
            if (level[(size_t) i] < maxLevel - 45.0) continue; // ignore the noise floor
            Resonance r;
            r.freq = freqs[(size_t) i];
            r.prominenceDb = p;
            const int b1 = binFor (r.freq * 0.9f), b2 = std::max (b1, binFor (r.freq * 1.1f));
            r.relDb = (float) (10.0 * std::log10 ((prefix[(size_t) b2 + 1] - prefix[(size_t) b1]) / total + 1e-20));
            found.push_back (r);
        }
        std::sort (found.begin(), found.end(), [] (const Resonance& a, const Resonance& b) { return a.prominenceDb > b.prominenceDb; });

        std::vector<Resonance> keep;
        for (auto& r : found)
        {
            bool close = false;
            for (auto& k : keep) close = close || std::abs (std::log2 (r.freq / k.freq)) < 1.0f / 3.0f;
            if (! close) keep.push_back (r);
            if ((int) keep.size() == maxResults) break;
        }
        return keep;
    }
}
