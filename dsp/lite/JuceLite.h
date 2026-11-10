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
#include "../Ramp.h"

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

    // JUCE's SmoothedValue, as vs::Ramp (the same numbers): a ramp toward
    // a target over a set time, linear or in equal ratios.
    template <typename T, typename Type = ValueSmoothingTypes::Linear>
    class SmoothedValue
    {
        static_assert (std::is_same_v<T, float>, "the browser build smooths floats");

    public:
        void reset (double sampleRate, double seconds) { ramp.reset (sampleRate, seconds); }
        void setCurrentAndTargetValue (T v) { ramp.setCurrentAndTarget (v); }
        void setTargetValue (T v) { ramp.setTarget (v); }
        T getNextValue() { return ramp.next(); }
        T skip (int n) { return ramp.skip (n); }
        bool isSmoothing() const { return ramp.isRamping(); }
        T getTargetValue() const { return ramp.getTarget(); }
        T getCurrentValue() const { return ramp.getCurrent(); }

    private:
        vs::Ramp<std::is_same_v<Type, ValueSmoothingTypes::Multiplicative>> ramp;
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
        // What a processor is prepared for
        struct ProcessSpec
        {
            double sampleRate;
            unsigned int maximumBlockSize;
            unsigned int numChannels;
        };

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
