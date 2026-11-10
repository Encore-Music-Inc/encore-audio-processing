#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "Loudness.h"
#include "TruePeak.h"

namespace vs
{
    // A song's loudness report, the figures streaming services and mastering
    // engineers read (ITU-R BS.1770, EBU R128 and Tech 3342): integrated
    // loudness, the loudest 3 s (short-term) and 400 ms (momentary), the
    // loudness range, sample peak and true peak, and the balance between the
    // channels. Built on LoudnessMeter (its K-weighting, gating and recorded
    // 100 ms block energies) and TruePeakMeter. One channel is measured as one
    // channel, as BS.1770 says (not as the same signal on both sides).
    //
    // dB figures: LUFS for loudness, LU for the range, dBFS / dBTP for peaks
    // (20 log10 (x + 1e-12)). Silence reads -100 LUFS.
    struct LoudnessReport
    {
        double sampleRate = 48000.0;
        int channels = 0;
        std::int64_t frames = 0;

        double integrated = -100.0;     // gated, LUFS
        double shortTermMax = -100.0;   // the loudest 3 s, LUFS
        double shortTermMaxAt = 0.0;    // its middle, seconds
        double momentaryMax = -100.0;   // the loudest 400 ms, LUFS
        double momentaryMaxAt = 0.0;    // its middle, seconds
        double range = 0.0;             // loudness range, LU
        double balanceDb = 0.0;         // left minus right over the blocks that count toward the integrated loudness
        std::vector<double> timeline;   // short-term loudness once a second, to 0.1 LU, -60 at the lowest

        double samplePeakDb = -240.0;
        double truePeakDb = -240.0;
        double truePeakAt = 0.0;        // seconds
        double peakDb[2] { -240.0, -240.0 };  // each channel's sample peak
        double rmsDb[2] { -200.0, -200.0 };   // each channel's RMS level, 10 log10 (mean square + 1e-20)
    };

    namespace loudness
    {
        inline double db10 (double x) { return 10.0 * std::log10 (x + 1e-20); }
        inline double db20 (double x) { return 20.0 * std::log10 (x + 1e-12); }

        // Rounds half toward +infinity (half up), as the browser does.
        inline double roundHalfUp (double x) { return std::floor (x + 0.5); }

        // The loudness figures that come from a meter's recorded block
        // energies (blockLength samples per block) and its integrated
        // loudness: short-term and momentary maxima, range, timeline, balance.
        // energyR is all zero for one channel.
        inline void fromBlocks (LoudnessReport& rep, const std::vector<double>& kL, const std::vector<double>& kR,
                                int blockLength, double integrated)
        {
            rep.integrated = integrated;
            const int nb = (int) kL.size();
            const double hop = blockLength;
            auto loud = [&] (int from, int count)
            {
                double e = 0.0;
                for (int k = from; k < from + count; ++k) e += kL[(size_t) k] + kR[(size_t) k];
                return -0.691 + 10.0 * std::log10 (e / (count * hop) + 1e-20);
            };

            // short-term: 30 blocks (3 s), every 100 ms; a song under 3 s is one window
            const int stLen = std::min (30, nb);
            std::vector<double> st;
            if (stLen > 0)
                for (int k = 0; k + stLen <= nb; ++k) st.push_back (loud (k, stLen));
            double stMax = -100.0, stMaxAt = 0.0;
            for (size_t k = 0; k < st.size(); ++k)
                if (st[k] > stMax) { stMax = st[k]; stMaxAt = ((double) k + stLen / 2.0) * 0.1; }
            rep.shortTermMax = st.empty() ? -100.0 : stMax;
            rep.shortTermMaxAt = stMaxAt;

            double mMax = -100.0, mMaxAt = 0.0;
            for (int k = 0; k + 4 <= nb; ++k)
            {
                const double m = loud (k, 4);
                if (m > mMax) { mMax = m; mMaxAt = (k + 2) * 0.1; }
            }
            rep.momentaryMax = mMax;
            rep.momentaryMaxAt = mMaxAt;

            // loudness range (EBU Tech 3342): the short-term values above -70
            // LUFS and within 20 LU of their mean, 10th to 95th percentile
            std::vector<double> absG;
            for (double v : st) if (v > -70.0) absG.push_back (v);
            rep.range = 0.0;
            if (absG.size() > 1)
            {
                double sum = 0.0;
                for (double v : absG) sum += std::pow (10.0, v / 10.0);
                const double mean = 10.0 * std::log10 (sum / (double) absG.size());
                std::vector<double> g;
                for (double v : absG) if (v > mean - 20.0) g.push_back (v);
                std::sort (g.begin(), g.end());
                if (g.size() > 1)
                {
                    const double last = (double) (g.size() - 1);
                    rep.range = g[(size_t) roundHalfUp (0.95 * last)] - g[(size_t) roundHalfUp (0.1 * last)];
                }
            }

            rep.timeline.clear();
            for (size_t k = 0; k < st.size(); k += 10)
                rep.timeline.push_back (std::max (-60.0, roundHalfUp (st[k] * 10.0) / 10.0));

            // left against right, over the 400 ms blocks that count toward the loudness
            double bL = 0.0, bR = 0.0;
            const double gate = std::max (-70.0, integrated - 10.0);
            for (int k = 0; k + 4 <= nb; ++k)
                if (loud (k, 4) > gate)
                    for (int j = k; j < k + 4; ++j) { bL += kL[(size_t) j]; bR += kR[(size_t) j]; }
            rep.balanceDb = bL > 0.0 && bR > 0.0 ? db10 (bL) - db10 (bR) : 0.0;
        }
    }

