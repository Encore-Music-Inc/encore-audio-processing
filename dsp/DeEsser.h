#pragma once

#include <algorithm>
#include <atomic>
#include "JuceInclude.h"
#include "Dynamics.h"
#include "Svf.h"
#include "Util.h"

namespace vs
{
    // Split-band de-esser that listens for *relative* sibilance: how much of
    // the signal's energy sits above the split frequency. That makes one
    // setting work on a whisper and a scream alike, since it doesn't depend on
    // how loud the singer is. Only the band above the split is turned down.
    // Up to two channels. The split is a state-variable high-pass (Svf.h).
    class DeEsser
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec)
        {
            hp.prepare (spec.sampleRate, (int) spec.numChannels);
            hp.setType (StateVariableFilter<float>::Type::highPass);
            hp.setResonance (0.7071f);
            envAtk = onePole (1.0f, spec.sampleRate);
            envRel = onePole (60.0f, spec.sampleRate);
            grAtk = onePole (0.5f, spec.sampleRate);
            grRel = onePole (40.0f, spec.sampleRate);
            hEnv = fEnv = 1.0e-9f;
            grDb = 0.0f;
        }

        void setParams (float amount01, float freqHz)
        {
            thresholdDb = -2.0f - amount01 * 14.0f;
            hp.setCutoffFrequency (freqHz);
        }

        void process (float* const* ch, int numCh, int numSamples)
        {
            float maxGr = 0.0f;

            for (int i = 0; i < numSamples; ++i)
            {
                float h[2] {}, hs = 0.0f, fs = 0.0f;
                for (int c = 0; c < numCh; ++c)
                {
                    const float x = ch[c][i];
                    h[c] = hp.processSample (c, x);
                    hs = std::max (hs, h[c] * h[c]);
                    fs = std::max (fs, x * x);
                }

                followEnvelope (hEnv, hs, envAtk, envRel);
                followEnvelope (fEnv, fs, envAtk, envRel);

                const float ratioDb = powerToDb (hEnv) - powerToDb (fEnv);
                const float over = ratioDb - thresholdDb;
                const float target = powerToDb (fEnv) > -60.0f ? dynamics::cutDb (over, 1.5f, 0.0f, 14.0f) : 0.0f;

                followEnvelope (grDb, target, grAtk, grRel);
                maxGr = std::max (maxGr, grDb);
                const float g = dbToGain (-grDb);

                for (int c = 0; c < numCh; ++c)
                    ch[c][i] = (ch[c][i] - h[c]) + g * h[c];
            }

            meterDb.store (maxGr);
        }

        std::atomic<float> meterDb { 0.0f };

    private:
        StateVariableFilter<float> hp;
        float thresholdDb = -8.0f;
        float envAtk = 0.0f, envRel = 0.0f, grAtk = 0.0f, grRel = 0.0f;
        float hEnv = 0.0f, fEnv = 0.0f, grDb = 0.0f;
    };
}
