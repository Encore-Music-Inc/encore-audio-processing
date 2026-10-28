#pragma once

#include <cmath>

namespace vs
{
    // Coefficient for a one-pole smoother of the form  y += c * (x - y),
    // reaching ~63% of a step after timeMs.
    inline float onePole (float timeMs, double sampleRate)
    {
        const auto samples = (double) timeMs * 0.001 * sampleRate;
        return samples <= 1.0 ? 1.0f : (float) (1.0 - std::exp (-1.0 / samples));
    }

    inline float dbToGain (float db)      { return std::pow (10.0f, db * 0.05f); }
    inline float powerToDb (float power)  { return 10.0f * std::log10 (power + 1.0e-12f); }
    inline float ampToDb (float amp)      { return 20.0f * std::log10 (amp + 1.0e-9f); }
}
