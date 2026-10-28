#pragma once

#include <cmath>
#include <complex>
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

        static BiquadCoefs peak (double fs, double f, double q, double gainDb)
        {
            const double A = std::pow (10.0, gainDb / 40.0), w = juce::MathConstants<double>::twoPi * clampFreq (f, fs) / fs;
            const double alpha = std::sin (w) / (2.0 * q), c = std::cos (w);
            return make (1 + alpha * A, -2 * c, 1 - alpha * A, 1 + alpha / A, -2 * c, 1 - alpha / A);
        }

        static BiquadCoefs lowShelf (double fs, double f, double q, double gainDb)
        {
            const double A = std::pow (10.0, gainDb / 40.0), w = juce::MathConstants<double>::twoPi * clampFreq (f, fs) / fs;
            const double alpha = std::sin (w) / (2.0 * q), c = std::cos (w), s = 2.0 * std::sqrt (A) * alpha;
            return make (A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s),
                         (A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s);
        }

        static BiquadCoefs highShelf (double fs, double f, double q, double gainDb)
        {
            const double A = std::pow (10.0, gainDb / 40.0), w = juce::MathConstants<double>::twoPi * clampFreq (f, fs) / fs;
            const double alpha = std::sin (w) / (2.0 * q), c = std::cos (w), s = 2.0 * std::sqrt (A) * alpha;
            return make (A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s),
                         (A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s);
        }

        static BiquadCoefs highPass (double fs, double f, double q)
        {
            const double w = juce::MathConstants<double>::twoPi * clampFreq (f, fs) / fs, alpha = std::sin (w) / (2.0 * q), c = std::cos (w);
            return make ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
        }

        static BiquadCoefs lowPass (double fs, double f, double q)
        {
            const double w = juce::MathConstants<double>::twoPi * clampFreq (f, fs) / fs, alpha = std::sin (w) / (2.0 * q), c = std::cos (w);
            return make ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha);
        }

        static BiquadCoefs bandPass (double fs, double f, double q) // 0 dB at centre
        {
            const double w = juce::MathConstants<double>::twoPi * clampFreq (f, fs) / fs, alpha = std::sin (w) / (2.0 * q), c = std::cos (w);
            return make (alpha, 0.0, -alpha, 1 + alpha, -2 * c, 1 - alpha);
        }

        static BiquadCoefs notch (double fs, double f, double q)
        {
            const double w = juce::MathConstants<double>::twoPi * clampFreq (f, fs) / fs, alpha = std::sin (w) / (2.0 * q), c = std::cos (w);
            return make (1, -2 * c, 1, 1 + alpha, -2 * c, 1 - alpha);
        }
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
}
