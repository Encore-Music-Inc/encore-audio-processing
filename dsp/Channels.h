#pragma once

#include <algorithm>
#include <cmath>
#include "Loudness.h"
#include "Util.h"

namespace vs
{
    // Whole-file channel work for converting and exporting: channel maps,
    // trimming, fades, the sample peak and normalizing. Audio is float, one
    // array per channel, changed in place where the length stays the same.
    namespace channels
    {
        // keep: as it is. left / right: that channel alone (right falls back to
        // left for mono). mono: the average of every channel, so a centred sound
        // keeps its level. stereo: mono doubled, stereo kept, and 5.1 and up
        // (L R C LFE Ls Rs ...) folded down the usual ITU way, L + 0.707 C +
        // 0.707 Ls (LFE left out).
        enum class Map { keep, left, right, mono, stereo };

        inline int mappedChannels (Map m, int numIn)
        {
            switch (m)
            {
                case Map::keep:   return numIn;
                case Map::left:
                case Map::right:
                case Map::mono:   return 1;
                case Map::stereo: return 2;
            }
            return numIn;
        }

        // Writes mappedChannels (m, numIn) channels of n samples to out (which
        // may not overlap in).
        inline void map (Map m, const float* const* in, int numIn, int n, float* const* out)
        {
            auto copy = [n] (const float* a, float* b) { std::copy (a, a + n, b); };
            switch (m)
            {
                case Map::keep:  for (int c = 0; c < numIn; ++c) copy (in[c], out[c]); return;
                case Map::left:  copy (in[0], out[0]); return;
                case Map::right: copy (numIn > 1 ? in[1] : in[0], out[0]); return;
                case Map::stereo:
                {
                    if (numIn == 1) { copy (in[0], out[0]); copy (in[0], out[1]); return; }
                    if (numIn == 2) { copy (in[0], out[0]); copy (in[1], out[1]); return; }
                    const double g = 0.7071067811865476;
                    for (int i = 0; i < n; ++i)
                    {
                        const double c = numIn > 2 ? in[2][i] * g : 0.0;
                        const double ls = numIn > 4 ? in[4][i] * g : 0.0;
                        const double rs = numIn > 5 ? in[5][i] * g : 0.0;
                        out[0][i] = (float) (in[0][i] + c + ls);
                        out[1][i] = (float) (in[1][i] + c + rs);
                    }
                    return;
                }
                case Map::mono:
                {
                    const double k = 1.0 / numIn;
                    std::fill (out[0], out[0] + n, 0.0f);
                    for (int c = 0; c < numIn; ++c)
                        for (int i = 0; i < n; ++i) out[0][i] = (float) (out[0][i] + in[c][i] * k);
                    return;
                }
            }
        }

        // The samples [first, last) that keep [startSec, endSec) of n samples
        // (endSec 0: to the end).
        struct Range { int first, last; };
        inline Range trimRange (int n, double fs, double startSec, double endSec = 0.0)
        {
            const int a = (int) std::max (0.0, std::min ((double) n, roundHalfUp (startSec * fs)));
            const int b = endSec > 0 ? (int) std::max ((double) a, std::min ((double) n, roundHalfUp (endSec * fs))) : n;
            return { a, b };
        }

        // Quarter-sine fades in place: sample i of a fade of len samples is scaled
        // by sin (pi / 2 * i / len), from the start and back from the end.
        inline void fades (float* const* ch, int numCh, int n, double fs, double fadeInSec, double fadeOutSec)
        {
            const int fi = (int) std::min ((double) n, roundHalfUp (fadeInSec * fs));
            const int fo = (int) std::min ((double) n, roundHalfUp (fadeOutSec * fs));
            const double halfPi = 3.141592653589793 / 2;
            for (int c = 0; c < numCh; ++c)
            {
                float* x = ch[c];
                for (int i = 0; i < fi; ++i) x[i] = (float) (x[i] * std::sin (halfPi * ((double) i / fi)));
                for (int i = 0; i < fo; ++i) x[n - 1 - i] = (float) (x[n - 1 - i] * std::sin (halfPi * ((double) i / fo)));
            }
        }

        // The sample peak (largest |x|) over every channel.
        inline float peakOf (const float* const* ch, int numCh, int n)
        {
            float p = 0.0f;
            for (int c = 0; c < numCh; ++c)
                for (int i = 0; i < n; ++i) p = std::max (p, std::abs (ch[c][i]));
            return p;
        }

        // Integrated loudness (BS.1770, vs::LoudnessMeter) of the first one or
        // two channels: one channel is measured as one channel.
        inline float loudnessOf (const float* const* ch, int numCh, int n, double fs)
        {
            LoudnessMeter m;
            m.prepare (fs);
            constexpr int block = 8192;
            for (int s = 0; s < n; s += block)
                m.process (ch[0] + s, numCh == 1 ? nullptr : ch[1] + s, std::min (block, n - s));
            return m.integrated();
        }

        // peak: a gain to -1 dBFS sample peak. lufs: to -14 LUFS, but never
        // past -1 dBFS sample peak. The gain is applied in place.
        enum class Normalize { off, peak, lufs };

        struct NormalizeResult
        {
            enum Outcome { unchanged, applied, silent, tooQuiet, peakLimited };
            double gainDb = 0.0;
            Outcome outcome = unchanged;
            float lufs = -100.0f;   // measured (lufs mode)
            double reachedLufs = 0; // where it stopped when peakLimited
        };

        inline NormalizeResult normalize (float* const* ch, int numCh, int n, double fs, Normalize mode)
        {
            NormalizeResult r;
            if (mode == Normalize::off) return r;
            const float peak = peakOf (ch, numCh, n);
            if (peak < 1e-9) { r.outcome = NormalizeResult::silent; return r; }
            const double peakRoom = -1 - 20 * std::log10 ((double) peak);
            double gainDb = peakRoom;
            r.outcome = NormalizeResult::applied;
            if (mode == Normalize::lufs)
            {
                r.lufs = loudnessOf (ch, numCh, n, fs);
                if (r.lufs <= -70) { r.outcome = NormalizeResult::tooQuiet; return r; }
                gainDb = -14 - (double) r.lufs;
                if (gainDb > peakRoom)
                {
                    r.outcome = NormalizeResult::peakLimited;
                    r.reachedLufs = r.lufs + peakRoom;
                    gainDb = peakRoom;
                }
            }
            r.gainDb = gainDb;
            const double g = std::pow (10.0, gainDb / 20);
            if (std::abs (gainDb) > 1e-6)
                for (int c = 0; c < numCh; ++c)
                    for (int i = 0; i < n; ++i) ch[c][i] = (float) (ch[c][i] * g);
            return r;
        }
    }
}
