#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include "Dynamics.h"
#include "Util.h"

namespace vs
{
    // One-knob vocal compressor. "Amount" moves threshold, ratio and makeup
    // together; "Style" picks the character:
    //   Smooth - RMS detection, slow attack, for folk / ballads
    //   Punchy - peak detection, medium attack, for most rock / emo leads
    //   Slam   - fast peak detection and high ratio, for screams and shouts
    class Compressor
    {
    public:
        enum Style { smooth = 0, punchy, slam };

        void prepare (double sampleRate)
        {
            fs = sampleRate;
            rmsCoef = onePole (10.0f, fs);
            rms = 0.0f;
            grDb = 0.0f;
            setParams (amount, style);
        }

        void setParams (float amount01, int newStyle)
        {
            amount = amount01;
            style = newStyle;

            thresholdDb = -10.0f - amount * 22.0f;
            ratio = 1.5f + amount * (style == slam ? 8.0f : 4.5f);

            const float atkMs = style == smooth ? 12.0f : style == punchy ? 4.0f : 0.8f;
            const float relMs = style == smooth ? 180.0f : style == punchy ? 90.0f : 50.0f;
            atk = onePole (atkMs, fs);
            rel = onePole (relMs, fs);

            makeupDb = std::max (0.0f, -12.0f - thresholdDb) * (1.0f - 1.0f / ratio) * 0.7f;
            rmsDetect = style == smooth;
        }

        // The same compressor set by hand, as a general-purpose one: where it
        // starts (dB), how hard (ratio), how fast it cuts and lets go (ms), the
        // gain after it (dB), and whether it hears the level as RMS (smooth)
        // or peak (punchy)
        struct Detail { float thresholdDb = -18.0f, ratio = 3.0f, attackMs = 10.0f, releaseMs = 120.0f, makeupDb = 0.0f; bool rms = false; };
        void setDetail (const Detail& d)
        {
            thresholdDb = d.thresholdDb;
            ratio = std::max (1.0f, d.ratio);
            atk = onePole (std::max (0.05f, d.attackMs), fs);
            rel = onePole (std::max (1.0f, d.releaseMs), fs);
            makeupDb = d.makeupDb;
            rmsDetect = d.rms;
        }

        // Compresses ch in place. With key channels (a sidechain), the compressor
        // listens to them, times keyGain, instead of to ch: another track can
        // trigger it (ducking under a kick drum or a voice).
        void process (float* const* ch, int numCh, int numSamples,
                      const float* const* key = nullptr, int numKey = 0, float keyGain = 1.0f)
        {
            float maxGr = 0.0f;
            const bool keyed = key != nullptr && numKey > 0;

            for (int i = 0; i < numSamples; ++i)
            {
                float peak = 0.0f, sq = 0.0f;
                if (keyed)
                {
                    for (int c = 0; c < numKey; ++c)
                    {
                        const float k = key[c][i] * keyGain;
                        peak = std::max (peak, std::abs (k));
                        sq = std::max (sq, k * k);
                    }
                }
                else
                {
                    for (int c = 0; c < numCh; ++c)
                    {
                        peak = std::max (peak, std::abs (ch[c][i]));
                        sq = std::max (sq, ch[c][i] * ch[c][i]);
                    }
                }

                rms += rmsCoef * (sq - rms);
                const float levelDb = rmsDetect ? powerToDb (rms) + 3.0f : ampToDb (peak);

                const float target = dynamics::compressedDb (levelDb, thresholdDb, ratio, kneeDb) - levelDb; // <= 0
                followEnvelope (grDb, target, rel, atk); // the cut deepens at the attack rate
                maxGr = std::max (maxGr, -grDb);

                const float g = dbToGain (grDb + makeupDb);
                for (int c = 0; c < numCh; ++c)
                    ch[c][i] *= g;
            }

            meterDb.store (maxGr);
        }

        std::atomic<float> meterDb { 0.0f };

    private:
        static constexpr float kneeDb = 6.0f;

        double fs = 44100.0;
        float amount = 0.35f;
        int style = smooth;
        float thresholdDb = -18.0f, ratio = 3.0f, makeupDb = 0.0f;
        float atk = 0.0f, rel = 0.0f, rmsCoef = 0.0f;
        float rms = 0.0f, grDb = 0.0f;
        bool rmsDetect = true;
    };
}
