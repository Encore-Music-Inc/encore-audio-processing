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
    // shelves ignore Q (slope S = 1). A mono device (a phone's single speaker)
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

        inline const std::vector<Device>& devices()
        {
            using F = FilterType;
            static const std::vector<Device> list {
                { "studio", "Studio monitors", false, {} },
                { "phone", "Phone speaker", true, { { F::highpass, 450, 0.9 }, { F::peaking, 2500, 1.2, 5 }, { F::lowpass, 9000, 0.7 } } },
                { "laptop", "Laptop", false, { { F::highpass, 200, 0.8 }, { F::peaking, 1800, 1, 3 }, { F::lowpass, 13000, 0.7 } } },
                { "earbuds", "Earbuds", false, { { F::highpass, 40, 0.7 }, { F::lowshelf, 120, 1, 2 }, { F::peaking, 6000, 2, 3 } } },
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
            const double nyquist = fs / 2, freq = std::max (0.0, std::min (nyquist, f.frequency));
            const double A = std::pow (10.0, f.gainDb / 40);
            const double w0 = 2 * 3.141592653589793 * freq / fs, c = std::cos (w0), s = std::sin (w0);
            const double alphaQ = s / (2 * f.q);
            const double alphaQdB = s / (2 * std::pow (10.0, f.q / 20));
            const double alphaS = s / 2 * std::sqrt (2.0); // S = 1
            switch (f.type)
            {
                case FilterType::lowpass:
                    return BiquadCoefs::make ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alphaQdB, -2 * c, 1 - alphaQdB);
                case FilterType::highpass:
                    return BiquadCoefs::make ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alphaQdB, -2 * c, 1 - alphaQdB);
                case FilterType::peaking:
                    return BiquadCoefs::make (1 + alphaQ * A, -2 * c, 1 - alphaQ * A, 1 + alphaQ / A, -2 * c, 1 - alphaQ / A);
                case FilterType::lowshelf:
                {
                    const double k = 2 * alphaS * std::sqrt (A);
                    return BiquadCoefs::make (A * ((A + 1) - (A - 1) * c + k), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - k),
                                              (A + 1) + (A - 1) * c + k, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - k);
                }
                case FilterType::highshelf:
                {
                    const double k = 2 * alphaS * std::sqrt (A);
                    return BiquadCoefs::make (A * ((A + 1) + (A - 1) * c + k), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - k),
                                              (A + 1) - (A - 1) * c + k, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - k);
                }
            }
            return {};
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
                for (int i = 0; i < n; ++i) l[i] = 0.5f * (l[i] + r[i]);
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
