#pragma once

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>
#include "Biquad.h"
#include "Util.h"

namespace vs
{
    // "Listen on": how a mix sounds on everyday playback devices (studio
    // monitors, a phone speaker, a laptop, earbuds, a car), as short filter
    // chains that run offline or in real time.
    //
    // The filters are the Web Audio API's BiquadFilterNode designs (the
    // specification's formulas): lowpass and highpass take Q in dB
    // (alpha = sin w / (2 * 10^(Q / 20))), peaking takes a plain Q, and the
    // shelves ignore Q (slope S = 1). The devices' lowpass and highpass Qs
    // are plain Qs (0.7 is about Butterworth's, flat to the corner), given
    // to those formulas in dB (qDb), so each corner has no resonance. A mono device (a phone's single speaker)
    // first sums the channels to 0.5 (L + R), as Web Audio's stereo to mono
    // mix does, and plays that on both.
    namespace device
    {
        enum class FilterType { lowpass, highpass, peaking, lowshelf, highshelf };

        struct Filter
        {
            FilterType type;
            double frequency, q = 1.0, gainDb = 0.0;
        };

        struct Device
        {
            const char* id;
            const char* name;
            bool mono;
            std::vector<Filter> filters;
        };

        // A plain Q as the lowpass and highpass formulas read it, dB: 20 log10 (q)
        inline double qDb (double q) { return 20.0 * std::log10 (q); }

        inline const std::vector<Device>& devices()
        {
            using F = FilterType;
            static const std::vector<Device> list {
                { "studio", "Studio monitors", false, {} },
                { "phone", "Phone speaker", true, { { F::highpass, 450, qDb (0.9) }, { F::peaking, 2500, 1.2, 5 }, { F::lowpass, 9000, qDb (0.7) } } },
                { "laptop", "Laptop", false, { { F::highpass, 200, qDb (0.8) }, { F::peaking, 1800, 1, 3 }, { F::lowpass, 13000, qDb (0.7) } } },
                { "earbuds", "Earbuds", false, { { F::highpass, 40, qDb (0.7) }, { F::lowshelf, 120, 1, 2 }, { F::peaking, 6000, 2, 3 } } },
                { "car", "Car", false, { { F::lowshelf, 90, 1, 5 }, { F::peaking, 300, 1.5, 2 }, { F::highshelf, 7000, 1, -4 } } },
            };
            return list;
        }

        inline int find (const char* id)
        {
            const auto& d = devices();
            for (size_t i = 0; i < d.size(); ++i)
                if (std::string_view (d[i].id) == id) return (int) i;
            return -1;
        }

        // The specification's coefficients for one filter, normalised to a0 = 1.
        // The frequency is kept to 0 .. fs / 2.
        inline BiquadCoefs design (const Filter& f, double fs)
        {
            using T = BiquadCoefs::Type;
            const double freq = std::max (0.0, std::min (fs / 2, f.frequency));
            // lowpass and highpass take their Q in dB; the shelves have a slope of 1 (Q = 1 / sqrt 2)
            const double qdB = std::pow (10.0, f.q / 20), shelfQ = 1.0 / std::sqrt (2.0);
            BiquadCoefs::Raw r {};
            switch (f.type)
            {
                case FilterType::lowpass:   r = BiquadCoefs::rbjRaw (T::lowPass, fs, freq, qdB); break;
                case FilterType::highpass:  r = BiquadCoefs::rbjRaw (T::highPass, fs, freq, qdB); break;
                case FilterType::peaking:   r = BiquadCoefs::rbjRaw (T::peak, fs, freq, f.q, f.gainDb); break;
                case FilterType::lowshelf:  r = BiquadCoefs::rbjRaw (T::lowShelf, fs, freq, shelfQ, f.gainDb); break;
                case FilterType::highshelf: r = BiquadCoefs::rbjRaw (T::highShelf, fs, freq, shelfQ, f.gainDb); break;
                default: return {};
            }
            return BiquadCoefs::make (r.b0, r.b1, r.b2, r.a0, r.a1, r.a2);
        }
    }

    // One device's chain on stereo audio, in place, block by block.
    class DeviceSim
    {
    public:
        void prepare (double sampleRate) { fs = sampleRate; setDevice (index); }

        // A device by its place in device::devices() (0 = studio monitors, no change).
        void setDevice (int i)
        {
            const auto& list = device::devices();
            index = (i >= 0 && i < (int) list.size()) ? i : 0;
            const auto& d = list[(size_t) index];
            mono = d.mono;
            std::vector<BiquadCoefs> s;
            for (const auto& f : d.filters) s.push_back (device::design (f, fs));
            chain.setSections (std::move (s), 2);
        }

        int getDevice() const { return index; }
        void reset() { chain.reset(); }

        // r may be null (one channel).
        void process (float* l, float* r, int n)
        {
            if (mono && r != nullptr)
            {
                for (int i = 0; i < n; ++i) l[i] = midOf (l[i], r[i]);
                chain.process (0, l, n);
                std::copy (l, l + n, r);
                return;
            }
            chain.process (0, l, n);
            if (r != nullptr) chain.process (1, r, n);
        }

        // The chain's gain at freq, dB (the curve to draw)
        double magnitudeDb (double freq) const
        {
            return db20 (std::max (1.0e-12, sectionsResponse (chain.getSections(), freq, fs)), 0.0);
        }

    private:
        double fs = 48000.0;
        int index = 0;
        bool mono = false;
        BiquadChain chain;
    };
}
