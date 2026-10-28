#pragma once

#include "JuceInclude.h"
#include <array>
#include <vector>
#include "Util.h"

namespace vs
{
    // Algorithmic reverb.
    //
    // Room / Chamber / Plate / Hall / Huge: an 8-line feedback delay network
    // (FDN). Input is smeared by allpass diffusers, then circulates through
    // eight delay lines that are mixed into each other every pass by a
    // Hadamard matrix. Each line loses exactly enough level per pass to hit the
    // requested decay time, and a lowpass in the loop makes highs die first,
    // like real air and walls. Slow pitch modulation of the lines keeps the
    // tail from ringing metallic. Rooms/halls add discrete early reflections.
    //
    // Spring: two simulated springs. A long chain of allpass filters delays
    // low frequencies more than highs (dispersion), which is the "boing" and
    // "drip" of a real spring tank, inside a feedback loop.
    class ReverbEngine
    {
    public:
        enum Mode { room = 0, chamber, plate, hall, huge, spring, numModes };

        struct Settings
        {
            int mode = plate;
            float predelayMs = 20.0f;
            float decaySec = 2.0f;
            float size = 0.5f;      // 0..1
            float toneHz = 8000.0f; // in-loop damping: lower = darker tail
            float lowCutHz = 150.0f;
            float width = 1.0f;     // 0 mono .. 1.5 extra wide
            float mod = 0.3f;       // 0..1
            float duck = 0.0f;      // 0..1, dip the tail while the dry signal plays
            float shimmer = 0.0f;   // 0..1, octave-up feedback
        };

        void prepare (double sampleRate, int maxBlock)
        {
            fs = sampleRate;
            const int maxDelay = (int) (0.45 * fs) + 16; // longest line (Huge, size 1) + mod
            for (auto& l : lines) l.init (maxDelay);
            for (auto& d : diffL) d.init ((int) (0.05 * fs));
            for (auto& d : diffR) d.init ((int) (0.05 * fs));
            pre.init ((int) (0.26 * fs) + 4);
            preL.init ((int) (0.26 * fs) + 4);
            preR.init ((int) (0.26 * fs) + 4);
            er.init ((int) (0.12 * fs) + 4);
            for (auto& s : springs) s.delay.init ((int) (0.12 * fs));
            shimBuf.assign (shimSize, 0.0f);
            juce::ignoreUnused (maxBlock);
            reset();
            applySettings (settings, true);
        }

        void reset()
        {
            for (auto& l : lines) l.clear();
            for (auto& d : diffL) d.clear();
            for (auto& d : diffR) d.clear();
            pre.clear(); preL.clear(); preR.clear();
            er.clear();
            for (auto& s : springs) { s.delay.clear(); for (auto& a : s.ap) a = 0.0f; s.lp = s.hp = 0.0f; }
            std::fill (shimBuf.begin(), shimBuf.end(), 0.0f);
            shimWrite = 0; shimPhase = 0.0f;
            for (auto& v : damp) v = 0.0f;
            for (auto& p : lfoPhase) p = 0.0f;
            duckEnv = 0.0f; duckGain = 1.0f;
            outHpL = outHpR = 0.0f; outHpPrevL = outHpPrevR = 0.0f;
            inLp[0] = inLp[1] = 0.0f;
            fade = 0.0f;
        }

        void setSettings (const Settings& s)
        {
            if (s.mode != settings.mode)
            {
                // Mode switch: fade the old tail out, then rebuild. Avoids a
                // click from suddenly changing delay lengths.
                pendingMode = true;
            }
            applySettings (s, false);
        }

