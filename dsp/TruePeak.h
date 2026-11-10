#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "Oversampler.h"

namespace vs
{
    // The true-peak detector: the one way ENCORE. sees the peaks that land
    // between samples (the ones a DAC or an MP3/AAC encoder turns into
    // clipping). The signal is upsampled 4x by an Oversampler with these
    // settings and the largest |x| of the upsampled copy is the true peak.
    // TruePeakLimiter (Loudness.h) holds its ceiling with this detector and
    // TruePeakMeter below reads with it, so a limited master and its meter
    // can't disagree.
    struct TruePeakDetector
    {
        static constexpr int factorLog2 = 2;
        static constexpr int factor = 1 << factorLog2;
        // 127 taps, Kaiser beta 10: keeps the top octave, so peaks there are seen
        static constexpr int taps = 127;
        static constexpr double kaiserBeta = 10.0;

        template <typename T>
        static Oversampler<T> makeOversampler (int numChannels)
        {
            return Oversampler<T> (numChannels, factorLog2, taps, kaiserBeta);
        }

        // The upsampler's delay, in samples of the upsampled copy: each 2x
        // stage delays by (taps - 1) / 2 samples at its own rate.
        static constexpr int upLatency()
        {
            int lat = 0;
            for (int s = 0; s < factorLog2; ++s) lat += ((taps - 1) / 2) << (factorLog2 - 1 - s);
            return lat;
        }

        // The same delay in input samples, rounded: sub-samples 4g .. 4g + 3
        // of the upsampled copy (the group the limiter reads per sample)
        // belong to input sample g - groupLatency().
        static constexpr int groupLatency() { return (upLatency() + factor / 2) / factor; }

        // How many silent samples after the end let the upsampler's tail out.
        static constexpr int tailSamples() { return (2 * upLatency()) / factor + 2; }
    };

    // True-peak meter (ITU-R BS.1770 style, 4x oversampled) of up to
    // maxChannels channels, measured block by block: each channel's true peak
    // and sample peak, and where the loudest true peak is. It never reads
    // below the sample peak. finish() lets the upsampler's tail out at the end
    // of a file (a live meter never needs it). reset() clears the reading (a
    // held peak) and the filters.
    class TruePeakMeter
    {
    public:
        static constexpr int maxChannels = 2;

        // Allocates; call it off the audio thread. Blocks of any length can be
        // processed: longer ones are measured maxBlock samples at a time.
        void prepare (int numChannels, int maxBlock)
        {
            channels = std::max (1, std::min (maxChannels, numChannels));
            block = std::max (1, maxBlock);
            os = TruePeakDetector::makeOversampler<float> (channels);
            os.initProcessing (block);
            zeros.assign ((size_t) block, 0.0f);
            reset();
        }

        void reset()
        {
            os.reset();
            for (int c = 0; c < maxChannels; ++c) { tp[c] = 0.0f; sp[c] = 0.0f; }
            groups = 0;
            frames = 0;
            peakAt = 0;
            spAt = 0;
            best = 0.0f;
            bestSample = 0.0f;
        }

        // Measures n samples of each channel (ch[0 .. numChannels)).
        void process (const float* const* ch, int n) { process (ch, n, [] (std::int64_t, float) {}); }

        // The same, and calls onSample (position, peak) for each input sample
        // with that sample's true peak over every channel (the largest |x| of
        // its four sub-samples). Positions arrive in order, delayed by the
        // upsampler (TruePeakDetector::groupLatency samples); finish() brings
        // the last ones out. Positions past the end are reported as the last
        // sample.
        template <typename OnSample>
        void process (const float* const* ch, int n, OnSample&& onSample)
        {
            for (int done = 0; done < n;)
            {
                const int len = std::min (block, n - done);
                const float* in[maxChannels] {};
                for (int c = 0; c < channels; ++c)
                {
                    in[c] = ch[c] + done;
                    for (int i = 0; i < len; ++i)
                    {
                        const float a = std::abs (in[c][i]);
                        if (a > sp[c]) sp[c] = a;
                        if (a > bestSample) { bestSample = a; spAt = frames + i; }
                    }
                }
                upsample (in, len, frames + len - 1, onSample);
                frames += len;
                done += len;
            }
        }

