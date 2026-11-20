#pragma once

#include <vector>
#include "AbCompare.h"
#include "DeviceSim.h"
#include "MasterChain.h"
#include "Mix.h"
#include "Ramp.h"
#include "Spectrum.h"
#include "Util.h"

namespace vs
{
    // A song mastered as you listen (Master in the browser): block by block,
    // the song through vs::MasterChain; then "hear the original"
    // (vs::AbCompare: the original lined up with the chain's latency and
    // played at the master's loudness); then the level a streaming service
    // would play it at; then a playback device (vs::DeviceSim). A live
    // spectrum (vs::SpectrumAnalyser, the mid of what comes out) shows what is
    // heard.
    //
    // Playback level off: the original is matched to the master live, and
    // nothing is turned up or down. On: each plays at a service's level, the
    // master at masterDb and the original at originalDb (the caller works
    // both out from the service's rule, ms_playback_gain); the level glides
    // over 20 ms.
    class MasterMonitor
    {
    public:
        void prepare (double sampleRate, int maxBlock, const SpectrumAnalyser::Options& spectrumOptions = SpectrumAnalyser::webAudio (8192, 0.75f))
        {
            fs = sampleRate;
            chain.prepare (fs, maxBlock);
            ab.prepare (fs, maxBlock, chain.getLatency());
            device.prepare (fs);
            level.reset (fs, 0.02);
            level.setCurrentAndTarget (1.0f);
            spectrum.prepare (spectrumOptions);
            mid.assign ((size_t) std::max (1, maxBlock), 0.0f);
        }

        void setHearOriginal (bool on) { ab.setHearOriginal (on); }

        void setPlaybackLevel (bool on, float masterDb = 0.0f, float originalDb = 0.0f)
        {
            if (! on) { ab.setLiveMatch(); level.setTarget (1.0f); return; }
            ab.setFixedMatch (masterDb - originalDb);
            level.setTarget (dbToGain (originalDb));
        }

        void setDevice (int i) { device.setDevice (i); }

        // Stereo, in place (a mono song comes in as the same samples on both channels).
        void process (float* l, float* r, int n)
        {
            ab.takeOriginal (l, r, n);
            chain.process (l, r, n);
            ab.mix (l, r, n, chain.outMeter.shortTerm.load());
            float* ch[2] { l, r };
            mix::applyGain (level, ch, 2, n);
            device.process (l, r, n);
            if ((int) mid.size() < n) mid.resize ((size_t) n);
            for (int i = 0; i < n; ++i) mid[(size_t) i] = midOf (l[i], r[i]);
            spectrum.push (mid.data(), n);
        }

        MasterChain chain;
        AbCompare ab;
        DeviceSim device;
        SpectrumAnalyser spectrum;

    private:
        double fs = 48000.0;
        Ramp<false> level;
        std::vector<float> mid;
    };
}