        // Wet-only output; the plugin handles dry/wet.
        void process (const float* inL, const float* inR, float* outL, float* outR, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                float xl = inL[i], xr = inR[i];

                // Ducking follows the dry input level
                const float lvl = std::max (std::abs (xl), std::abs (xr));
                duckEnv = lvl > duckEnv ? duckEnv + duckAtk * (lvl - duckEnv) : duckEnv + duckRel * (lvl - duckEnv);
                const float duckAmt = settings.duck * juce::jlimit (0.0f, 1.0f, (ampToDb (duckEnv) + 45.0f) / 30.0f);
                const float duckTarget = 1.0f - 0.85f * duckAmt;
                duckGain += 0.002f * (duckTarget - duckGain);

                // Mode-change crossfade: fade out, swap, fade in
                if (pendingMode)
                {
                    fade = std::max (0.0f, fade - fadeStep);
                    if (fade <= 0.0f) { pendingMode = false; reset(); applySettings (settings, true); }
                }
                else
                {
                    fade = std::min (1.0f, fade + fadeStep);
                }

                // Gentle input band-limit so the reverb doesn't fizz
                inLp[0] += inLpC * (xl - inLp[0]);
                inLp[1] += inLpC * (xr - inLp[1]);
                xl = inLp[0]; xr = inLp[1];

                pre.write (0.5f * (xl + xr));
                preL.write (xl); // stereo pre-delay for the FDN's two inputs
                preR.write (xr);
                const float pm = pre.read (preSamples);
                const float pl = preL.read (preSamples), pr = preR.read (preSamples);

                float wl = 0.0f, wr = 0.0f;
                if (activeMode == spring)
                    processSpring (pm, wl, wr);
                else
                    processFdn (pl, pr, pm, wl, wr);

                // Low cut on the wet signal (one-pole x2)
                const float hl = wl - outHpL;  outHpL += outHpC * hl;
                const float hr = wr - outHpR;  outHpR += outHpC * hr;
                const float hl2 = hl - outHpPrevL; outHpPrevL += outHpC * hl2;
                const float hr2 = hr - outHpPrevR; outHpPrevR += outHpC * hr2;
                wl = hl2; wr = hr2;

                // Width (mid/side)
                const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * settings.width;
                const float g = duckGain * fade * outGain;
                outL[i] = (mid + side) * g;
                outR[i] = (mid - side) * g;
            }
        }

    private:
        //======================================================================
        struct Delay
        {
            std::vector<float> buf;
            int mask = 0, w = 0;

            void init (int maxSamples)
            {
                int size = 1;
                while (size < maxSamples + 4) size <<= 1;
                buf.assign ((size_t) size, 0.0f);
                mask = size - 1;
                w = 0;
            }
            void clear() { std::fill (buf.begin(), buf.end(), 0.0f); w = 0; }
            void write (float x) { buf[(size_t) w] = x; w = (w + 1) & mask; }
            // delay >= 1 sample (1 = newest), fractional, linear interpolation
            float read (float delay) const
            {
                const float pos = (float) w - delay;
                const int i = (int) std::floor (pos);
                const float f = pos - (float) i;
                const float a = buf[(size_t) (i & mask)], b = buf[(size_t) ((i + 1) & mask)];
                return a + f * (b - a);
            }
        };

        //======================================================================
        void applySettings (const Settings& s, bool rebuildMode)
        {
            const bool modeChanged = rebuildMode || s.mode != activeMode;
            settings = s;
            if (pendingMode && ! rebuildMode) return; // finish the fade first

            if (modeChanged) activeMode = s.mode;
            const int m = juce::jlimit (0, (int) numModes - 1, activeMode);
            const float scale = 0.55f + 0.9f * juce::jlimit (0.0f, 1.0f, s.size);

            static const float baseMs[5][numLines] {
                { 11.3f, 13.7f, 16.1f, 18.9f, 21.7f, 24.3f, 27.1f, 29.9f },     // room
                { 17.3f, 21.1f, 25.3f, 29.3f, 33.7f, 37.9f, 41.3f, 45.7f },     // chamber
                { 9.7f, 12.9f, 15.3f, 17.9f, 21.1f, 23.9f, 27.3f, 31.1f },      // plate
                { 31.1f, 37.3f, 43.1f, 51.7f, 59.3f, 67.1f, 73.9f, 83.3f },     // hall
                { 67.1f, 79.3f, 97.1f, 113.3f, 131.9f, 149.3f, 167.9f, 191.3f } // huge
            };
            static const float diffMs[4] { 4.7f, 3.6f, 12.7f, 9.3f };
            static const float diffGain[5] { 0.6f, 0.68f, 0.78f, 0.7f, 0.75f };
            static const float modDepthMs[5] { 0.25f, 0.35f, 0.3f, 0.6f, 1.6f };
            static const float erLevel[5] { 0.7f, 0.5f, 0.0f, 0.35f, 0.15f };

            if (m != spring)
            {
                for (int i = 0; i < numLines; ++i)
                {
                    lineSamples[i] = baseMs[m][i] * 0.001f * scale * (float) fs;
                    // per-pass gain for the requested RT60 (-60 dB after decaySec)
                    lineGain[i] = std::pow (10.0f, -3.0f * lineSamples[i] / ((float) fs * std::max (0.1f, s.decaySec)));
                }
                for (int i = 0; i < 4; ++i)
                    diffSamples[i] = diffMs[i] * 0.001f * (0.6f + 0.6f * s.size) * (float) fs;
                apGain = diffGain[m];
                modDepth = s.mod * modDepthMs[m] * 0.001f * (float) fs;
                erMix = erLevel[m];
                buildEarlyReflections (m, scale);
            }
            else
            {
                for (int k = 0; k < 2; ++k)
                {
                    springs[(size_t) k].delaySamples = (k == 0 ? 37.0f : 43.3f) * 0.001f * (0.6f + 0.8f * s.size) * (float) fs;
                    springs[(size_t) k].fb = std::pow (10.0f, -3.0f * springs[(size_t) k].delaySamples / ((float) fs * std::max (0.1f, s.decaySec)));
                }
                springLpC = onePoleHz (std::min (s.toneHz, 5500.0f));
                springHpC = onePoleHz (90.0f);
            }

            dampC = onePoleHz (s.toneHz);
            inLpC = onePoleHz (16000.0f);
            outHpC = onePoleHz (s.lowCutHz);
            preSamples = std::max (1.0f, s.predelayMs * 0.001f * (float) fs);
            duckAtk = onePole (5.0f, fs);
            duckRel = onePole (300.0f, fs);
            fadeStep = 1.0f / (0.03f * (float) fs);
            shimGain = s.shimmer * 0.32f;
            // Loudness roughly even across modes and decay times
            outGain = (m == spring ? 0.5f : 0.6f) / std::sqrt (std::max (0.3f, s.decaySec * 0.6f)) * (m == huge ? 1.3f : 1.0f);
            outGain = std::min (outGain, 0.9f);
        }

