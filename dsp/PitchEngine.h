#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <vector>
#include "PitchDetector.h"
#include "Scales.h"
#include "Util.h"

namespace vs
{
    // Real-time TD-PSOLA pitch engine for a monophonic voice.
    //
    // The input is cut into "grains", one per vocal-cord pulse (pitch period),
    // each two periods long and centred on a waveform peak. To change pitch,
    // the grains are laid back down closer together (higher) or further apart
    // (lower). Because each grain keeps its own shape, the voice keeps its
    // formants: harmonies sound like another singer, not a chipmunk.
    //
    // Voice 0 is the lead (pitch correction). Voices 1-2 are harmonies that
    // share the same analysis but pick their own target notes.
    class PitchEngine
    {
    public:
        struct HarmonyVoice
        {
            int interval = 0;       // index into scales::intervalNames, 0 = off
            float levelDb = -6.0f;
            float pan = 0.0f;       // -1..1
        };

        struct Settings
        {
            bool tuneOn = false;
            int key = 0, scale = 0;
            float speedMs = 40.0f;  // retune time, 0 = hard / robotic
            float amount = 1.0f;    // how much of the correction to apply
            bool harmonyOn = false;
            HarmonyVoice voice[2];
            float formantSemis = 0.0f; // harmony voices only, +/-3
            float humanize = 0.5f;
        };

        std::atomic<float> detectedMidi { -1.0f }, correctionCents { 0.0f };

        void prepare (double sampleRate)
        {
            fs = sampleRate;
            detector.prepare (fs);
            maxP = detector.maxPeriod();
            minP = detector.minPeriod();
            defP = (float) (0.005 * fs);
            // A grain centred up to 1.5 periods ahead of the output needs input
            // reaching another (formant-stretched) period beyond that.
            latency = (int) std::ceil (maxP * (1.5f + maxFormantRatio)) + 32;
            hop = detector.windowSize() / 8;
            scratch.assign ((size_t) detector.windowSize(), 0.0f);
            in.assign (ringSize, 0.0f);
            for (auto& v : voices)
                v.acc.assign (ringSize, 0.0f);
            for (int i = 0; i <= hannSize; ++i)
                hann[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) hannSize);

            const juce::dsp::ProcessSpec mono { fs, 512, 1 };
            for (auto& d : humanDelay)
            {
                d.setMaximumDelayInSamples ((int) (0.05 * fs));
                d.prepare (mono);
            }
            reset();
        }

        int getLatency() const { return latency; }

        void reset()
        {
            std::fill (in.begin(), in.end(), 0.0f);
            for (auto& v : voices)
            {
                std::fill (v.acc.begin(), v.acc.end(), 0.0f);
                v.nextTs = -1.0;
                v.corr = 0.0f;
                v.target = -1;
                v.detune = 0.0f;
                v.active = false;
            }
            for (auto& d : humanDelay) d.reset();
            w = 0;
            hopCount = 0;
            markCount = markHead = 0;
            cur = {};
            lfo[0] = 0.0f;
            lfo[1] = 1.7f;
            detectedMidi.store (-1.0f);
            correctionCents.store (0.0f);
        }

        void setSettings (const Settings& s)
        {
            settings = s;
            formantRatio = std::pow (2.0f, juce::jlimit (-3.0f, 3.0f, s.formantSemis) / 12.0f);
        }

