#pragma once

#include <array>
#include "Biquad.h"

namespace vs
{
    // One band of a parametric EQ. Cuts can be 12, 24 or 48 dB/octave
    // (1, 2 or 4 cascaded Butterworth sections).
    class EqBand
    {
    public:
        enum Type { bell = 0, lowShelf, highShelf, lowCut, highCut, notch };

        struct Settings
        {
            bool on = true;
            int type = bell;
            float freq = 1000.0f, gainDb = 0.0f, q = 0.7f;
            int slope = 1; // 0 = 12, 1 = 24, 2 = 48 dB/oct
            bool operator== (const Settings& o) const
            {
                return on == o.on && type == o.type && freq == o.freq && gainDb == o.gainDb && q == o.q && slope == o.slope;
            }
        };

        // Fills up to 4 sections; returns how many are used.
        static int design (const Settings& s, double fs, std::array<BiquadCoefs, 4>& out)
        {
            switch (s.type)
            {
                case lowShelf:  out[0] = BiquadCoefs::lowShelf (fs, s.freq, s.q, s.gainDb); return 1;
                case highShelf: out[0] = BiquadCoefs::highShelf (fs, s.freq, s.q, s.gainDb); return 1;
                case notch:     out[0] = BiquadCoefs::notch (fs, s.freq, std::max (0.3f, s.q * 2.0f)); return 1;
                case lowCut:
                case highCut:
                {
                    static const double q4[2] { 0.5412, 1.3066 };
                    static const double q8[4] { 0.5098, 0.6013, 0.9000, 2.5629 };
                    const int n = s.slope == 0 ? 1 : s.slope == 1 ? 2 : 4;
                    for (int i = 0; i < n; ++i)
                    {
                        // 12 dB/oct uses the band's Q (lets you add a resonant bump); steeper slopes are pure Butterworth
                        const double q = n == 1 ? (double) s.q : n == 2 ? q4[i] : q8[i];
                        out[(size_t) i] = s.type == lowCut ? BiquadCoefs::highPass (fs, s.freq, q) : BiquadCoefs::lowPass (fs, s.freq, q);
                    }
                    return n;
                }
                default: out[0] = BiquadCoefs::peak (fs, s.freq, s.q, s.gainDb); return 1;
            }
        }

        static double magnitudeDb (const Settings& s, double freq, double fs)
        {
            if (! s.on) return 0.0;
            std::array<BiquadCoefs, 4> c;
            const int n = design (s, fs, c);
            double db = 0.0;
            for (int i = 0; i < n; ++i) db += c[(size_t) i].magnitudeDb (freq, fs);
            return db;
        }

        void prepare (double sampleRate) { fs = sampleRate; reset(); first = true; }
        void reset() { for (auto& b : sections) b.reset(); }

        // Called once per small block. Frequency, gain and Q glide toward the
        // target so dragging a node doesn't zipper.
        void setTarget (const Settings& s)
        {
            target = s;
            if (first) { current = s; first = false; redesign(); return; }

            const bool structural = s.on != current.on || s.type != current.type || s.slope != current.slope;
            if (structural) { current = s; reset(); redesign(); return; } // fresh state: no click from stale history

            const float k = 0.25f;
            auto glideLog = [k] (float& v, float t) { v = std::exp (std::log (v) + k * (std::log (t) - std::log (v))); };
            const auto before = current;
            glideLog (current.freq, s.freq);
            glideLog (current.q, s.q);
            current.gainDb += k * (s.gainDb - current.gainDb);
            if (std::abs (current.freq - s.freq) < 0.01f) current.freq = s.freq;
            if (std::abs (current.gainDb - s.gainDb) < 0.005f) current.gainDb = s.gainDb;
            if (std::abs (current.q - s.q) < 0.0005f) current.q = s.q;
            if (! (before == current)) redesign();
        }

        void process (float* const* ch, int numCh, int start, int len)
        {
            if (! current.on) return;
            for (int c = 0; c < numCh; ++c)
                for (int i = start; i < start + len; ++i)
                {
                    float x = ch[c][i];
                    for (int k = 0; k < used; ++k) x = sections[(size_t) k].process (c, x);
                    ch[c][i] = x;
                }
        }

