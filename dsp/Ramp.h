#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace vs
{
    // A value that ramps to a new target over a fixed number of samples,
    // linearly (equal steps) or multiplicatively (equal ratios, for gains),
    // number for number as JUCE's SmoothedValue does (a multiplicative ramp
    // starts at 1). A new target closer than float precision to the old one
    // is ignored. The browser build's juce::SmoothedValue (lite/JuceLite.h)
    // is this ramp.
    template <bool multiplicative>
    class Ramp
    {
    public:
        // (a time or rate of 0 or less leaves everything as it is)
        void reset (double sampleRate, double seconds)
        {
            if (! (sampleRate > 0 && seconds > 0)) return;
            stepsToTarget = (int) std::floor (seconds * sampleRate);
            setCurrentAndTarget (target);
        }

        void setCurrentAndTarget (float v) { target = current = v; countdown = 0; }

        void setTarget (float v)
        {
            if (nearlyEqual (v, target)) return;
            if (stepsToTarget <= 0) { setCurrentAndTarget (v); return; }
            target = v;
            countdown = stepsToTarget;
            if constexpr (multiplicative)
                step = std::exp ((std::log (std::abs (target)) - std::log (std::abs (current))) / (float) countdown);
            else
                step = (target - current) / (float) countdown;
        }

        float next()
        {
            if (countdown <= 0) return target;
            --countdown;
            if (countdown > 0)
            {
                if constexpr (multiplicative) current *= step; else current += step;
            }
            else
            {
                current = target;
            }
            return current;
        }

        float skip (int n)
        {
            if (n >= countdown) { setCurrentAndTarget (target); return target; }
            if constexpr (multiplicative) current *= (float) std::pow (step, n); else current += step * (float) n;
            countdown -= n;
            return current;
        }

        bool isRamping() const { return countdown > 0; }
        float getTarget() const { return target; }
        float getCurrent() const { return current; }

    private:
        static bool nearlyEqual (float a, float b)
        {
            if (! (std::isfinite (a) && std::isfinite (b))) return a == b;
            const float d = std::abs (a - b);
            return d <= std::numeric_limits<float>::min() || d <= std::numeric_limits<float>::epsilon() * std::max (std::abs (a), std::abs (b));
        }

        float current = multiplicative ? 1.0f : 0.0f, target = current, step = 0.0f;
        int countdown = 0, stepsToTarget = 0;
    };
}