    // Peak and level per channel, for any number of channels (an audio
    // interface check, leaving out inputs with nothing on them): add blocks,
    // then read each channel's sample peak and RMS. Each block's sum of
    // squares is added up on its own first, then to the total.
    struct ChannelLevels
    {
        explicit ChannelLevels (int numChannels = 2) : peak ((size_t) std::max (1, numChannels), 0.0f), energy (peak.size(), 0.0) {}

        // A null channel pointer is skipped (it reads as silence).
        void add (const float* const* data, int channels, int samples)
        {
            for (int c = 0; c < std::min (channels, (int) peak.size()); ++c)
            {
                if (data[c] == nullptr) continue;
                float p = 0.0f;
                double e = 0.0;
                for (int i = 0; i < samples; ++i) { p = std::max (p, std::abs (data[c][i])); e += data[c][i] * data[c][i]; }
                peak[(size_t) c] = std::max (peak[(size_t) c], p);
                energy[(size_t) c] += e;
            }
            count += samples;
        }

        void reset() { std::fill (peak.begin(), peak.end(), 0.0f); std::fill (energy.begin(), energy.end(), 0.0); count = 0; }

        int numChannels() const { return (int) peak.size(); }
        float peakOf (int c) const { return peak[(size_t) c]; }
        float rmsOf (int c) const { return count > 0 ? (float) std::sqrt (energy[(size_t) c] / (double) count) : 0.0f; }
        float loudest() const { return *std::max_element (peak.begin(), peak.end()); }

        std::vector<float> peak;
        std::vector<double> energy;
        std::int64_t count = 0;
    };

    // Measures one or two channels block by block, then finish() makes the
    // report. Allocates as it goes (it keeps every 100 ms block's energy), so
    // it's for measuring files, not for a live meter.
    class LoudnessReporter
    {
    public:
        void prepare (double sampleRate, int numChannels, int maxBlock = 8192)
        {
            fs = sampleRate;
            channels = std::max (1, std::min (2, numChannels));
            meter.prepare (fs);
            meter.setRecordBlocks (true);
            peaks.prepare (channels, maxBlock);
            reset();
        }

        void reset()
        {
            meter.reset();
            peaks.reset();
            frames = 0;
            sumSq[0] = sumSq[1] = 0.0;
        }

        // r is read only for two channels.
        void process (const float* l, const float* r, int n)
        {
            if (channels < 2) r = nullptr;
            meter.process (l, r, n);
            const float* ch[2] { l, r };
            peaks.process (ch, n);
            for (int c = 0; c < channels; ++c)
            {
                double s = 0.0;
                for (int i = 0; i < n; ++i) s += (double) ch[c][i] * (double) ch[c][i];
                sumSq[c] += s;
            }
            frames += n;
        }

        LoudnessReport finish()
        {
            peaks.finish();
            LoudnessReport rep;
            rep.sampleRate = fs;
            rep.channels = channels;
            rep.frames = frames;
            loudness::fromBlocks (rep, meter.blockEnergy (0), meter.blockEnergy (1), meter.blockLength(), meter.integrated());
            rep.samplePeakDb = loudness::db20 (peaks.samplePeak());
            rep.truePeakDb = loudness::db20 (peaks.truePeak());
            rep.truePeakAt = (double) peaks.truePeakPosition() / fs;
            for (int c = 0; c < channels; ++c)
            {
                rep.peakDb[c] = loudness::db20 (peaks.samplePeak (c));
                rep.rmsDb[c] = loudness::db10 (frames > 0 ? sumSq[c] / (double) frames : 0.0);
            }
            return rep;
        }

    private:
        double fs = 48000.0;
        int channels = 2;
        LoudnessMeter meter;
        TruePeakMeter peaks;
        std::int64_t frames = 0;
        double sumSq[2] {};
    };

    // The report of a whole song: r null for one channel.
    inline LoudnessReport measureLoudness (const float* l, const float* r, std::int64_t n, double sampleRate)
    {
        LoudnessReporter m;
        m.prepare (sampleRate, r != nullptr ? 2 : 1);
        for (std::int64_t s = 0; s < n; s += 8192)
        {
            const int len = (int) std::min<std::int64_t> (8192, n - s);
            m.process (l + s, r != nullptr ? r + s : nullptr, len);
        }
        return m.finish();
    }
}
