#pragma once

#include "JuceInclude.h"
#include "Util.h"

namespace vs
{
    // Tube-ish preamp drive. Slightly asymmetric tanh (adds even harmonics),
    // run at 2x oversampling to keep the top end clean. Unity gain for quiet
    // signals, so turning it up adds grit without a big volume jump.
    // Oversampling always runs so the plugin's latency never changes.
    class Saturator
    {
    public:
        void prepare (double sampleRate, int maxBlockSize)
        {
            os.initProcessing ((size_t) maxBlockSize);
            os.reset();
            dcR = 1.0f - (float) (juce::MathConstants<double>::twoPi * 10.0 / sampleRate);
            for (auto& s : dcX) s = 0.0f;
            for (auto& s : dcY) s = 0.0f;
        }

        int getLatencySamples() const { return (int) std::round (os.getLatencyInSamples()); }

        void setDrive (float amount01)
        {
            active = amount01 > 0.001f;
            k = dbToGain (amount01 * 24.0f);
            tb = std::tanh (k * bias);
            norm = 1.0f / (k * (1.0f - tb * tb));
            mix = std::min (1.0f, amount01 * 5.0f);
        }

        void process (juce::dsp::AudioBlock<float>& block)
        {
            auto up = os.processSamplesUp (block);

            if (active)
            {
                for (size_t c = 0; c < up.getNumChannels(); ++c)
                {
                    auto* d = up.getChannelPointer (c);
                    for (size_t i = 0; i < up.getNumSamples(); ++i)
                    {
                        const float x = d[i];
                        const float sat = (std::tanh (k * (x + bias)) - tb) * norm;
                        d[i] = x + mix * (sat - x);
                    }
                }
            }

            os.processSamplesDown (block);

            if (active)
            {
                for (size_t c = 0; c < block.getNumChannels() && c < 2; ++c)
                {
                    auto* d = block.getChannelPointer (c);
                    for (size_t i = 0; i < block.getNumSamples(); ++i)
                    {
                        const float y = d[i] - dcX[c] + dcR * dcY[c];
                        dcX[c] = d[i];
                        dcY[c] = y;
                        d[i] = y;
                    }
                }
            }
        }

    private:
        juce::dsp::Oversampling<float> os { 2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true };
        static constexpr float bias = 0.12f;
        bool active = false;
        float k = 1.0f, tb = 0.0f, norm = 1.0f, mix = 0.0f;
        float dcR = 0.999f, dcX[2] {}, dcY[2] {};
    };
}
