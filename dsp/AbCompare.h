#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include "Loudness.h"
#include "Util.h"
#include "DelayLine.h"
#include "Mix.h"
#include "Ramp.h"

namespace vs
{
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
    //
    // Two renders compared after the fact (a whole mastered song against its
    // mix) can instead set a fixed match: setFixedMatch (gainDb) plays the
    // processed version at that gain (the difference the caller measured over
    // the whole of both) and the original as it is; setNoMatch() plays both as
    // they are. setLiveMatch() returns to matching as described above.
    class AbCompare
    {
    public:
        void prepare (double sampleRate, int maxBlock, int latencySamples)
        {
            fs = sampleRate;
            originalMeter.prepare (fs);
            processedMeter.prepare (fs);
            for (auto& o : original) o.assign ((size_t) std::max (1, maxBlock), 0.0f);
            delayLen = std::max (0, latencySamples);
            for (auto& d : delay) d.prepare (std::max (1, delayLen));
            matchGain.reset (fs, 0.5);
            processedGain.reset (fs, 0.03);
            processedGain.setCurrentAndTarget (match == Match::fixed ? processedGain.getTarget() : 1.0f);
            mixRamp.reset (fs, 0.03);
            mixRamp.setCurrentAndTarget (hearOriginal ? 0.0f : 1.0f);
        }

        // true: play the (matched) original
        void setHearOriginal (bool on) { hearOriginal = on; }

        void setLiveMatch() { match = Match::live; processedGain.setTarget (1.0f); }
        void setFixedMatch (float processedGainDb) { match = Match::fixed; processedGain.setTarget (dbToGain (processedGainDb)); matchGain.setTarget (1.0f); }
        void setNoMatch() { match = Match::none; processedGain.setTarget (1.0f); matchGain.setTarget (1.0f); }
        bool isHearingOriginal() const { return hearOriginal; }

        // The input before processing (r may be null for mono): delayed and measured.
        void takeOriginal (const float* l, const float* r, int n)
        {
            if ((int) original[0].size() < n) for (auto& o : original) o.resize ((size_t) n);
            originalMeter.process (l, r != nullptr ? r : l, n);
            const float* in[2] { l, r != nullptr ? r : l };
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                {
                    // with no latency, the original is the input as it is
                    if (delayLen == 0) { original[c][(size_t) i] = in[c][i]; continue; }
                    original[c][(size_t) i] = delay[c].at (delayLen);
                    delay[c].write (in[c][i]);
                }
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
            if (match == Match::live && processedShortTerm > -60.0f && origST > -60.0f)
                matchGain.setTarget (dbToGain (std::clamp (processedShortTerm - origST, -24.0f, 24.0f)));
            mixRamp.setTarget (hearOriginal ? 0.0f : 1.0f);
            float* ch[2] { l, r };
            const int nch = r != nullptr ? 2 : 1;
            if (match != Match::live && (processedGain.isRamping() || processedGain.getTarget() != 1.0f))
            {
                // a fixed match: the processed version at its gain, then the crossfade as below
                mix::applyGain (processedGain, ch, nch, n);
            }
            if (mixRamp.isRamping() || mixRamp.getTarget() < 0.5f)
            {
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
        std::vector<float> original[2];
        DelayLine delay[2];
        int delayLen = 0;
        bool hearOriginal = false;
        enum class Match { live, fixed, none };
        Match match = Match::live;
        Ramp<true> matchGain;
        Ramp<false> mixRamp, processedGain;
    };
}