        float onePoleHz (float hz) const
        {
            return 1.0f - std::exp (-juce::MathConstants<float>::twoPi * juce::jlimit (10.0f, 0.45f * (float) fs, hz) / (float) fs);
        }

        void buildEarlyReflections (int m, float scale)
        {
            // Hand-picked tap times (ms) for a box-shaped room, gains falling
            // off with distance, alternating sides.
            static const float tapMs[numTaps] { 3.1f, 5.3f, 7.9f, 10.7f, 13.9f, 17.3f, 21.1f, 25.7f, 30.3f, 35.9f, 41.3f, 47.9f };
            const float roomScale = (m == hall ? 1.9f : m == chamber ? 1.3f : m == huge ? 2.2f : 0.8f) * scale;
            for (int t = 0; t < numTaps; ++t)
            {
                taps[t].samples = std::min (tapMs[t] * roomScale * 0.001f * (float) fs, 0.11f * (float) fs);
                const float g = 0.9f * std::pow (0.82f, (float) t);
                taps[t].gl = (t % 2 == 0) ? g : g * 0.45f;
                taps[t].gr = (t % 2 == 0) ? g * 0.45f : g;
            }
        }

        static float allpass (Delay& d, float x, float delay, float g)
        {
            const float v = d.read (delay);
            const float in = x + g * v;
            d.write (in);
            return v - g * in;
        }

        void processFdn (float xl, float xr, float xm, float& outL, float& outR)
        {
            // Early reflections from the mono pre-delayed signal
            er.write (xm);
            float erl = 0.0f, err = 0.0f;
            if (erMix > 0.0f)
                for (int t = 0; t < numTaps; ++t)
                {
                    const float s = er.read (taps[t].samples);
                    erl += s * taps[t].gl;
                    err += s * taps[t].gr;
                }

            // Input diffusion (separate chains keep the stereo image)
            for (int k = 0; k < 4; ++k)
            {
                xl = allpass (diffL[(size_t) k], xl, diffSamples[k], apGain);
                xr = allpass (diffR[(size_t) k], xr, diffSamples[k] * 1.07f, apGain);
            }

            // Read lines (modulated)
            float y[numLines];
            for (int i = 0; i < numLines; ++i)
            {
                lfoPhase[i] += lfoInc[i] / (float) fs;
                if (lfoPhase[i] >= 1.0f) lfoPhase[i] -= 1.0f;
                const float mod = modDepth * std::sin (juce::MathConstants<float>::twoPi * lfoPhase[i]);
                y[i] = lines[(size_t) i].read (std::max (2.0f, lineSamples[i] + mod));
            }

            // Damping (highs decay faster)
            float h[numLines];
            for (int i = 0; i < numLines; ++i)
            {
                damp[i] += dampC * (y[i] - damp[i]);
                h[i] = damp[i];
            }

            // Hadamard 8x8 mix (fast Walsh-Hadamard transform), energy preserving
            for (int len = 1; len < numLines; len <<= 1)
                for (int i = 0; i < numLines; i += len << 1)
                    for (int j = i; j < i + len; ++j)
                    {
                        const float a = h[j], b = h[j + len];
                        h[j] = a + b;
                        h[j + len] = a - b;
                    }
            constexpr float norm = 0.35355339f; // 1/sqrt(8)

            // Shimmer: octave-up of the tail fed back in
            float shim = 0.0f;
            if (shimGain > 0.0f)
                shim = shimmerShift (0.125f * (y[0] + y[2] + y[4] + y[6] - y[1] - y[3] - y[5] - y[7])) * shimGain;

            for (int i = 0; i < numLines; ++i)
            {
                const float in = (i % 2 == 0 ? xl : xr) * 0.5f + shim;
                lines[(size_t) i].write (in + lineGain[i] * h[i] * norm);
            }

            // Two decorrelated output mixes
            constexpr float signL[numLines] { 1, 1, -1, -1, 1, 1, -1, -1 };
            constexpr float signR[numLines] { 1, -1, 1, -1, 1, -1, 1, -1 };
            float l = 0.0f, r = 0.0f;
            for (int i = 0; i < numLines; ++i) { l += y[i] * signL[i]; r += y[i] * signR[i]; }

            outL = l * norm + erl * erMix * 0.5f;
            outR = r * norm + err * erMix * 0.5f;
        }

