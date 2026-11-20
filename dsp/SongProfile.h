#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include "Loudness.h"
#include "Mastering.h"
#include "Spectrum.h"
#include "Util.h"

namespace vs
{
    // What a song "is", for comparing it with another (Master's reference
    // matching), measured on a stretch of it: its tone per octave (each band's
    // level relative to the stretch's loudness), its stereo width in three
    // ranges (the side's level minus the mid's), how squashed it is (the
    // sample peak over the loudness) and its loudness, plus the whole song's
    // loudness. Band levels are vs::bandLevelsDb: the tone on the mid at
    // tonal balance's octaves (Q 1.41), the width at 100 Hz, 1 kHz and 8 kHz
    // (Q 0.7).
    //
    // A song as a reference is measured on its loudest 30 s
    // (vs::LoudestStretch): pass 1 feeds the whole song to feedSong (the
    // stretch and the whole song's loudness), then the stretch's samples go to
    // feedStretch. A stretch the caller chose (a render of a master's loudest
    // part): setStretch with the whole song's loudness, then feedStretch.
    //
    //     SongProfile p;
    //     p.begin (fs, length);
    //     feed the song to feedSong();
    //     auto w = p.stretch();
    //     feed samples [w.from, w.from + w.length) to feedStretch();
    //     auto result = p.finish();
    class SongProfile
    {
    public:
        static constexpr int numTone = TonalBalance::numBands, numWidth = 3;
        static constexpr double stretchSeconds = 30.0;
        static constexpr double widthHz[numWidth] { 100, 1000, 8000 };
        static constexpr double toneQ = 1.41, widthQ = 0.7;

        struct Result
        {
            float lufs = -100.0f;                 // the stretch's integrated loudness
            float wholeLufs = -100.0f;            // the whole song's
            std::array<float, numTone> tone {};   // dB relative to lufs
            std::array<float, numWidth> width {}; // side minus mid, dB
            float plr = 0.0f;                     // sample peak over lufs, dB
        };

        void begin (double sampleRate, long long songLength)
        {
            fs = sampleRate;
            loud.begin (fs, songLength);
            wholeMeter.prepare (fs);
            stretchMeter.prepare (fs);
            mid.clear();
            side.clear();
            peak = 0.0f;
            haveWhole = false;
            wholeGiven = -100.0f;
            chosen = {};
        }

        // ---- pass 1 (a reference): the whole song, in order (r null: mono)
        void feedSong (const float* l, const float* r, int n)
        {
            loud.feed (l, r, n);
            wholeMeter.process (l, r != nullptr ? r : l, n);
            haveWhole = true;
        }

        // Where the stretch is: the loudest 30 s after feedSong, or what setStretch chose.
        LoudestStretch::Window stretch() { return chosen.length > 0 ? chosen : loud.window (stretchSeconds); }

        // ---- or: a stretch the caller chose, and the whole song's loudness
        void setStretch (long long from, long long length, float wholeSongLufs)
        {
            chosen.from = from;
            chosen.length = length;
            wholeGiven = wholeSongLufs;
        }

        // ---- pass 2: the stretch's samples, in order (r null: mono)
        void feedStretch (const float* l, const float* r, int n)
        {
            const float* R = r != nullptr ? r : l;
            stretchMeter.process (l, R, n);
            for (int i = 0; i < n; ++i)
            {
                mid.push_back (midOf (l[i], R[i]));
                side.push_back (0.5f * (l[i] - R[i]));
                peak = std::max (peak, std::max (std::abs (l[i]), std::abs (R[i])));
            }
        }

        Result finish()
        {
            Result out;
            out.lufs = stretchMeter.integrated();
            out.wholeLufs = haveWhole ? wholeMeter.integrated() : (wholeGiven > -100.0f ? wholeGiven : out.lufs);
            const auto n = (std::int64_t) mid.size();
            const auto tone = bandLevelsDb (mid.data(), n, fs, TonalBalance::bandHz, numTone, toneQ);
            const auto wm = bandLevelsDb (mid.data(), n, fs, widthHz, numWidth, widthQ);
            const auto ws = bandLevelsDb (side.data(), n, fs, widthHz, numWidth, widthQ);
            for (int b = 0; b < numTone; ++b) out.tone[(size_t) b] = (float) tone[(size_t) b] - out.lufs;
            for (int b = 0; b < numWidth; ++b) out.width[(size_t) b] = (float) (ws[(size_t) b] - wm[(size_t) b]);
            out.plr = ampToDb (peak, 1.0e-12f) - out.lufs;
            return out;
        }

    private:
        double fs = 48000.0;
        LoudestStretch loud;
        LoudestStretch::Window chosen;
        LoudnessMeter wholeMeter, stretchMeter;
        std::vector<float> mid, side;
        float peak = 0.0f, wholeGiven = -100.0f;
        bool haveWhole = false;
    };
}
