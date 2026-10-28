#pragma once

#include <algorithm>
#include <atomic>
#include "Util.h"

namespace vs
{
    // Automatic fader rider. Measures the vocal's RMS level and moves a gain
    // toward the target, limited to +/- range. When the singer stops (level far
    // below target) it holds its last position instead of cranking up the
    // room noise, so breaths and bleed don't get pulled up between phrases.
    class Rider
    {
    public:
        void prepare (double sampleRate)
        {
            fs = sampleRate;
            detCoef = onePole (30.0f, fs);
            reset();
        }

        void reset()
        {
            env = 1.0e-6f;
            gainDb = desiredDb = 0.0f;
        }

        void setParams (float newTargetDb, float newRangeDb, float speed01)
        {
            targetDb = newTargetDb;
            rangeDb = newRangeDb;
            // speed 0 = slow, gentle rides (600 ms), 1 = fast, word-by-word (60 ms)
            moveCoef = onePole (600.0f * std::pow (0.1f, speed01), fs);
        }

        // `follow` channels (e.g. harmony voices) get the same gain moves but
        // don't affect the measurement.
        void process (float* const* ch, int numCh, int numSamples, float* const* follow = nullptr, int numFollow = 0)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                float sq = 0.0f;
                for (int c = 0; c < numCh; ++c)
                    sq = std::max (sq, ch[c][i] * ch[c][i]);

                env += detCoef * (sq - env);
                const float levelDb = powerToDb (env);

                if (levelDb > targetDb - 30.0f)
                    desiredDb = std::clamp (targetDb - levelDb, -rangeDb, rangeDb);

                gainDb += moveCoef * (desiredDb - gainDb);
                const float g = dbToGain (gainDb);

                for (int c = 0; c < numCh; ++c)
                    ch[c][i] *= g;
                for (int c = 0; c < numFollow; ++c)
                    follow[c][i] *= g;
            }

            meterDb.store (gainDb);
        }

        std::atomic<float> meterDb { 0.0f };

    private:
        double fs = 44100.0;
        float detCoef = 0.0f, moveCoef = 0.0f;
        float env = 0.0f, gainDb = 0.0f, desiredDb = 0.0f;
        float targetDb = -18.0f, rangeDb = 6.0f;
    };
}
