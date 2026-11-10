#pragma once

#include <cmath>
#include <complex>
#include <vector>
#include "JuceInclude.h"

namespace vs
{
    // Second-order filter coefficients (normalised so a0 = 1), with the
    // RBJ "Audio EQ Cookbook" designs. Double precision so low shelves and
    // 20 Hz cuts stay clean. magnitudeDb() lets the UI draw exactly the
    // curve the audio is getting.
    struct BiquadCoefs
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

        double magnitudeDb (double freq, double fs) const
        {
            const double w = juce::MathConstants<double>::twoPi * freq / fs;
            const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
            const auto h = (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
            return 20.0 * std::log10 (std::max (std::abs (h), 1.0e-12));
        }

        static BiquadCoefs make (double b0, double b1, double b2, double a0, double a1, double a2)
        {
            return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
        }

        static double clampFreq (double f, double fs) { return juce::jlimit (5.0, fs * 0.49, f); }

        // The RBJ designs, by type, at exactly the frequency given (no
        // clamping). gainDb is used by peak and the shelves; bandPass is the
        // 0 dB-at-centre form.
        enum class Type { lowPass, highPass, bandPass, notch, peak, lowShelf, highShelf };

        // The six coefficients as the cookbook gives them, before dividing by a0.
        struct Raw { double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0; };

        static Raw rbjRaw (Type type, double fs, double f, double q, double gainDb = 0.0)
        {
            const double w = juce::MathConstants<double>::twoPi * f / fs, alpha = std::sin (w) / (2.0 * q), c = std::cos (w);
            switch (type)
            {
                case Type::lowPass:  return { (1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha };
                case Type::highPass: return { (1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha };
                case Type::bandPass: return { alpha, 0.0, -alpha, 1 + alpha, -2 * c, 1 - alpha };
                case Type::notch:    return { 1, -2 * c, 1, 1 + alpha, -2 * c, 1 - alpha };
                case Type::peak:
                {
                    const double A = std::pow (10.0, gainDb / 40.0);
                    return { 1 + alpha * A, -2 * c, 1 - alpha * A, 1 + alpha / A, -2 * c, 1 - alpha / A };
                }
                case Type::lowShelf:
                {
                    const double A = std::pow (10.0, gainDb / 40.0), s = 2.0 * std::sqrt (A) * alpha;
                    return { A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s),
                             (A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s };
                }
                case Type::highShelf:
                {
                    const double A = std::pow (10.0, gainDb / 40.0), s = 2.0 * std::sqrt (A) * alpha;
                    return { A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s),
                             (A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s };
                }
            }
            return {};
        }

        static BiquadCoefs rbj (Type type, double fs, double f, double q, double gainDb = 0.0)
        {
            const auto r = rbjRaw (type, fs, f, q, gainDb);
            return make (r.b0, r.b1, r.b2, r.a0, r.a1, r.a2);
        }

        // The same designs with the frequency kept to 5 Hz .. 0.49 fs.
        static BiquadCoefs peak (double fs, double f, double q, double gainDb)  { return rbj (Type::peak, fs, clampFreq (f, fs), q, gainDb); }
        static BiquadCoefs lowShelf (double fs, double f, double q, double gainDb)  { return rbj (Type::lowShelf, fs, clampFreq (f, fs), q, gainDb); }
        static BiquadCoefs highShelf (double fs, double f, double q, double gainDb) { return rbj (Type::highShelf, fs, clampFreq (f, fs), q, gainDb); }
        static BiquadCoefs highPass (double fs, double f, double q) { return rbj (Type::highPass, fs, clampFreq (f, fs), q); }
        static BiquadCoefs lowPass (double fs, double f, double q)  { return rbj (Type::lowPass, fs, clampFreq (f, fs), q); }
        static BiquadCoefs bandPass (double fs, double f, double q) { return rbj (Type::bandPass, fs, clampFreq (f, fs), q); } // 0 dB at centre
        static BiquadCoefs notch (double fs, double f, double q)    { return rbj (Type::notch, fs, clampFreq (f, fs), q); }
    };

    // Transposed direct form II, one state pair per channel.
    struct Biquad
    {
        BiquadCoefs c;
        double s1[2] {}, s2[2] {};

        void reset() { s1[0] = s1[1] = s2[0] = s2[1] = 0.0; }

        float process (int ch, float xf)
        {
            const double x = xf;
            const double y = c.b0 * x + s1[ch];
            s1[ch] = c.b1 * x - c.a1 * y + s2[ch];
            s2[ch] = c.b2 * x - c.a2 * y;
            return (float) y;
        }
    };

    // One section run once over a whole buffer, in place, in direct form I
    // with the coefficients as designed (not divided by a0): each output is
    // (b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2) / a0, the past outputs kept in
    // double and the inputs as stored. The click and drum synthesizers filter
    // this way, so their sounds come out the same on every platform.
    template <typename T>
    void filterDirectFormI (const BiquadCoefs::Raw& k, T* x, int n)
    {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (int i = 0; i < n; ++i)
        {
            const double v = (double) x[i];
            const double y = (k.b0 * v + k.b1 * x1 + k.b2 * x2 - k.a1 * y1 - k.a2 * y2) / k.a0;
            x2 = x1; x1 = v; y2 = y1; y1 = y;
            x[i] = (T) y;
        }
    }

    // An even-order Butterworth filter (low-pass or high-pass) as order / 2
    // cascaded RBJ sections, section k with Q = 1 / (2 cos (pi (2k + 1) / (2 order))).
    // No frequency clamping.
    inline std::vector<BiquadCoefs> butterworth (BiquadCoefs::Type type, double fs, double f, int order = 4)
    {
        std::vector<BiquadCoefs> out;
        for (int k = 0; k < order / 2; ++k)
            out.push_back (BiquadCoefs::rbj (type, fs, f, 1.0 / (2.0 * std::cos ((juce::MathConstants<double>::pi * (2 * k + 1)) / (2 * order)))));
        return out;
    }

    // |H(f)|, the linear magnitude of a cascade of sections at frequency f.
    inline double sectionsResponse (const std::vector<BiquadCoefs>& sections, double f, double fs)
    {
        const double w = juce::MathConstants<double>::twoPi * f / fs;
        const double cr = std::cos (w), ci = -std::sin (w), c2r = std::cos (2 * w), c2i = -std::sin (2 * w);
        double mag = 1.0;
        for (const auto& s : sections)
        {
            const double nr = s.b0 + s.b1 * cr + s.b2 * c2r, ni = s.b1 * ci + s.b2 * c2i;
            const double dr = 1 + s.a1 * cr + s.a2 * c2r, di = s.a1 * ci + s.a2 * c2i;
            mag *= std::sqrt ((nr * nr + ni * ni) / (dr * dr + di * di));
        }
        return mag;
    }

    // A cascade of any number of sections over any number of channels, each
    // channel's state (double, transposed direct form II) kept between
    // chunks. process() filters a chunk in place, one section after another
    // over the whole chunk; with float samples each section's output is stored
    // as float before the next section reads it.
    class BiquadChain
    {
    public:
        BiquadChain() = default;
        BiquadChain (std::vector<BiquadCoefs> s, int channels = 2) { setSections (std::move (s), channels); }

        void setSections (std::vector<BiquadCoefs> s, int channels = 2)
        {
            sections = std::move (s);
            state.assign ((size_t) channels, std::vector<double> (sections.size() * 2, 0.0));
        }

        void reset() { for (auto& z : state) std::fill (z.begin(), z.end(), 0.0); }

        const std::vector<BiquadCoefs>& getSections() const { return sections; }
        int numChannels() const { return (int) state.size(); }

        template <typename T>
        void process (int ch, T* x, int n)
        {
            auto& z = state[(size_t) ch];
            for (size_t s = 0; s < sections.size(); ++s)
            {
                const auto& k = sections[s];
                double z1 = z[2 * s], z2 = z[2 * s + 1];
                for (int i = 0; i < n; ++i)
                {
                    const double v = (double) x[i];
                    const double y = k.b0 * v + z1;
                    z1 = k.b1 * v - k.a1 * y + z2;
                    z2 = k.b2 * v - k.a2 * y;
                    x[i] = (T) y;
                }
                z[2 * s] = z1;
                z[2 * s + 1] = z2;
            }
        }

        // Channels 0 and 1 at once (b may be null for channel 0 alone), every
        // section per sample: the same numbers as process (0, a, n) and
        // process (1, b, n), with the work of the sections and the two
        // channels overlapping, which runs faster.
        template <typename T>
        void processPair (T* a, T* b, int n)
        {
            switch (sections.size())
            {
                case 1: pair<1> (a, b, n); break;
                case 2: pair<2> (a, b, n); break;
                case 3: pair<3> (a, b, n); break;
                case 4: pair<4> (a, b, n); break;
                default: process (0, a, n); if (b != nullptr) process (1, b, n); break;
            }
        }

    private:
        template <int NS, typename T>
        void pair (T* a, T* b, int n)
        {
            double za[2 * NS] {}, zb[2 * NS] {};
            BiquadCoefs k[NS];
            for (int s = 0; s < NS; ++s) k[s] = sections[(size_t) s];
            std::copy (state[0].begin(), state[0].end(), za);
            if (b != nullptr) std::copy (state[1].begin(), state[1].end(), zb);
            for (int i = 0; i < n; ++i)
            {
                T va = a[i], vb = b != nullptr ? b[i] : T (0);
                for (int s = 0; s < NS; ++s)
                {
                    const double x0 = (double) va, x1 = (double) vb;
                    const double y0 = k[s].b0 * x0 + za[2 * s];
                    const double y1 = k[s].b0 * x1 + zb[2 * s];
                    za[2 * s] = k[s].b1 * x0 - k[s].a1 * y0 + za[2 * s + 1];
                    zb[2 * s] = k[s].b1 * x1 - k[s].a1 * y1 + zb[2 * s + 1];
                    za[2 * s + 1] = k[s].b2 * x0 - k[s].a2 * y0;
                    zb[2 * s + 1] = k[s].b2 * x1 - k[s].a2 * y1;
                    va = (T) y0;
                    vb = (T) y1;
                }
                a[i] = va;
                if (b != nullptr) b[i] = vb;
            }
            std::copy (za, za + 2 * NS, state[0].begin());
            if (b != nullptr) std::copy (zb, zb + 2 * NS, state[1].begin());
        }

        std::vector<BiquadCoefs> sections;
        std::vector<std::vector<double>> state;
    };
}
