#pragma once

// Minimal stand-ins for the handful of JUCE utilities the Encore DSP
// core uses, so the same DSP code compiles without JUCE (e.g. to WebAssembly
// for the browser). Written from scratch for this project; only the names
// match, so the DSP headers don't need to change. Enabled with VS_NO_JUCE.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace juce
{
    using int64 = std::int64_t;
    using uint32 = std::uint32_t;

    template <typename T> struct MathConstants
    {
        static constexpr T pi = static_cast<T> (3.141592653589793238);
        static constexpr T twoPi = static_cast<T> (6.283185307179586477);
    };

    template <typename T> constexpr T jlimit (T lo, T hi, T v) { return v < lo ? lo : (v > hi ? hi : v); }
    template <typename T> constexpr T jmin (T a, T b) { return a < b ? a : b; }
    template <typename T> constexpr T jmax (T a, T b) { return a > b ? a : b; }
    template <typename... T> void ignoreUnused (const T&...) {}
    template <typename T> bool isPositiveAndBelow (T v, T upper) { return v >= T (0) && v < upper; }

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
        struct ProcessSpec { double sampleRate; uint32 maximumBlockSize; uint32 numChannels; };

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
            T getSample (int c, int i) const { return ch[c][i]; }
            void setSample (int c, int i, T v) const { ch[c][i] = v; }

        private:
            T* ch[8] {};
            size_t numChannels = 0, numSamples = 0;
        };

        enum class LinkwitzRileyFilterType { lowpass, highpass, allpass };

        // 4th-order Linkwitz-Riley = two cascaded 2nd-order Butterworth
        // sections (topology-preserving state variable form). LP + HP sum to
        // an allpass, so a crossover built from these recombines flat.
        template <typename T>
        class LinkwitzRileyFilter
        {
        public:
            void prepare (const ProcessSpec& s) { fs = s.sampleRate; update(); reset(); }
            void reset() { std::memset (st, 0, sizeof (st)); }
            void setType (LinkwitzRileyFilterType t) { type = t; }
            void setCutoffFrequency (T hz) { cutoff = hz; update(); }

            T processSample (int ch, T x)
            {
                if (type == LinkwitzRileyFilterType::allpass)
                    return section (ch, 0, section (ch, 1, x, true), true) + section (ch, 2, section (ch, 3, x, false), false);
                const bool lp = type == LinkwitzRileyFilterType::lowpass;
                return section (ch, 0, section (ch, 1, x, lp), lp);
            }

        private:
            // one Butterworth 2nd-order TPT SVF section; returns LP or HP
            T section (int ch, int k, T x, bool lowpass)
            {
                T* s = st[ch & 1][k];
                const T hp = (x - (T) (2 * R + g) * s[0] - s[1]) * h;
                const T bp = g * hp + s[0]; s[0] = g * hp + bp;
                const T lpv = g * bp + s[1]; s[1] = g * bp + lpv;
                return lowpass ? lpv : hp;
            }
            void update()
            {
                g = (T) std::tan (MathConstants<double>::pi * jlimit (10.0, fs * 0.49, (double) cutoff) / fs);
                h = (T) (1.0 / (1.0 + 2.0 * R * g + g * g));
            }

            static constexpr double R = 0.70710678118654752; // 1/(2Q), Q = 1/sqrt(2)
            double fs = 48000.0;
            T cutoff = T (1000), g = T (0.06), h = T (0.9);
            T st[2][4][2] {};
            LinkwitzRileyFilterType type = LinkwitzRileyFilterType::lowpass;
        };

        // Power-of-two oversampling with linear-phase half-band FIR stages
        // (Kaiser-windowed sinc). Both "types" use the same filters here.
        template <typename T>
        class Oversampling
        {
        public:
            enum FilterType { filterHalfBandFIREquiripple, filterHalfBandPolyphaseIIR };

            Oversampling (size_t numChannels, size_t factorLog2, FilterType, bool = true, bool = false)
                : channels (numChannels), stages (factorLog2)
            {
                // 63-tap half-band low-pass, cutoff fs/4 (of the higher rate):
                // long enough to keep the top octave accurate for true-peak detection
                const int n = taps;
                const double beta = 8.0;
                auto bessel0 = [] (double x) { double s = 1.0, t = 1.0; for (int k = 1; k < 30; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; } return s; };
                for (int i = 0; i < n; ++i)
                {
                    const double m = i - (n - 1) / 2.0;
                    const double sinc = m == 0.0 ? 0.5 : std::sin (MathConstants<double>::pi * m * 0.5) / (MathConstants<double>::pi * m);
                    const double r = 2.0 * i / (n - 1) - 1.0;
                    coeff[i] = (T) (sinc * bessel0 (beta * std::sqrt (1.0 - r * r)) / bessel0 (beta));
                }
            }

            void initProcessing (size_t maxBlock)
            {
                maxIn = maxBlock;
                for (size_t s = 0; s < stages; ++s)
                    buf[s].assign (channels, std::vector<T> (maxBlock << (s + 1), T (0)));
                for (auto& sdata : upState) sdata.assign (channels, std::vector<T> ((size_t) taps, T (0)));
                for (auto& sdata : downState) sdata.assign (channels, std::vector<T> ((size_t) taps, T (0)));
                reset();
            }

            void reset()
            {
                for (auto& sdata : upState) for (auto& c : sdata) std::fill (c.begin(), c.end(), T (0));
                for (auto& sdata : downState) for (auto& c : sdata) std::fill (c.begin(), c.end(), T (0));
            }

            // Each stage's FIR delays by (taps-1)/2 samples at its rate; up + down
            float getLatencyInSamples() const
            {
                double lat = 0.0;
                for (size_t s = 0; s < stages; ++s) lat += 2.0 * ((taps - 1) / 2.0) / (double) (2u << s);
                return (float) lat;
            }

            AudioBlock<T> processSamplesUp (const AudioBlock<T>& in)
            {
                const size_t nch = std::min (channels, in.getNumChannels());
                size_t n = in.getNumSamples();
                for (size_t s = 0; s < stages; ++s)
                {
                    for (size_t c = 0; c < nch; ++c)
                    {
                        const T* src = s == 0 ? in.getChannelPointer (c) : buf[s - 1][c].data();
                        T* dst = buf[s][c].data();
                        auto& z = upState[s][c];
                        for (size_t i = 0; i < n; ++i)
                        {
                            // push x then a zero; gain 2 restores level
                            dst[2 * i] = fir (z, src[i] * T (2));
                            dst[2 * i + 1] = fir (z, T (0));
                        }
                    }
                    n *= 2;
                }
                for (size_t c = 0; c < nch; ++c) ptrs[c] = buf[stages - 1][c].data();
                return AudioBlock<T> (ptrs, nch, n);
            }

            void processSamplesDown (AudioBlock<T>& out)
            {
                const size_t nch = std::min (channels, out.getNumChannels());
                for (size_t s = stages; s-- > 0;)
                {
                    const size_t nHigh = out.getNumSamples() << (s + 1);
                    for (size_t c = 0; c < nch; ++c)
                    {
                        const T* src = buf[s][c].data();
                        T* dst = s == 0 ? out.getChannelPointer (c) : buf[s - 1][c].data();
                        auto& z = downState[s][c];
                        for (size_t i = 0; i < nHigh; i += 2)
                        {
                            const T y = fir (z, src[i]);
                            fir (z, src[i + 1]);
                            dst[i / 2] = y;
                        }
                    }
                }
            }

        private:
            static constexpr int taps = 63;
            T fir (std::vector<T>& z, T x)
            {
                std::memmove (z.data() + 1, z.data(), sizeof (T) * (size_t) (taps - 1));
                z[0] = x;
                T acc = T (0);
                for (int k = 0; k < taps; ++k) acc += coeff[k] * z[(size_t) k];
                return acc;
            }

            size_t channels, stages, maxIn = 0;
            T coeff[taps] {};
            std::vector<std::vector<T>> buf[4];
            std::vector<std::vector<T>> upState[4], downState[4];
            T* ptrs[8] {};
        };
    }
}