    private:
        void redesign()
        {
            std::array<BiquadCoefs, 4> c;
            const int n = design (current, fs, c);
            if (n != used) for (auto& b : sections) b.reset();
            used = n;
            for (int i = 0; i < n; ++i) sections[(size_t) i].c = c[(size_t) i];
        }

        double fs = 48000.0;
        Settings target, current;
        bool first = true;
        int used = 1;
        std::array<Biquad, 4> sections;
    };

    // A cut that only happens when its frequency spikes. Detection is
    // relative (band level vs. whole-signal level), so one setting works
    // whether the source is quiet or slammed.
    class DynamicBand
    {
    public:
        struct Settings
        {
            bool on = false;
            float freq = 3000.0f, q = 4.0f;
            float rangeDb = 6.0f;     // most it will cut
            float thresholdDb = -18.0f; // band level relative to full signal where cutting starts
        };

        void prepare (double sampleRate)
        {
            fs = sampleRate;
            // Both detectors average power the same way (10 ms) so their ratio
            // is the true relative level whatever the source's crest factor.
            avgC = 1.0f - std::exp (-1.0f / (0.01f * (float) fs));
            envB = envF = 1.0e-9f;
            cutDb = 0.0f;
            eq.reset(); det.reset();
            lastAppliedDb = 999.0f;
            setSettings (settings);
        }

        void setSettings (const Settings& s)
        {
            settings = s;
            det.c = BiquadCoefs::bandPass (fs, s.freq, std::max (1.0f, s.q));
        }

        float currentCutDb() const { return cutDb; }

        void process (float* const* ch, int numCh, int start, int len)
        {
            if (! settings.on) { cutDb = 0.0f; wasOn = false; return; }
            if (! wasOn) { eq.reset(); det.reset(); lastAppliedDb = 999.0f; wasOn = true; }

            for (int i = start; i < start + len; ++i)
            {
                float m = 0.0f;
                for (int c = 0; c < numCh; ++c) m += ch[c][i];
                m /= (float) numCh;
                const float b = det.process (0, m);
                envB += avgC * (b * b - envB);
                envF += avgC * (m * m - envF);
            }

            const float relDb = 10.0f * std::log10 ((envB + 1e-12f) / (envF + 1e-12f));
            const float fullDb = 10.0f * std::log10 (envF + 1e-12f);
            const float over = relDb - settings.thresholdDb;
            const float target = (over > 0.0f && fullDb > -70.0f) ? std::min (settings.rangeDb, over * 1.5f) : 0.0f;

            // clamp down fast (3 ms), let go slowly (120 ms)
            const float blockSec = (float) len / (float) fs;
            const float tau = target > cutDb ? 0.003f : 0.12f;
            cutDb += (1.0f - std::exp (-blockSec / tau)) * (target - cutDb);

            if (std::abs (cutDb - lastAppliedDb) > 0.05f)
            {
                lastAppliedDb = cutDb;
                eq.c = BiquadCoefs::peak (fs, settings.freq, settings.q, -cutDb);
            }

            for (int c = 0; c < numCh; ++c)
                for (int i = start; i < start + len; ++i)
                    ch[c][i] = eq.process (c, ch[c][i]);
        }

    private:
        double fs = 48000.0;
        Settings settings;
        Biquad det, eq;
        float avgC = 0.01f, envB = 0.0f, envF = 0.0f, cutDb = 0.0f, lastAppliedDb = 999.0f;
        bool wasOn = false;
    };

    // "Carve": ducks this track's frequencies wherever a side-chained track
    // (usually the vocal) is putting energy, band by band, only while it
    // plays. 20 bands from 80 Hz to 12 kHz.
    class Carve
    {
    public:
        static constexpr int numBands = 20;

        struct Settings
        {
            bool on = false;
            float depthDb = 6.0f;
            float lowHz = 120.0f, highHz = 8000.0f;
            float speed = 0.5f; // 0 slow and smooth .. 1 fast
        };

        static float bandFreq (int k) { return 80.0f * std::pow (12000.0f / 80.0f, (float) k / (float) (numBands - 1)); }

        void prepare (double sampleRate)
        {
            fs = sampleRate;
            for (int k = 0; k < numBands; ++k)
            {
                det[k].c = BiquadCoefs::bandPass (fs, bandFreq (k), 3.0);
                det[k].reset();
                cut[k].reset();
                cut[k].c = BiquadCoefs {};
                env[k] = 1.0e-9f;
                gainDb[k] = 0.0f;
                applied[k] = 0.0f;
            }
            setSettings (settings);
        }