        // monoIn -> lead (delayed by getLatency(), tuned if Tune is on) and a
        // stereo harmony bus.
        void process (const float* monoIn, float* lead, float* harmL, float* harmR, int numSamples)
        {
            float gain[2], panL[2], panR[2];
            bool on[2];
            for (int h = 0; h < 2; ++h)
            {
                const auto& hv = settings.voice[h];
                on[h] = settings.harmonyOn && hv.interval > 0;
                gain[h] = dbToGain (hv.levelDb);
                const float p = (juce::jlimit (-1.0f, 1.0f, hv.pan) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
                panL[h] = std::cos (p);
                panR[h] = std::sin (p);
            }

            setActive (0, settings.tuneOn);
            setActive (1, on[0]);
            setActive (2, on[1]);

            const float lfoInc[2] { juce::MathConstants<float>::twoPi * 0.23f / (float) fs,
                                    juce::MathConstants<float>::twoPi * 0.31f / (float) fs };

            for (int i = 0; i < numSamples; ++i)
            {
                in[(size_t) (w & mask)] = monoIn[i];
                ++w;

                if (++hopCount >= hop)
                {
                    hopCount = 0;
                    detect();
                }
                makeMarks();

                const juce::int64 tOut = w - 1 - latency; // w-1 is the sample just written
                if (tOut < 0)
                {
                    lead[i] = harmL[i] = harmR[i] = 0.0f;
                    continue;
                }

                if (settings.tuneOn)
                {
                    synth (0, tOut);
                    lead[i] = take (0, tOut);
                }
                else
                {
                    lead[i] = in[(size_t) (tOut & mask)];
                }

                float l = 0.0f, r = 0.0f;
                for (int h = 0; h < 2; ++h)
                {
                    if (! on[h]) continue;
                    synth (h + 1, tOut);
                    float s = take (h + 1, tOut) * gain[h];

                    // Humanize: each harmony singer is a little late and drifts
                    lfo[h] += lfoInc[h];
                    if (lfo[h] > juce::MathConstants<float>::twoPi) lfo[h] -= juce::MathConstants<float>::twoPi;
                    const float delayMs = 2.0f + settings.humanize * (16.0f + 3.0f * std::sin (lfo[h]));
                    humanDelay[(size_t) h].pushSample (0, s);
                    s = humanDelay[(size_t) h].popSample (0, delayMs * 0.001f * (float) fs);

                    l += s * panL[h];
                    r += s * panR[h];
                }
                harmL[i] = l;
                harmR[i] = r;
            }
        }

    private:
        struct Mark
        {
            juce::int64 pos = 0;
            float period = 0.0f;
            float f0 = 0.0f;
            bool voiced = false;
        };

        struct Voice
        {
            std::vector<float> acc;
            double nextTs = -1.0;
            float corr = 0.0f;   // current shift in semitones
            int target = -1;     // tracked note the singer is aiming for
            float detune = 0.0f; // humanize drift in cents
            bool active = false;
        };

        static constexpr int ringSize = 1 << 16;
        static constexpr juce::int64 mask = ringSize - 1;
        static constexpr int maxMarks = 256;
        static constexpr int hannSize = 1024;
        static constexpr float maxFormantRatio = 1.1892f; // +3 semitones

        void setActive (int idx, bool active)
        {
            auto& v = voices[(size_t) idx];
            if (active == v.active) return;
            v.active = active;
            std::fill (v.acc.begin(), v.acc.end(), 0.0f);
            v.nextTs = -1.0;
            v.corr = 0.0f;
            v.target = -1;
            if (idx > 0) humanDelay[(size_t) idx - 1].reset();
        }

        float take (int idx, juce::int64 t)
        {
            auto& cell = voices[(size_t) idx].acc[(size_t) (t & mask)];
            const float v = cell;
            cell = 0.0f;
            return v;
        }

        float hannAt (float p) const
        {
            const float x = juce::jlimit (0.0f, 1.0f, p) * (float) hannSize;
            const int i = std::min ((int) x, hannSize - 1);
            const float f = x - (float) i;
            return hann[(size_t) i] + f * (hann[(size_t) i + 1] - hann[(size_t) i]);
        }

        float readIn (double pos) const
        {
            auto i = (juce::int64) std::floor (pos);
            float f = (float) (pos - (double) i);
            const juce::int64 lo = w - ringSize + 2, hi = w - 2;
            if (i < lo) { i = lo; f = 0.0f; }
            if (i > hi) { i = hi; f = 0.0f; }
            const float a = in[(size_t) (i & mask)], b = in[(size_t) ((i + 1) & mask)];
            return a + f * (b - a);
        }

        void detect()
        {
            const int n = detector.windowSize();
            for (int j = 0; j < n; ++j)
                scratch[(size_t) j] = in[(size_t) ((w - n + j) & mask)];

            cur = detector.analyse (scratch.data());
            detectedMidi.store (cur.voiced ? 69.0f + 12.0f * std::log2 (cur.f0 / 440.0f) : -1.0f);
        }

        void pushMark (const Mark& m)
        {
            marks[(size_t) markHead] = m;
            markHead = (markHead + 1) % maxMarks;
            markCount = std::min (markCount + 1, maxMarks);
        }

        // Lay analysis marks one pitch period apart, each snapped to the
        // nearest waveform peak so grains line up with the vocal-cord pulses.
        void makeMarks()
        {
            if (markCount == 0)
            {
                if (w < (juce::int64) (maxP * 2.0f)) return;
                pushMark ({ w - (juce::int64) maxP, defP, 0.0f, false });
            }

            for (;;)
            {
                const Mark& last = marks[(size_t) ((markHead + maxMarks - 1) % maxMarks)];
                const bool voiced = cur.voiced;
                const float period = voiced ? juce::jlimit (minP, maxP, (float) fs / cur.f0) : defP;
                const int search = voiced ? (int) (period / 3.0f) : 0;
                const juce::int64 cand = last.pos + (juce::int64) std::llround (period);
                if (cand + search >= w - 1) break;

                juce::int64 pos = cand;
                if (voiced)
                {
                    float best = -1.0e9f;
                    for (juce::int64 t = cand - search; t <= cand + search; ++t)
                    {
                        const float x = in[(size_t) (t & mask)];
                        if (x > best) { best = x; pos = t; }
                    }
                    if (pos <= last.pos + (juce::int64) (period * 0.5f))
                        pos = cand;
                }

                pushMark ({ pos, period, voiced ? cur.f0 : 0.0f, voiced });
            }
        }

        const Mark* nearestMark (double t) const
        {
            const Mark* best = nullptr;
            double bestDist = 1.0e18;
            for (int i = 0; i < markCount; ++i)
            {
                const Mark& m = marks[(size_t) i];
                const double dist = std::abs ((double) m.pos - t);
                if (dist < bestDist) { bestDist = dist; best = &m; }
            }
            return best;
        }

        void computeRatio (int idx, const Mark& m, float& ratio, float& gain)
        {
            auto& v = voices[(size_t) idx];

            if (! m.voiced)
            {
                // breaths and consonants pass through unshifted
                ratio = 1.0f;
                gain = idx == 0 ? 1.0f : 0.5f;
                return;
            }

            const float note = 69.0f + 12.0f * std::log2 (m.f0 / 440.0f);
            const auto sc = scales::get (settings.scale);
            const int nearest = scales::nearest (note, settings.key, sc);

            // hysteresis: only switch target once clearly closer to another note
            if (v.target < 0 || ! scales::inScale (v.target, settings.key, sc)
                || (nearest != v.target && std::abs (note - (float) v.target) - std::abs (note - (float) nearest) > 0.15f))
                v.target = nearest;

            const float tuneCorr = settings.tuneOn ? ((float) v.target - note) * settings.amount : 0.0f;
            float desired = tuneCorr;
            float speed = settings.speedMs;

            if (idx > 0)
            {
                const auto& hv = settings.voice[idx - 1];
                const int hn = scales::harmonyNote (v.target, hv.interval, settings.key, settings.scale);

                const float maxCents = settings.humanize * 12.0f;
                v.detune = juce::jlimit (-maxCents, maxCents, v.detune * 0.995f + (rng.nextFloat() - 0.5f) * 3.0f);

                desired = (float) (hn - v.target) + tuneCorr + v.detune * 0.01f;
                if (! settings.tuneOn) speed = 15.0f; // just smooth interval changes
            }

            const float alpha = speed < 0.5f ? 1.0f : 1.0f - std::exp (-m.period / (speed * 0.001f * (float) fs));
            v.corr += alpha * (desired - v.corr);

            ratio = std::pow (2.0f, v.corr / 12.0f);
            gain = 1.0f / std::sqrt (ratio); // keep loudness steady as grain density changes

            if (idx == 0)
                correctionCents.store (v.corr * 100.0f);
        }

        void synth (int idx, juce::int64 tOut)
        {
            auto& v = voices[(size_t) idx];
            const double horizon = (double) (tOut + (juce::int64) maxP);
            if (v.nextTs < 0.0) v.nextTs = horizon;

            while (v.nextTs <= horizon)
            {
                const Mark* m = nearestMark (v.nextTs);
                if (m == nullptr) { v.nextTs += defP; continue; }

                float ratio = 1.0f, gain = 1.0f;
                computeRatio (idx, *m, ratio, gain);

                const float fr = (idx == 0 || ! m->voiced) ? 1.0f : formantRatio;
                const int half = std::max (8, (int) m->period);
                const auto centre = (juce::int64) std::llround (v.nextTs);
                const float invLen = 1.0f / (float) (2 * half);

                for (int k = -half; k < half; ++k)
                {
                    const juce::int64 t = centre + k;
                    if (t < tOut) continue;
                    const float win = hannAt ((float) (k + half) * invLen);
                    v.acc[(size_t) (t & mask)] += win * gain * readIn ((double) m->pos + (double) k * fr);
                }

                v.nextTs += std::max ((double) minP * 0.5, (double) m->period / ratio);
            }
        }

        double fs = 44100.0;
        PitchDetector detector;
        PitchDetector::Result cur;
        Settings settings;
        float formantRatio = 1.0f;
        float maxP = 551.0f, minP = 36.0f, defP = 220.0f;
        int latency = 1300, hop = 256, hopCount = 0;

        std::vector<float> in, scratch;
        juce::int64 w = 0;
        std::array<Mark, maxMarks> marks;
        int markCount = 0, markHead = 0;
        std::array<Voice, 3> voices;
        std::array<float, hannSize + 1> hann {};
        std::array<juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear>, 2> humanDelay;
        float lfo[2] {};
        juce::Random rng { 0x5a5a };
    };
}
