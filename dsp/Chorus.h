#pragma once

#include <algorithm>
#include <cmath>
#include "Channels.h"
#include "DelayLine.h"

namespace vs
{
    // A chorus: each channel plus a copy of itself delayed by a slowly
    // sweeping amount (a sine LFO between baseMs and baseMs + depthMs), the
    // right channel's sweep ahead of the left's by width x half a cycle,
    // blended with the dry signal by an equal-power crossfade (mix 0 dry, 1
    // wet). Its settings are Tracktion Engine's chorus (GPLv3), rewritten.
    class Chorus
    {
    public:
        struct Settings
        {
            float depthMs = 3.0f;   // how far the delay sweeps
            float speedHz = 1.0f;   // how fast
            float width = 0.5f;     // 0 both channels together .. 1 opposite
            float mix = 0.5f;       // 0 dry .. 1 wet
        };
        static constexpr float baseMs = 20.0f, maxDepthMs = 20.0f;

        void prepare (double sampleRate)
        {
            fs = sampleRate;
            for (auto& d : delays) { d.prepare ((int) std::ceil ((baseMs + maxDepthMs + 2.0f) * 0.001 * fs)); d.clear(); }
            phase = 0.0;
        }

        void reset() { for (auto& d : delays) d.clear(); phase = 0.0; }
        void setSettings (const Settings& s) { set = s; set.depthMs = std::clamp (s.depthMs, 0.0f, maxDepthMs); }
        const Settings& getSettings() const { return set; }

        // numCh channels (1 or 2) of n samples, in place
        void process (float* const* ch, int numCh, int n)
        {
            constexpr double twoPi = 6.283185307179586;
            const double step = twoPi * set.speedHz / fs;
            const float minS = (float) (baseMs * 0.001 * fs), maxS = (float) ((baseMs + set.depthMs) * 0.001 * fs);
            const float half = 0.5f * (maxS - minS), centre = minS + half;
            const float wet = (float) channels::fadeGain (channels::FadeCurve::quarterSine, set.mix);
            const float dry = (float) channels::fadeOutGain (channels::FadeCurve::quarterSine, set.mix);
            const int nc = std::clamp (numCh, 0, 2);
            for (int i = 0; i < n; ++i)
            {
                for (int c = 0; c < nc; ++c)
                {
                    const double ph = phase + (c == 1 ? 3.141592653589793 * set.width : 0.0);
                    const float delay = centre + half * (float) std::sin (ph);
                    auto& d = delays[(size_t) c];
                    const float in = ch[c][i];
                    d.write (in);
                    const float out = d.read (delay);
                    ch[c][i] = out * wet + in * dry;
                }
                phase += step;
                if (phase >= twoPi) phase -= twoPi;
            }
        }

    private:
        double fs = 48000.0, phase = 0.0;
        Settings set;
        DelayLine delays[2];
    };
}
