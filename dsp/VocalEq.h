#pragma once

#include <algorithm>
#include <array>
#include "EqBand.h"

namespace vs
{
    // The vocal EQ: a low cut (24 dB/octave Butterworth, 20 to 400 Hz, off at
    // 21 Hz and below) and a four-band tone EQ (low shelf 110 Hz Q 0.7, body
    // bell 350 Hz Q 0.9, presence bell 3.5 kHz Q 0.8, air high shelf at
    // 11 kHz or 0.42 fs if lower, Q 0.7, each +-12 dB). Built on EqBand, so
    // frequency and gain glide to a new setting every 32 samples instead of
    // jumping (no zipper noise while a knob moves).
    class VocalEq
    {
    public:
        struct Settings
        {
            float lowCutHz = 20.0f;
            float lowDb = 0.0f, bodyDb = 0.0f, presenceDb = 0.0f, airDb = 0.0f;
            bool lowCutOn = true, toneOn = true;
        };

        static constexpr int glideBlock = 32;

        void prepare (double sampleRate)
        {
            fs = sampleRate;
            for (auto& b : bands) b.prepare (fs);
            applyTargets();
        }

        void reset() { for (auto& b : bands) b.reset(); }

        void setSettings (const Settings& s) { settings = s; }
        const Settings& getSettings() const { return settings; }

        // The EqBand settings each band glides to (0 low cut, 1 low, 2 body, 3 presence, 4 air)
        EqBand::Settings bandTarget (int band) const
        {
            EqBand::Settings b;
            auto clampDb = [] (float v) { return std::clamp (v, -12.0f, 12.0f); };
            switch (band)
            {
                case 0:
                    b.type = EqBand::lowCut; b.slope = 1;
                    b.freq = std::clamp (settings.lowCutHz, 20.0f, 400.0f);
                    b.on = settings.lowCutOn && settings.lowCutHz > 21.0f;
                    return b;
                case 1: b.type = EqBand::lowShelf;  b.freq = 110.0f;  b.q = 0.7f; b.gainDb = clampDb (settings.lowDb); break;
                case 2: b.type = EqBand::bell;      b.freq = 350.0f;  b.q = 0.9f; b.gainDb = clampDb (settings.bodyDb); break;
                case 3: b.type = EqBand::bell;      b.freq = 3500.0f; b.q = 0.8f; b.gainDb = clampDb (settings.presenceDb); break;
                default:
                    b.type = EqBand::highShelf; b.freq = std::min (11000.0f, (float) fs * 0.42f); b.q = 0.7f;
                    b.gainDb = clampDb (settings.airDb);
                    break;
            }
            b.on = settings.toneOn;
            return b;
        }

        // Processes numCh channels (up to 2) in place: the low cut, then the tone EQ.
        void process (float* const* ch, int numCh, int n) { processBands (ch, numCh, n, 0, numBands); }

        // The two halves on their own, for a chain that puts other processing
        // between them (the low cut early, the tone EQ after drive).
        void processLowCut (float* const* ch, int numCh, int n) { processBands (ch, numCh, n, 0, 1); }
        void processTone (float* const* ch, int numCh, int n)   { processBands (ch, numCh, n, 1, numBands); }

        // The whole EQ's gain at freq, dB, at the current settings
        double magnitudeDb (double freq) const
        {
            double db = 0.0;
            for (int b = 0; b < numBands; ++b) db += EqBand::magnitudeDb (bandTarget (b), freq, fs);
            return db;
        }

        static constexpr int numBands = 5;

    private:
        void applyTargets() { for (int b = 0; b < numBands; ++b) bands[(size_t) b].setTarget (bandTarget (b)); }

        void processBands (float* const* ch, int numCh, int n, int from, int to)
        {
            for (int start = 0; start < n; start += glideBlock)
            {
                const int len = std::min (glideBlock, n - start);
                for (int b = from; b < to; ++b)
                {
                    bands[(size_t) b].setTarget (bandTarget (b));
                    bands[(size_t) b].process (ch, std::min (numCh, 2), start, len);
                }
            }
        }

        double fs = 48000.0;
        Settings settings;
        std::array<EqBand, numBands> bands;
    };
}
