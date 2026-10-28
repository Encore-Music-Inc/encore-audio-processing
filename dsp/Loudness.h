#pragma once

#include <atomic>
#include <cmath>
#include <vector>
#include "Biquad.h"

namespace vs
{
    // Loudness meter per ITU-R BS.1770-4 / EBU R128: K-weighting, momentary
    // (400 ms), short-term (3 s) and gated integrated loudness, in LUFS.
    // This is the measurement streaming services use to set playback level.
    class LoudnessMeter
    {
    public:
        void prepare (double sampleRate)
        {
            fs = sampleRate;
            // K-weighting stage 1: high shelf (head acoustics)
            {
                const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
                const double K = std::tan (juce::MathConstants<double>::pi * f0 / fs);
                const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
                const double a0 = 1.0 + K / Q + K * K;
                shelf.c = { (Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                            2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
            }
            // stage 2: high pass (RLB weighting)
            {
                const double f0 = 38.13547087602444, Q = 0.5003270373238773;
                const double K = std::tan (juce::MathConstants<double>::pi * f0 / fs);
                const double a0 = 1.0 + K / Q + K * K;
                hp.c = { 1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
            }
            subLen = std::max (1, (int) std::round (fs * 0.1));
            blocks.assign (maxBlocks, 0.0f);
            reset();
        }

        void reset()
        {
            shelf.reset(); hp.reset();
            subAcc = 0.0; subCount = 0;
            for (auto& s : sub) s = 0.0;
            subIndex = 0; subFilled = 0;
            numBlocks.store (0);
            momentary.store (-100.0f); shortTerm.store (-100.0f);
        }

        void process (const float* l, const float* r, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                const double kl = hp.process (0, shelf.process (0, l[i]));
                const double kr = r != nullptr ? hp.process (1, shelf.process (1, r[i])) : 0.0;
                subAcc += kl * kl + kr * kr;
                if (++subCount >= subLen) closeSubBlock();
            }
        }

        std::atomic<float> momentary { -100.0f }, shortTerm { -100.0f };

        // Gated integrated loudness since the last reset (safe to call from the UI thread)
        float integrated() const
        {
            const int count = numBlocks.load();
            if (count == 0) return -100.0f;
            double sum = 0.0; int n = 0;
            for (int i = 0; i < count; ++i)
                if (blocks[(size_t) i] > -70.0f) { sum += std::pow (10.0, (blocks[(size_t) i] + 0.691) / 10.0); ++n; }
            if (n == 0) return -100.0f;
            const double relGate = -0.691 + 10.0 * std::log10 (sum / n) - 10.0;
            sum = 0.0; n = 0;
            for (int i = 0; i < count; ++i)
                if (blocks[(size_t) i] > -70.0f && blocks[(size_t) i] > relGate) { sum += std::pow (10.0, (blocks[(size_t) i] + 0.691) / 10.0); ++n; }
            return n == 0 ? -100.0f : (float) (-0.691 + 10.0 * std::log10 (sum / n));
        }

        static float toLufs (double meanSquareSum) { return (float) (-0.691 + 10.0 * std::log10 (meanSquareSum + 1e-20)); }

    private:
        void closeSubBlock()
        {
            sub[subIndex] = subAcc / subCount;
            subIndex = (subIndex + 1) % 30;
            subFilled = std::min (subFilled + 1, 30);
            subAcc = 0.0; subCount = 0;

            auto meanOf = [this] (int count)
            {
                double s = 0.0;
                for (int k = 1; k <= count; ++k) s += sub[(subIndex - k + 30) % 30];
                return s / count;
            };
            if (subFilled >= 4)
            {
                const float m = toLufs (meanOf (4));
                momentary.store (m);
                // every 100 ms hop, one 400 ms gating block (75% overlap)
                const int c = numBlocks.load();
                if (c < maxBlocks) { blocks[(size_t) c] = m; numBlocks.store (c + 1); }
            }
            if (subFilled >= 30) shortTerm.store (toLufs (meanOf (30)));
            else if (subFilled >= 4) shortTerm.store (toLufs (meanOf (subFilled)));
        }

        static constexpr int maxBlocks = 36000; // one hour of 100 ms hops
        double fs = 48000.0;
        Biquad shelf, hp;
        double subAcc = 0.0, sub[30] {};
        int subLen = 4800, subCount = 0, subIndex = 0, subFilled = 0;
        std::vector<float> blocks;
        std::atomic<int> numBlocks { 0 };
    };

    // Brickwall true-peak limiter. The audio itself stays at the normal rate
    // and is only ever turned down smoothly (which can't create new peaks);
    // a 4x-oversampled copy runs alongside purely to see the peaks that land
    // *between* samples, the ones a DAC or an MP3/AAC encoder turns into
    // clipping. Look-ahead lets it turn down before a peak arrives.
    class TruePeakLimiter
    {
    public:
        void prepare (double sampleRate, int maxBlock)
        {
            os.initProcessing ((size_t) maxBlock);
            os.reset();
            look = std::max (4, (int) std::round (0.0015 * sampleRate));
            detLatency = measureDetectorLatency (maxBlock);
            // one sample of slack each side of the detector alignment
            delayLen = look + detLatency + 1;
            window = look + 4;
            for (auto& d : delay) d.assign ((size_t) delayLen, 0.0f);
            boxBuf.assign ((size_t) look, 1.0f);
            minQ.assign ((size_t) window + 4, { 0, 1.0f });
            scratch.setSize (2, maxBlock);
            relCoef = (float) (1.0 - std::exp (-1.0 / (0.08 * sampleRate)));
            fs = sampleRate;
            reset();
        }

        void reset()
        {
            for (auto& d : delay) std::fill (d.begin(), d.end(), 0.0f);
            std::fill (boxBuf.begin(), boxBuf.end(), 1.0f);
            boxSum = (double) look; boxIdx = 0; dIdx = 0;
            qHead = qTail = 0; t = 0;
            relState = 1.0f;
            os.reset();
            grDb.store (0.0f);
        }

        int getLatency() const { return delayLen; }

    private:
        // Send a click through the up-sampler and see where it comes out:
        // exact for any oversampling filter, no assumptions about its delay.
        int measureDetectorLatency (int maxBlock)
        {
            const int len = std::max (1, std::min (maxBlock, 64));
            juce::AudioBuffer<float> probe (2, len);
            int found = 0, offset = 0;
            float best = 0.0f;
            os.reset();
            for (int block = 0; block < 16; ++block)
            {
                for (int c = 0; c < 2; ++c)
                {
                    float* p = probe.getWritePointer (c);
                    for (int i = 0; i < len; ++i) p[i] = (block == 0 && i == 0) ? 1.0f : 0.0f;
                }
                juce::dsp::AudioBlock<float> b (probe.getArrayOfWritePointers(), 2, (size_t) len);
                auto up = os.processSamplesUp (b);
                for (int i = 0; i < (int) up.getNumSamples(); ++i)
                    if (std::abs (up.getSample (0, i)) > best) { best = std::abs (up.getSample (0, i)); found = offset + i; }
                offset += (int) up.getNumSamples();
            }
            os.reset();
            return (int) std::round (found / 4.0);
        }

    public:

        // 0.3 dB of margin: a 4x detector can under-read the very top octave, and
        // the ceiling should hold even when measured more strictly than BS.1770
        void setCeilingDb (float db) { ceiling = std::pow (10.0f, db / 20.0f); target = ceiling * 0.966f; }
        void setReleaseMs (float ms, double) { relCoef = (float) (1.0 - std::exp (-1.0 / (ms * 0.001 * fs))); }

        void process (juce::dsp::AudioBlock<float>& block)
        {
            const int n = (int) block.getNumSamples(), nch = (int) std::min ((size_t) 2, block.getNumChannels());
            if (scratch.getNumSamples() < n) scratch.setSize (2, n, false, false, true);

            // detection path: 4x upsampled copy, max |x| over each sample's 4 sub-samples
            for (int c = 0; c < 2; ++c)
                scratch.copyFrom (c, 0, block.getChannelPointer ((size_t) std::min (c, nch - 1)), n);
            juce::dsp::AudioBlock<float> sb (scratch.getArrayOfWritePointers(), 2, (size_t) n);
            auto up = os.processSamplesUp (sb);

            float minGain = 1.0f;
            for (int i = 0; i < n; ++i)
            {
                float peak = 0.0f;
                for (int c = 0; c < 2; ++c)
                    for (int k = 0; k < 4; ++k) peak = std::max (peak, std::abs (up.getSample (c, i * 4 + k)));
                const float need = peak > target ? target / peak : 1.0f;

                // sliding minimum of the required gain
                const int qs = (int) minQ.size();
                while (qTail != qHead && minQ[(size_t) ((qTail - 1 + qs) % qs)].second >= need)
                    qTail = (qTail - 1 + qs) % qs;
                minQ[(size_t) qTail] = { t, need };
                qTail = (qTail + 1) % qs;
                while (minQ[(size_t) qHead].first < t - window) qHead = (qHead + 1) % qs;
                const float held = minQ[(size_t) qHead].second;

                relState = held < relState ? held : relState + relCoef * (held - relState);

                // box average: the gain is fully down before the peak leaves the delay
                boxSum += relState - boxBuf[(size_t) boxIdx];
                boxBuf[(size_t) boxIdx] = relState;
                boxIdx = (boxIdx + 1) % look;
                const float g = (float) std::min (1.0, boxSum / look);
                minGain = std::min (minGain, g);

                for (int c = 0; c < nch; ++c)
                {
                    auto& d = delay[c];
                    const float delayed = d[(size_t) dIdx];
                    auto* p = block.getChannelPointer ((size_t) c);
                    d[(size_t) dIdx] = p[i];
                    p[i] = juce::jlimit (-ceiling, ceiling, delayed * g); // safety net, normally never touches
                }
                dIdx = (dIdx + 1) % delayLen;
                ++t;
            }
            grDb.store (20.0f * std::log10 (std::max (minGain, 1e-6f)));
        }

        std::atomic<float> grDb { 0.0f };

    private:
        juce::dsp::Oversampling<float> os { 2, 2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true };
        juce::AudioBuffer<float> scratch;
        std::vector<float> delay[2], boxBuf;
        std::vector<std::pair<juce::int64, float>> minQ;
        int look = 72, detLatency = 0, delayLen = 72, window = 74, boxIdx = 0, dIdx = 0, qHead = 0, qTail = 0;
        juce::int64 t = 0;
        double boxSum = 0.0, fs = 48000.0;
        float ceiling = 0.891f, target = 0.88f, relCoef = 0.001f, relState = 1.0f;
    };
}
