#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
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
        }

        void process (float* const* ch, int numCh, int numSamples)
        {
            float maxGr = 0.0f;

            for (int i = 0; i < numSamples; ++i)
            {
                float peak = 0.0f, sq = 0.0f;
                for (int c = 0; c < numCh; ++c)
                {
                    peak = std::max (peak, std::abs (ch[c][i]));
                    sq = std::max (sq, ch[c][i] * ch[c][i]);
                }

                rms += rmsCoef * (sq - rms);
                const float levelDb = style == smooth ? powerToDb (rms) + 3.0f : ampToDb (peak);

                const float target = computeGain (levelDb) - levelDb; // <= 0
                grDb += (target < grDb ? atk : rel) * (target - grDb);
                maxGr = std::max (maxGr, -grDb);

                const float g = dbToGain (grDb + makeupDb);
                for (int c = 0; c < numCh; ++c)
                    ch[c][i] *= g;
            }

            meterDb.store (maxGr);
        }

        std::atomic<float> meterDb { 0.0f };

    private:
        float computeGain (float x) const
        {
            constexpr float knee = 6.0f;
            const float d = x - thresholdDb;
            if (2.0f * d < -knee) return x;
            if (2.0f * std::abs (d) <= knee)
            {
                const float t = d + knee * 0.5f;
                return x + (1.0f / ratio - 1.0f) * t * t / (2.0f * knee);
            }
            return thresholdDb + d / ratio;
        }

        double fs = 44100.0;
        float amount = 0.35f;
        int style = smooth;
        float thresholdDb = -18.0f, ratio = 3.0f, makeupDb = 0.0f;
        float atk = 0.0f, rel = 0.0f, rmsCoef = 0.0f;
        float rms = 0.0f, grDb = 0.0f;
    };
}
