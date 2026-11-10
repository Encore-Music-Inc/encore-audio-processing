#pragma once

#include <cmath>
#include <cstring>

namespace vs
{
    // 4th-order Linkwitz-Riley filter for band splits: two cascaded
    // 2nd-order Butterworth sections, each a topology-preserving
    // state-variable filter. The low-pass and high-pass at the same cut-off
    // sum to an all-pass, so bands split with them recombine flat; the
    // all-pass type is that sum, for keeping a band that skips a split in
    // phase with the ones that went through it. Up to two channels; the
    // same filters run on the desktop and in the browser.
    template <typename T>
    class LinkwitzRiley
    {
    public:
        enum class Type { lowPass, highPass, allPass };

        void prepare (double sampleRate) { fs = sampleRate; update(); reset(); }
        void reset() { std::memset (st, 0, sizeof (st)); }
        void setType (Type t) { type = t; }
        // Kept to 10 Hz .. 0.49 fs.
        void setCutoffFrequency (T hz) { cutoff = hz; update(); }

        T processSample (int ch, T x)
        {
            if (type == Type::allPass)
                return section (ch, 0, section (ch, 1, x, true), true) + section (ch, 2, section (ch, 3, x, false), false);
            const bool lp = type == Type::lowPass;
            return section (ch, 0, section (ch, 1, x, lp), lp);
        }

    private:
        // one Butterworth 2nd-order section; returns its low-pass or high-pass output
        T section (int ch, int k, T x, bool lowPass)
        {
            T* s = st[ch & 1][k];
            const T hp = (x - (T) (2 * R + g) * s[0] - s[1]) * h;
            const T bp = g * hp + s[0]; s[0] = g * hp + bp;
            const T lpv = g * bp + s[1]; s[1] = g * bp + lpv;
            return lowPass ? lpv : hp;
        }

        void update()
        {
            const double v = (double) cutoff, hi = fs * 0.49;
            const double f = v < 10.0 ? 10.0 : (v > hi ? hi : v);
            g = (T) std::tan (pi * f / fs);
            h = (T) (1.0 / (1.0 + 2.0 * R * g + g * g));
        }

        static constexpr double pi = 3.141592653589793;
        static constexpr double R = 0.70710678118654752; // 1/(2Q), Q = 1/sqrt(2)
        double fs = 48000.0;
        T cutoff = T (1000), g = T (0.06), h = T (0.9);
        T st[2][4][2] {};
        Type type = Type::lowPass;
    };
}
