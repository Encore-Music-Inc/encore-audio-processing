#pragma once

#include "JuceInclude.h"
#include "Biquad.h"
#include "DelayLine.h"
#include "Util.h"

namespace vs
{
    // Double: a fake second take for width. Two copies of the guitar, each
    // delayed 14-25 ms with its own slow wobble (which also bends the pitch a
    // few cents, like a second performance drifting), go into the *side*
    // signal only: left gets +copy, right gets -copy. In mono, L+R, they
    // cancel exactly, so the mono sum is the original guitar with no comb
    // filtering at all; only the overall level drops slightly (see trim).
    // Below ~150 Hz the copies are filtered out so the low end stays centred.
    class Doubler
    {
    public:
        void prepare (double sampleRate)
        {
            fs = sampleRate;
            line.prepare ((int) (0.06 * fs));
            phaseA = 0.0; phaseB = 0.37;
            hp1.c = BiquadCoefs::highPass (fs, 150.0, 0.7071);
            hp2.c = hp1.c;
            hp1.reset(); hp2.reset();
            width.reset (fs, 0.05);
            width.setCurrentAndTargetValue (0.0f);
        }

        // Stereo in place. widthTarget 0..1 (0 = off).
        void process (float* l, float* r, int n, float widthTarget)
        {
            width.setTargetValue (widthTarget);
            if (! width.isSmoothing() && width.getTargetValue() <= 0.0f)
            {
                // still keep the delay line fed, so turning it on is seamless
                for (int i = 0; i < n; ++i) line.write (midOf (l[i], r[i]));
                return;
            }

            const double incA = 0.31 / fs, incB = 0.47 / fs;
            for (int i = 0; i < n; ++i)
            {
                const float mid = midOf (l[i], r[i]), side = sideOf (l[i], r[i]);
                line.put (mid);

                // delays in samples: A 14 ms +-1.1 ms, B 23 ms +-1.3 ms (about 4 and 7 cents of drift)
                const double dA = fs * (0.014 + 0.0011 * std::sin (juce::MathConstants<double>::twoPi * phaseA));
                const double dB = fs * (0.023 + 0.0013 * std::sin (juce::MathConstants<double>::twoPi * phaseB));
                phaseA += incA; if (phaseA >= 1.0) phaseA -= 1.0;
                phaseB += incB; if (phaseB >= 1.0) phaseB -= 1.0;

                const float copy = 0.62f * line.readHermite (dA) - 0.62f * line.readHermite (dB);
                const float s = hp2.process (0, hp1.process (0, copy));
                line.advance();

                // The copies add up to ~0.77 of the guitar's power on each side;
                // the trim splits the difference between "mono stays the
                // same" and "stereo stays the same" (mono -1.4 dB, each side
                // +1.1 dB at full width).
                const float w = width.getNextValue();
                const float trim = 1.0f / std::sqrt (1.0f + 0.5f * 0.77f * w * w);
                l[i] = trim * (mid + side + w * s);
                r[i] = trim * (mid - side - w * s);
            }
        }

    private:
        double fs = 48000.0, phaseA = 0.0, phaseB = 0.0;
        DelayLine line;
        Biquad hp1, hp2;
        juce::SmoothedValue<float> width;
    };
}