        void setSettings (const Settings& s)
        {
            settings = s;
            const float atkMs = 80.0f - 70.0f * s.speed, relMs = 400.0f - 330.0f * s.speed;
            envC = 1.0f - std::exp (-1.0f / (0.01f * (float) fs));
            atk = atkMs; rel = relMs;
        }

        float bandGainDb (int k) const { return gainDb[k]; }
        bool isActive() const { return settings.on && hasSidechain; }

        // sc may be nullptr (no side-chain connected)
        void process (float* const* ch, int numCh, const float* const* sc, int scCh, int start, int len)
        {
            hasSidechain = sc != nullptr && scCh > 0;
            const bool active = settings.on && hasSidechain;

            float maxDb = -200.0f, totalPow = 0.0f;
            float bandDb[numBands];
            if (active)
            {
                for (int i = start; i < start + len; ++i)
                {
                    float m = 0.0f;
                    for (int c = 0; c < scCh; ++c) m += sc[c][i];
                    m /= (float) scCh;
                    totalPow += m * m;
                    for (int k = 0; k < numBands; ++k)
                    {
                        const float b = det[k].process (0, m);
                        env[k] += envC * (b * b - env[k]);
                    }
                }
                for (int k = 0; k < numBands; ++k)
                {
                    bandDb[k] = 10.0f * std::log10 (env[k] + 1e-12f);
                    maxDb = std::max (maxDb, bandDb[k]);
                }
            }

            const float scDb = 10.0f * std::log10 (totalPow / (float) std::max (1, len) + 1e-12f);
            const float activity = active ? juce::jlimit (0.0f, 1.0f, (scDb + 50.0f) / 20.0f) : 0.0f;
            const float blockSec = (float) len / (float) fs;

            for (int k = 0; k < numBands; ++k)
            {
                float target = 0.0f;
                if (activity > 0.0f)
                {
                    const float f = bandFreq (k);
                    // soft edges around the chosen range (1/3 octave fade)
                    const float edgeLo = juce::jlimit (0.0f, 1.0f, 1.0f + 3.0f * std::log2 (f / settings.lowHz));
                    const float edgeHi = juce::jlimit (0.0f, 1.0f, 1.0f - 3.0f * std::log2 (f / settings.highHz));
                    // neighbouring bells overlap, so weight falls off fast with
                    // distance from the strongest band to keep the total cut
                    // close to the Depth setting
                    const float w = juce::jlimit (0.0f, 1.0f, 1.0f + (bandDb[k] - maxDb) / 14.0f);
                    const float weight = w * w * w;
                    target = -settings.depthDb * weight * activity * edgeLo * edgeHi;
                }
                const float tau = (target < gainDb[k] ? atk : rel) * 0.001f;
                gainDb[k] += (1.0f - std::exp (-blockSec / tau)) * (target - gainDb[k]);

                if (std::abs (gainDb[k] - applied[k]) > 0.05f)
                {
                    applied[k] = gainDb[k];
                    cut[k].c = BiquadCoefs::peak (fs, bandFreq (k), 2.8, gainDb[k] * 0.8f);
                }
            }

            if (! active && allFlat()) { running = false; return; }
            if (! running) { for (auto& b : cut) b.reset(); running = true; }

            // All bands always run while carving (a 0 dB bell is exact
            // pass-through), so no filter ever restarts from stale state.
            for (int c = 0; c < numCh; ++c)
                for (int i = start; i < start + len; ++i)
                {
                    float x = ch[c][i];
                    for (int k = 0; k < numBands; ++k)
                        x = cut[k].process (c, x);
                    ch[c][i] = x;
                }
        }

    private:
        bool allFlat() const
        {
            for (int k = 0; k < numBands; ++k) if (applied[k] < -0.001f) return false;
            return true;
        }

        double fs = 48000.0;
        Settings settings;
        Biquad det[numBands], cut[numBands];
        float env[numBands] {}, gainDb[numBands] {}, applied[numBands] {};
        float envC = 0.01f, atk = 40.0f, rel = 200.0f;
        bool hasSidechain = false, running = false;
    };
}
