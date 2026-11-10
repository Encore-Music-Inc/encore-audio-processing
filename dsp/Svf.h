#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace vs
{
    // The topology-preserving (trapezoidal) state-variable filter: a
    // second-order section with low-pass, band-pass and high-pass outputs
    // from the same two states, which stays well behaved when its cut-off
    // moves while it runs.
    //
    //   svfStep              one sample through one section, given its
    //                        coefficients: g = tan (pi fc / fs), k = g + 1/Q
    //                        and h = 1 / (1 + g / Q + g^2). The Linkwitz-Riley
    //                        splits (Crossover.h) are built from it.
    //   StateVariableFilter  a section with its own cut-off, resonance and
    //                        type, per channel, number for number as JUCE's
    //                        StateVariableTPTFilter (the de-esser's split).
    template <typename T> struct SvfOutputs { T lowPass, bandPass, highPass; };

    // s: the section's two states (s[0], s[1]), updated
    template <typename T>
    inline SvfOutputs<T> svfStep (T x, T* s, T g, T k, T h)
    {
        const T hp = (x - k * s[0] - s[1]) * h;
        const T bp = g * hp + s[0];
        s[0] = g * hp + bp;
        const T lp = g * bp + s[1];
        s[1] = g * bp + lp;
        return { lp, bp, hp };
    }

    template <typename T>
    class StateVariableFilter
    {
    public:
        enum class Type { lowPass, bandPass, highPass };

        StateVariableFilter() { update(); }

        // Allocates; call it off the audio thread. Clears the states.
        void prepare (double sampleRate, int numChannels)
        {
            fs = sampleRate;
            state.assign ((size_t) numChannels * 2, T (0));
            update();
        }

        void reset() { std::fill (state.begin(), state.end(), T (0)); }

        void setType (Type t) { type = t; }
        // Below fs / 2
        void setCutoffFrequency (T hz) { cutoff = hz; update(); }
        // 1 / sqrt (2) is the Butterworth (12 dB per octave) response
        void setResonance (T q) { resonance = q; update(); }

        T processSample (int channel, T x)
        {
            const auto o = svfStep (x, state.data() + 2 * channel, g, g + r2, h);
            return type == Type::highPass ? o.highPass : type == Type::bandPass ? o.bandPass : o.lowPass;
        }

    private:
        void update()
        {
            g = static_cast<T> (std::tan (pi * cutoff / fs));
            r2 = static_cast<T> (1.0 / resonance);
            h = static_cast<T> (1.0 / (1.0 + r2 * g + g * g));
        }

        static constexpr double pi = 3.141592653589793238;
        double fs = 44100.0;
        T cutoff = static_cast<T> (1000.0), resonance = static_cast<T> (1.0 / std::sqrt (2.0));
        T g {}, r2 {}, h {};
        Type type = Type::lowPass;
        std::vector<T> state = std::vector<T> (4, T (0));
    };
}
