// ENCORE. Audio Processing
// Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
//
// Tuning drums. A drum's hit isn't one clean pitch (its overtones don't line
// up and it dies away fast), so instead of following a note, each hit is
// caught and the main peak of its spectrum measured (drumPeak). rodSpread
// compares the pitches measured at each tension rod; tomSteps gives the notes
// for a set of toms tuned in steps. Plain C++ with no JUCE, so the browser
// (master.wasm) measures the same way.
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include "Fft.h"
#include "Windows.h"

namespace vs::drums
{
    // The main note of a drum hit, in Hz (0 when there isn't one, or it's
    // quieter than about -60 dB). x[0..n): the sound just after the hit; its
    // first `size` samples (a power of two) are Hann windowed and transformed.
    // The strongest peak between minHz and maxHz is taken, or with `lowest`
    // the lowest clear peak (within 10 dB of the strongest): that's the drum's
    // own note when it's hit in the middle, where the strongest peak can be an
    // overtone. The peak's frequency is refined with a parabola through the
    // log power of its bin and its neighbours.
    inline double drumPeak (const float* x, int n, double sampleRate, double minHz, double maxHz, bool lowest = false, int size = 32768)
    {
        const int len = std::min (n, size);
        if (len < 2) return 0.0;
        std::vector<double> re ((size_t) size, 0.0), im ((size_t) size, 0.0);
        const auto window = windows::hannSymmetric (len);
        double energy = 0.0;
        for (int i = 0; i < len; ++i)
        {
            re[(size_t) i] = x[i] * window[(size_t) i];
            energy += (double) x[i] * x[i];
        }
        if (energy / len < 1e-6) return 0.0;
        fftFor (size).forward (re.data(), im.data());
        const int lo = std::max (2, (int) std::floor ((minHz * size) / sampleRate));
        const int hi = std::min (size / 2 - 2, (int) std::ceil ((maxHz * size) / sampleRate));
        auto mag = [&] (int k) { return std::log (re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k] + 1e-20); };
        int best = lo;
        for (int k = lo; k <= hi; ++k) if (mag (k) > mag (best)) best = k;
        if (lowest)
        {
            const double floor = mag (best) - std::log (10.0); // 10 dB under the strongest, in power
            for (int k = lo + 1; k < best; ++k)
                if (mag (k) > floor && mag (k) >= mag (k - 1) && mag (k) >= mag (k + 1)) { best = k; break; }
        }
        const double a = mag (best - 1), b = mag (best), c = mag (best + 1);
        const double denom = a - 2 * b + c;
        const double shift = std::abs (denom) > 1e-12 ? (0.5 * (a - c)) / denom : 0.0;
        return ((best + shift) * sampleRate) / size;
    }

    // How far each rod's pitch is from the average of those measured: the
    // geometric mean (Hz, 0 when none is measured) and each rod's cents from it
    // (NaN where hz isn't above 0, not measured yet).
    struct RodSpread
    {
        double mean = 0.0;
        std::vector<double> cents;
    };

    inline RodSpread rodSpread (const std::vector<double>& hz)
    {
        RodSpread r;
        double sum = 0.0;
        int count = 0;
        for (double h : hz) if (h > 0) { sum += std::log2 (h); ++count; }
        if (count > 0) r.mean = std::pow (2.0, sum / count);
        for (double h : hz)
            r.cents.push_back (count > 0 && h > 0 ? 1200 * std::log2 (h / r.mean) : std::numeric_limits<double>::quiet_NaN());
        return r;
    }

    // Notes for toms tuned in steps: the highest tom's note, then each one `step` semitones lower.
    inline std::vector<double> tomSteps (double top, int count, double step)
    {
        std::vector<double> notes;
        for (int i = 0; i < count; ++i) notes.push_back (top - i * step);
        return notes;
    }
}
