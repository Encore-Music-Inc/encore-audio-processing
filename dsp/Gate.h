#pragma once

#include <algorithm>
#include <cmath>
#include "Util.h"

namespace vs
{
    // Soft gate: closes down to -24 dB rather than full silence so it sounds
    // natural on breathy folk takes, with hold + hysteresis to avoid chatter.
    class Gate
    {
    public:
        void prepare (double sampleRate)
        {
            relCoef = onePole (60.0f, sampleRate);
            openCoef = onePole (0.5f, sampleRate);
            closeCoef = onePole (120.0f, sampleRate);
            holdSamples = (int) (0.04 * sampleRate);
            env = 0.0f; gain = 1.0f; holdCount = 0;
        }

        void setThreshold (float db)
        {
            enabled = db > -89.5f;
            openThr = dbToGain (db);
            closeThr = openThr * 0.63f; // 4 dB hysteresis
        }

        void process (float* const* ch, int numCh, int numSamples)
        {
            if (! enabled)
            {
                gain = 1.0f;
                return;
            }

            for (int i = 0; i < numSamples; ++i)
            {
                float peak = 0.0f;
                for (int c = 0; c < numCh; ++c)
                    peak = std::max (peak, std::abs (ch[c][i]));

                env = peak > env ? peak : env + relCoef * (peak - env);

                if (env > openThr)            { open = true; holdCount = holdSamples; }
                else if (env < closeThr)      { if (holdCount > 0) --holdCount; else open = false; }

                const float target = open ? 1.0f : floorGain;
                gain += (target > gain ? openCoef : closeCoef) * (target - gain);

                for (int c = 0; c < numCh; ++c)
                    ch[c][i] *= gain;
            }
        }

    private:
        static constexpr float floorGain = 0.063f; // -24 dB
        bool enabled = false, open = true;
        float openThr = 0.0f, closeThr = 0.0f;
        float relCoef = 0.0f, openCoef = 0.0f, closeCoef = 0.0f;
        float env = 0.0f, gain = 1.0f;
        int holdSamples = 0, holdCount = 0;
    };
}