        // Two crossfading read heads sweeping through a short buffer: a simple
        // +12 semitone shifter, plenty clean for a washy shimmer tail.
        float shimmerShift (float x)
        {
            shimBuf[(size_t) shimWrite] = x;
            const float win = 0.06f * (float) fs;
            shimPhase += 1.0f / win; // (ratio - 1) / window, ratio = 2
            if (shimPhase >= 1.0f) shimPhase -= 1.0f;

            float out = 0.0f;
            for (int k = 0; k < 2; ++k)
            {
                float p = shimPhase + 0.5f * (float) k;
                if (p >= 1.0f) p -= 1.0f;
                const float delay = (1.0f - p) * win + 2.0f;
                const float pos = (float) shimWrite - delay;
                const int i0 = (int) std::floor (pos);
                const float f = pos - (float) i0;
                const float a = shimBuf[(size_t) (i0 & (shimSize - 1))], b = shimBuf[(size_t) ((i0 + 1) & (shimSize - 1))];
                const float gain = std::sin (juce::MathConstants<float>::pi * p); // fades in/out at the jumps
                out += (a + f * (b - a)) * gain * gain;
            }
            shimWrite = (shimWrite + 1) & (shimSize - 1);
            return out;
        }

        void processSpring (float x, float& outL, float& outR)
        {
            float out[2];
            for (int k = 0; k < 2; ++k)
            {
                auto& s = springs[(size_t) k];
                const float back = s.delay.read (s.delaySamples);
                s.lp += springLpC * (back - s.lp);
                float v = x * 0.6f + s.fb * s.lp;
                // drop the lows (springs can't reproduce them)
                s.hp += springHpC * (v - s.hp);
                v -= s.hp;

                // Dispersion: cascade of first-order allpasses
                const float a = k == 0 ? 0.62f : 0.58f;
                for (auto& z : s.ap)
                {
                    const float yv = -a * v + z;
                    z = v + a * yv;
                    v = yv;
                }
                s.delay.write (v);
                out[k] = v;
            }
            // cross-blend a little so the two springs read as one tank
            outL = out[0] * 0.85f + out[1] * 0.15f;
            outR = out[1] * 0.85f + out[0] * 0.15f;
        }

        //======================================================================
        static constexpr int numLines = 8, numTaps = 12, shimSize = 1 << 14;

        struct Tap { float samples = 1.0f, gl = 0.0f, gr = 0.0f; };
        struct Spring
        {
            Delay delay;
            std::array<float, 48> ap {};
            float delaySamples = 1000.0f, fb = 0.5f, lp = 0.0f, hp = 0.0f;
        };
        double fs = 48000.0;
        Settings settings;
        int activeMode = plate;
        bool pendingMode = false;
        float fade = 0.0f, fadeStep = 0.001f;

        std::array<Delay, numLines> lines;
        std::array<Delay, 4> diffL, diffR;
        Delay pre, er;
        Delay preL, preR;
        float lineSamples[numLines] {}, lineGain[numLines] {}, damp[numLines] {}, lfoPhase[numLines] {};
        const float lfoInc[numLines] { 0.31f, 0.43f, 0.57f, 0.67f, 0.79f, 0.89f, 1.03f, 1.13f };
        float diffSamples[4] {}, apGain = 0.7f, modDepth = 0.0f, dampC = 0.5f, erMix = 0.0f;
        Tap taps[numTaps];

        std::array<Spring, 2> springs;
        float springLpC = 0.5f, springHpC = 0.01f;

        std::vector<float> shimBuf;
        int shimWrite = 0;
        float shimPhase = 0.0f, shimGain = 0.0f;

        float preSamples = 1.0f, inLp[2] {}, inLpC = 0.9f;
        float outHpC = 0.01f, outHpL = 0.0f, outHpR = 0.0f, outHpPrevL = 0.0f, outHpPrevR = 0.0f;
        float duckEnv = 0.0f, duckGain = 1.0f, duckAtk = 0.1f, duckRel = 0.001f;
        float outGain = 0.5f;
    };
}
