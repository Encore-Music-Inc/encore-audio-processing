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

    // Rounds to the nearest whole number, halves upward (-2.5 is -2, 2.5 is 3),
    // as JavaScript's Math.round does. std::round takes halves away from zero.
    inline double roundHalfUp (double v)
    {
        const double f = std::floor (v);
        return v - f >= 0.5 ? f + 1.0 : f;
    }
}
