#pragma once

#include <algorithm>
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

    // The same coefficient from a cutoff in Hz (kept to 10 Hz .. 0.45 fs):
    // 1 - exp (-2 pi hz / fs), worked out in float.
    inline float onePoleHz (float hz, double sampleRate)
    {
        constexpr float twoPi = (float) (2 * 3.141592653589793238L);
        const float fsF = (float) sampleRate;
        return 1.0f - std::exp (-twoPi * std::min (std::max (hz, 10.0f), 0.45f * fsF) / fsF);
    }

    // An envelope follower's step: env moves toward x by `up` of the
    // difference when x is above it, by `down` when not.
    inline void followEnvelope (float& env, float x, float up, float down)
    {
        env += (x > env ? up : down) * (x - env);
    }

    // Sample i of every channel averaged (a float sum, then divided by the
    // count): what a detector listens to.
    inline float channelMean (const float* const* ch, int numCh, int i)
    {
        float m = 0.0f;
        for (int c = 0; c < numCh; ++c) m += ch[c][i];
        return m / (float) numCh;
    }

    // The same with an instant rise: env jumps up to x, and falls toward it by
    // `down` of the difference (a peak follower).
    inline void followPeak (float& env, float x, float down)
    {
        env = x > env ? x : env + down * (x - env);
    }

    // The same upside down: env drops to x at once, and rises toward it by
    // `up` of the difference (a gain that cuts at once and recovers).
    inline void followTrough (float& env, float x, float up)
    {
        env = x < env ? x : env + up * (x - env);
    }

    // Decibels. Each takes the small amount added before the logarithm (so
    // silence reads a finite level) as `floor`; 0 gives -inf for silence.
    //   float, for processing:  dbToGain 10^(dB / 20) (as pow (10, dB x 0.05)),
    //                           powerToDb 10 log10, ampToDb 20 log10
    //   double, for measuring:  gainFromDb 10^(dB / 20), db10 10 log10, db20 20 log10
    inline float dbToGain (float db)                                { return std::pow (10.0f, db * 0.05f); }
    inline float powerToDb (float power, float floor = 1.0e-12f)    { return 10.0f * std::log10 (power + floor); }
    inline float ampToDb (float amp, float floor = 1.0e-9f)         { return 20.0f * std::log10 (amp + floor); }

    inline double gainFromDb (double db)                            { return std::pow (10.0, db / 20); }
    inline double db10 (double power, double floor = 1e-20)         { return 10.0 * std::log10 (power + floor); }
    inline double db20 (double amp, double floor = 1e-12)           { return 20.0 * std::log10 (amp + floor); }

    // Straight-line interpolation from a (t = 0) to b (t = 1)
    template <typename T> inline T lerp (T a, T b, T t) { return a + t * (b - a); }

    // Mid and side of a stereo pair, (l + r) / 2 and (l - r) / 2; back to
    // left and right as mid + side and mid - side.
    template <typename T> inline T midOf (T l, T r)  { return T (0.5) * (l + r); }
    template <typename T> inline T sideOf (T l, T r) { return T (0.5) * (l - r); }

    // Rounds to the nearest whole number, halves upward (-2.5 is -2, 2.5 is 3),
    // as JavaScript's Math.round does. std::round takes halves away from zero.
    inline double roundHalfUp (double v)
    {
        const double f = std::floor (v);
        return v - f >= 0.5 ? f + 1.0 : f;
    }
}
