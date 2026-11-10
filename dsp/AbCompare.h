#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include "Loudness.h"
#include "Util.h"

namespace vs
{
    // A value that ramps to a new target over a fixed number of samples,
    // linearly (equal steps) or multiplicatively (equal ratios, for gains).
    // A new target closer than float precision to the old one is ignored.
    template <bool multiplicative>
    class Ramp
    {
    public:
        void reset (double sampleRate, double seconds)
        {
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

    // ENCORE.'s loudness-matched "hear the original". The original is delayed
    // by the processing's latency so both line up; both are measured with the
    // BS.1770 short-term loudness (vs::LoudnessMeter, 3 s); the original is
    // played at the processed version's loudness (the match gain is processed
    // minus original, kept to +-24 dB and updated only while both are above
    // -60 LUFS, gliding over 0.5 s), so a comparison is about tone and punch,
    // not level. Switching between them crossfades over 30 ms.
    //
    // Per block: takeOriginal() with the input before it's processed, then
    // mix() with the processed audio (in place), which plays the processed,
    // the matched original, or the crossfade between them.
    class AbCompare
    {
    public:
        void prepare (double sampleRate, int maxBlock, int latencySamples)
        {
            fs = sampleRate;
            originalMeter.prepare (fs);
            processedMeter.prepare (fs);
            for (auto& o : original) o.assign ((size_t) std::max (1, maxBlock), 0.0f);
            delayLen = std::max (1, latencySamples);
            for (auto& d : delay) d.assign ((size_t) delayLen, 0.0f);
            idx = 0;
            matchGain.reset (fs, 0.5);
            mixRamp.reset (fs, 0.03);
            mixRamp.setCurrentAndTarget (hearOriginal ? 0.0f : 1.0f);
        }

        // true: play the (matched) original
        void setHearOriginal (bool on) { hearOriginal = on; }
        bool isHearingOriginal() const { return hearOriginal; }

        // The input before processing (r may be null for mono): delayed and measured.
        void takeOriginal (const float* l, const float* r, int n)
        {
            if ((int) original[0].size() < n) for (auto& o : original) o.resize ((size_t) n);
            originalMeter.process (l, r != nullptr ? r : l, n);
            const float* in[2] { l, r != nullptr ? r : l };
            for (int c = 0; c < 2; ++c)
            {
                int k = idx;
                for (int i = 0; i < n; ++i)
                {
                    original[c][(size_t) i] = delay[c][(size_t) k];
                    delay[c][(size_t) k] = in[c][i];
                    k = (k + 1) % delayLen;
                }
            }
            idx = (idx + n) % delayLen;
        }

        // The processed audio, in place: measures it (its short-term loudness
        // is the target), then plays it, the original or the crossfade.
        void mix (float* l, float* r, int n)
        {
            processedMeter.process (l, r != nullptr ? r : l, n);
            mix (l, r, n, processedMeter.shortTerm.load());
        }

        // The same with the processed audio's short-term loudness measured by
        // the caller (a chain that meters its own output).
        void mix (float* l, float* r, int n, float processedShortTerm)
        {
            const float origST = originalMeter.shortTerm.load();
            if (processedShortTerm > -60.0f && origST > -60.0f)
                matchGain.setTarget (dbToGain (std::clamp (processedShortTerm - origST, -24.0f, 24.0f)));
            mixRamp.setTarget (hearOriginal ? 0.0f : 1.0f);
            if (mixRamp.isRamping() || mixRamp.getTarget() < 0.5f)
            {
                float* ch[2] { l, r };
                const int nch = r != nullptr ? 2 : 1;
                for (int i = 0; i < n; ++i)
                {
                    const float m = mixRamp.next(), g = matchGain.next();
                    for (int c = 0; c < nch; ++c)
                        ch[c][i] = ch[c][i] * m + original[c][(size_t) i] * g * (1.0f - m);
                }
            }
            else
            {
                matchGain.skip (n);
            }
        }

        // The current match gain, dB (what "hear the original" adds to the original)
        float matchGainDb() const { return ampToDb (std::max (1e-9f, matchGain.getCurrent()), 0.0f); }
        float originalShortTerm() const { return originalMeter.shortTerm.load(); }
        float processedShortTerm() const { return processedMeter.shortTerm.load(); }

    private:
        double fs = 48000.0;
        LoudnessMeter originalMeter, processedMeter;
        std::vector<float> original[2], delay[2];
        int delayLen = 1, idx = 0;
        bool hearOriginal = false;
        Ramp<true> matchGain;
        Ramp<false> mixRamp;
    };
}
