#pragma once

#include <juce_dsp/juce_dsp.h>
#include "Util.h"

namespace vs
{
    // Vocal delay (tempo-synced, darkened feedback) and reverb (pre-delayed,
    // filtered send) mixed in parallel with the dry vocal. Echoes are sent into
    // the reverb a little so they sit in the same room.
    class Space
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec)
        {
            fs = spec.sampleRate;

            delay.setMaximumDelayInSamples ((int) (fs * 2.5));
            delay.prepare (spec);
            preDelay.setMaximumDelayInSamples ((int) (fs * 0.1));
            preDelay.prepare (spec);

            reverb.setSampleRate (fs);
            reverb.reset();

            send.setSize (2, (int) spec.maximumBlockSize);
            echo.setSize (2, (int) spec.maximumBlockSize);

            fbLpC = onePole (1000.0f / (juce::MathConstants<float>::twoPi * 5000.0f), fs);
            fbHpC = onePole (1000.0f / (juce::MathConstants<float>::twoPi * 250.0f), fs);
            sendLpC = onePole (1000.0f / (juce::MathConstants<float>::twoPi * 8000.0f), fs);
            sendHpC = onePole (1000.0f / (juce::MathConstants<float>::twoPi * 220.0f), fs);
            for (int c = 0; c < 2; ++c)
                fbLp[c] = fbHp[c] = sendLp[c] = sendHp[c] = 0.0f;

            delaySamples.reset (fs, 0.08);
            delaySamples.setCurrentAndTargetValue ((float) (fs * 0.25));
            delayAmt.reset (fs, 0.05);
            verbAmt.reset (fs, 0.05);
        }

        void setParams (float verb01, float size01, float delay01, float delaySeconds, float feedback)
        {
            verbAmt.setTargetValue (verb01 * verb01 * 1.4f);
            delayAmt.setTargetValue (delay01 * 0.8f);
            delaySamples.setTargetValue ((float) juce::jlimit (1.0, fs * 2.4, delaySeconds * fs));
            fb = feedback;

            if (size01 != lastSize)
            {
                lastSize = size01;
                juce::Reverb::Parameters p;
                p.roomSize = 0.3f + size01 * 0.65f;
                p.damping = 0.45f;
                p.wetLevel = 0.33f;
                p.dryLevel = 0.0f;
                p.width = 1.0f;
                p.freezeMode = 0.0f;
                reverb.setParameters (p);
            }
        }

        void reset()
        {
            delay.reset();
            preDelay.reset();
            reverb.reset();
        }

        // feed = false stops new sound going into the delay and reverb but
        // lets what's already in them ring out (used when Space is switched off).
        void process (float* const* ch, int numSamples, bool feed = true)
        {
            const float inGain = feed ? 1.0f : 0.0f;
            auto* sL = send.getWritePointer (0);
            auto* sR = send.getWritePointer (1);
            auto* eL = echo.getWritePointer (0);
            auto* eR = echo.getWritePointer (1);
            const float preSamples = (float) (fs * 0.025);

            for (int i = 0; i < numSamples; ++i)
            {
                const float ds = delaySamples.getNextValue();
                const float dAmt = delayAmt.getNextValue();
                float* e[2] = { eL, eR };
                float* s[2] = { sL, sR };

                for (int c = 0; c < 2; ++c)
                {
                    // slightly longer right side for width
                    const float out = delay.popSample (c, c == 0 ? ds : ds * 1.015f);
                    fbLp[c] += fbLpC * (out - fbLp[c]);
                    fbHp[c] += fbHpC * (fbLp[c] - fbHp[c]);
                    const float dark = fbLp[c] - fbHp[c];

                    delay.pushSample (c, ch[c][i] * inGain + dark * fb);
                    e[c][i] = dark * dAmt;

                    float snd = ch[c][i] * inGain + e[c][i] * 0.5f;
                    sendLp[c] += sendLpC * (snd - sendLp[c]);
                    sendHp[c] += sendHpC * (sendLp[c] - sendHp[c]);
                    snd = sendLp[c] - sendHp[c];

                    preDelay.pushSample (c, snd);
                    s[c][i] = preDelay.popSample (c, preSamples);
                }
            }

            reverb.processStereo (sL, sR, numSamples);

            for (int i = 0; i < numSamples; ++i)
            {
                const float v = verbAmt.getNextValue();
                ch[0][i] += eL[i] + sL[i] * v;
                ch[1][i] += eR[i] + sR[i] * v;
            }
        }

    private:
        double fs = 44100.0;
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delay { 4 };
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> preDelay { 4 };
        juce::Reverb reverb;
        juce::AudioBuffer<float> send, echo;
        juce::SmoothedValue<float> delaySamples, delayAmt, verbAmt;
        float fb = 0.25f, lastSize = -1.0f;
        float fbLpC = 0.0f, fbHpC = 0.0f, sendLpC = 0.0f, sendHpC = 0.0f;
        float fbLp[2] {}, fbHp[2] {}, sendLp[2] {}, sendHp[2] {};
    };
}
