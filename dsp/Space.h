#pragma once

#include "JuceInclude.h"
#include <cmath>
#include <vector>
#include "DelayLine.h"
#include "ReverbEngine.h"
#include "Util.h"

namespace vs
{
    // Vocal delay (tempo-synced, darkened feedback) and reverb (ReverbEngine's
    // Space setting, fed a filtered send) mixed in parallel with the dry
    // vocal. Echoes are sent into the reverb a little so they sit in the same
    // room.
    class Space
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec)
        {
            fs = spec.sampleRate;
            const int block = (int) spec.maximumBlockSize;

            for (auto& d : delay) d.prepare ((int) (fs * 2.5));
            reverb.prepare (fs, block);
            reverb.setSettings (ReverbEngine::space (lastSize < 0.0f ? 0.5f : lastSize));

            for (auto* b : { &sendL, &sendR, &echoL, &echoR, &wetL, &wetR }) b->assign ((size_t) block, 0.0f);

            fbLpC = onePoleHz (5000.0f, fs);
            fbHpC = onePoleHz (250.0f, fs);
            sendLpC = onePoleHz (8000.0f, fs);
            sendHpC = onePoleHz (220.0f, fs);
            for (int c = 0; c < 2; ++c)
                fbLp[c] = fbHp[c] = sendLp[c] = sendHp[c] = 0.0f;

            delaySamples.reset (fs, 0.08);
            delaySamples.setCurrentAndTargetValue ((float) (fs * 0.25));
            delayAmt.reset (fs, 0.05);
            verbAmt.reset (fs, 0.05);
        }

        void setParams (float verb01, float size01, float delay01, float delaySeconds, float feedback)
        {
            if (size01 != lastSize)
            {
                lastSize = size01;
                const auto s = ReverbEngine::space (size01);
                reverb.setSettings (s);
                // The engine evens out loudness across decay times; Space's
                // reverb gets louder as it gets longer, about as the decay.
                verbLevel = std::sqrt (s.decaySec / 0.9f);
            }
            verbAmt.setTargetValue (verb01 * verb01 * 1.4f * verbLevel);
            delayAmt.setTargetValue (delay01 * 0.8f);
            delaySamples.setTargetValue ((float) juce::jlimit (1.0, fs * 2.4, delaySeconds * fs));
            fb = feedback;
        }

        void reset()
        {
            for (auto& d : delay) d.clear();
            reverb.reset();
        }

        // feed = false stops new sound going into the delay and reverb but
        // lets what's already in them ring out (used when Space is switched off).
        void process (float* const* ch, int numSamples, bool feed = true)
        {
            const float inGain = feed ? 1.0f : 0.0f;
            float* e[2] = { echoL.data(), echoR.data() };
            float* s[2] = { sendL.data(), sendR.data() };

            for (int i = 0; i < numSamples; ++i)
            {
                const float ds = delaySamples.getNextValue();
                const float dAmt = delayAmt.getNextValue();

                for (int c = 0; c < 2; ++c)
                {
                    // slightly longer right side for width
                    const float out = delay[c].read ((double) (c == 0 ? ds : ds * 1.015f));
                    fbLp[c] += fbLpC * (out - fbLp[c]);
                    fbHp[c] += fbHpC * (fbLp[c] - fbHp[c]);
                    const float dark = fbLp[c] - fbHp[c];

                    delay[c].write (ch[c][i] * inGain + dark * fb);
                    e[c][i] = dark * dAmt;

                    const float snd = ch[c][i] * inGain + e[c][i] * 0.5f;
                    sendLp[c] += sendLpC * (snd - sendLp[c]);
                    sendHp[c] += sendHpC * (sendLp[c] - sendHp[c]);
                    s[c][i] = sendLp[c] - sendHp[c];
                }
            }

            reverb.process (sendL.data(), sendR.data(), wetL.data(), wetR.data(), numSamples);

            for (int i = 0; i < numSamples; ++i)
            {
                const float v = verbAmt.getNextValue();
                ch[0][i] += echoL[(size_t) i] + wetL[(size_t) i] * v;
                ch[1][i] += echoR[(size_t) i] + wetR[(size_t) i] * v;
            }
        }

    private:
        double fs = 44100.0;
        DelayLine delay[2];
        ReverbEngine reverb;
        std::vector<float> sendL, sendR, echoL, echoR, wetL, wetR;
        juce::SmoothedValue<float> delaySamples, delayAmt, verbAmt;
        float fb = 0.25f, lastSize = -1.0f, verbLevel = 1.0f;
        float fbLpC = 0.0f, fbHpC = 0.0f, sendLpC = 0.0f, sendHpC = 0.0f;
        float fbLp[2] {}, fbHp[2] {}, sendLp[2] {}, sendHp[2] {};
    };
}