        // At the end of a file: lets the upsampler's tail out (the ringing
        // after the last sample is part of what a DAC plays).
        void finish() { finish ([] (std::int64_t, float) {}); }

        template <typename OnSample>
        void finish (OnSample&& onSample)
        {
            const float* in[maxChannels] {};
            for (int c = 0; c < channels; ++c) in[c] = zeros.data();
            for (int left = TruePeakDetector::tailSamples(); left > 0;)
            {
                const int len = std::min (block, left);
                upsample (in, len, std::max<std::int64_t> (frames - 1, 0), onSample);
                left -= len;
            }
        }

        int numChannels() const { return channels; }

        // Linear, never below the sample peak; the largest of the channels
        // for channel -1.
        float truePeak (int channel = -1) const
        {
            if (channel >= 0) return std::max (tp[channel], sp[channel]);
            float m = 0.0f;
            for (int c = 0; c < channels; ++c) m = std::max (m, std::max (tp[c], sp[c]));
            return m;
        }

        float samplePeak (int channel = -1) const
        {
            if (channel >= 0) return sp[channel];
            float m = 0.0f;
            for (int c = 0; c < channels; ++c) m = std::max (m, sp[c]);
            return m;
        }

        // dBTP of truePeak(); -inf for silence
        float truePeakDb (int channel = -1) const { return toDb (truePeak (channel)); }

        // The sample the loudest true peak belongs to (the sample peak's own
        // position when no in-between peak is higher).
        std::int64_t truePeakPosition() const { return best > bestSample ? peakAt : spAt; }
        std::int64_t samplePeakPosition() const { return spAt; }
        std::int64_t samplesMeasured() const { return frames; }

        static float toDb (float linear) { return 20.0f * std::log10 (linear); }

    private:
        // lastValid: the last input sample measured so far, where positions
        // past the end are put.
        template <typename OnSample>
        void upsample (const float* const* in, int len, std::int64_t lastValid, OnSample& onSample)
        {
            auto* const* up = os.processSamplesUp (in, channels, len);
            constexpr int f = TruePeakDetector::factor;
            for (int i = 0; i < len; ++i, ++groups)
            {
                float peak = 0.0f;
                for (int c = 0; c < channels; ++c)
                {
                    float pc = 0.0f;
                    for (int k = 0; k < f; ++k) pc = std::max (pc, std::abs (up[c][i * f + k]));
                    tp[c] = std::max (tp[c], pc);
                    peak = std::max (peak, pc);
                }
                const std::int64_t at = groups - TruePeakDetector::groupLatency();
                if (at < 0) continue;
                const std::int64_t pos = std::min (at, lastValid);
                if (peak > best) { best = peak; peakAt = pos; }
                onSample (pos, peak);
            }
        }

        Oversampler<float> os = TruePeakDetector::makeOversampler<float> (maxChannels);
        std::vector<float> zeros;
        int channels = maxChannels, block = 512;
        float tp[maxChannels] {}, sp[maxChannels] {};
        float best = 0.0f, bestSample = 0.0f;
        std::int64_t groups = 0, frames = 0, peakAt = 0, spAt = 0;
    };

    // The true peak (linear) of one channel of a whole file, tail included.
    inline float truePeakOf (const float* x, std::int64_t n)
    {
        TruePeakMeter m;
        m.prepare (1, 8192);
        const float* ch[1] { x };
        for (std::int64_t s = 0; s < n; s += 8192)
        {
            ch[0] = x + s;
            m.process (ch, (int) std::min<std::int64_t> (8192, n - s));
        }
        m.finish();
        return m.truePeak();
    }
}
