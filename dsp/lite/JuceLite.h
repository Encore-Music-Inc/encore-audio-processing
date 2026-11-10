#pragma once

// Minimal stand-ins for the handful of JUCE utilities the ENCORE. audio
// processing core uses, so the same code compiles without JUCE (e.g. to WebAssembly
// for the browser). Written from scratch for this project; only the names
// match, so the headers don't need to change. Enabled with VS_NO_JUCE.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace juce
{
    using int64 = std::int64_t;

    template <typename T> struct MathConstants
    {
        static constexpr T pi = static_cast<T> (3.141592653589793238);
        static constexpr T twoPi = static_cast<T> (6.283185307179586477);
    };

    template <typename T> constexpr T jlimit (T lo, T hi, T v) { return v < lo ? lo : (v > hi ? hi : v); }
    template <typename... T> void ignoreUnused (const T&...) {}

    namespace ValueSmoothingTypes { struct Linear {}; struct Multiplicative {}; }

    // Ramps toward a target over a set time (linear, or in equal ratios)
    template <typename T, typename Type = ValueSmoothingTypes::Linear>
    class SmoothedValue
    {
    public:
        void reset (double sampleRate, double seconds) { steps = std::max (1, (int) std::floor (sampleRate * seconds)); setCurrentAndTargetValue (target); }
        void setCurrentAndTargetValue (T v) { current = target = v; countdown = 0; }
        void setTargetValue (T v)
        {
            if (v == target) return;
            target = v;
            countdown = steps;
            if constexpr (std::is_same_v<Type, ValueSmoothingTypes::Multiplicative>)
                step = std::exp ((std::log (std::abs (target) + T (1e-12)) - std::log (std::abs (current) + T (1e-12))) / (T) countdown);
            else
                step = (target - current) / (T) countdown;
        }
        T getNextValue()
        {
            if (countdown <= 0) return target;
            --countdown;
            if constexpr (std::is_same_v<Type, ValueSmoothingTypes::Multiplicative>) current *= step; else current += step;
            if (countdown == 0) current = target;
            return current;
        }
        void skip (int n) { for (int i = 0; i < n && countdown > 0; ++i) getNextValue(); }
        bool isSmoothing() const { return countdown > 0; }
        T getTargetValue() const { return target; }

    private:
        // like JUCE: a multiplicative ramp starts at 1 (from 0 it could never move, and would sit at 0 until the countdown ends)
        static constexpr T start = std::is_same_v<Type, ValueSmoothingTypes::Multiplicative> ? T (1) : T (0);
        T current = start, target = start, step = T (0);
        int steps = 1, countdown = 0;
    };

    template <typename T>
    class AudioBuffer
    {
    public:
        AudioBuffer() = default;
        AudioBuffer (int ch, int n) { setSize (ch, n); }
        void setSize (int ch, int n, bool = false, bool = false, bool = false)
        {
            data.assign ((size_t) ch, std::vector<T> ((size_t) n, T (0)));
            ptrs.clear();
            for (auto& d : data) ptrs.push_back (d.data());
            numSamples = n;
        }
        int getNumSamples() const { return numSamples; }
        int getNumChannels() const { return (int) data.size(); }
        T* getWritePointer (int c) { return data[(size_t) c].data(); }
        const T* getReadPointer (int c) const { return data[(size_t) c].data(); }
        T* const* getArrayOfWritePointers() { return ptrs.data(); }
        void copyFrom (int c, int start, const T* src, int n) { std::memcpy (data[(size_t) c].data() + start, src, sizeof (T) * (size_t) n); }

    private:
        std::vector<std::vector<T>> data;
        std::vector<T*> ptrs;
        int numSamples = 0;
    };

    namespace dsp
    {
        template <typename T>
        class AudioBlock
        {
        public:
            AudioBlock() = default;
            AudioBlock (T* const* channels, size_t numCh, size_t n) : numChannels (numCh), numSamples (n)
            {
                for (size_t c = 0; c < numCh && c < 8; ++c) ch[c] = channels[c];
            }
            size_t getNumChannels() const { return numChannels; }
            size_t getNumSamples() const { return numSamples; }
            T* getChannelPointer (size_t c) const { return ch[c]; }

        private:
            T* ch[8] {};
            size_t numChannels = 0, numSamples = 0;
        };
    }
}
