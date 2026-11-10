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

        // Fade curves, as the gain at t from 0 (silent) to 1 (full level):
        // quarterSine sin (t pi / 2) (equal power: a fade out is cos (t pi / 2)),
        // sineSquared sin^2 (t pi / 2) (a Hann half, equal gain: out is cos^2),
        // linear t (out is 1 - t).
        enum class FadeCurve { quarterSine, sineSquared, linear };

        // Where in each sample the curve is read: sample k of a len-sample fade
        // (counted from the silent end) is at t = k / len (start: the silent
        // end's sample is 0), (k + 0.5) / len (centre) or (k + 1) / len (end:
        // no sample is silent).
        enum class FadePoint { start, centre, end };

        inline double fadeGain (FadeCurve c, double t)
        {
            switch (c)
            {
                case FadeCurve::sineSquared: { const double s = std::sin (t * 3.141592653589793 / 2); return s * s; }
                case FadeCurve::linear:      return t;
                case FadeCurve::quarterSine: break;
            }
            return std::sin (t * 3.141592653589793 / 2);
        }

        // The other side of a crossfade at the same t: the gain of what fades out.
        inline double fadeOutGain (FadeCurve c, double t)
        {
            switch (c)
            {
                case FadeCurve::sineSquared: { const double s = std::cos (t * 3.141592653589793 / 2); return s * s; }
                case FadeCurve::linear:      return 1.0 - t;
                case FadeCurve::quarterSine: break;
            }
            return std::cos (t * 3.141592653589793 / 2);
        }

        inline double fadePosition (int k, int len, FadePoint point)
        {
            const double off = point == FadePoint::centre ? 0.5 : point == FadePoint::end ? 1.0 : 0.0;
            return ((double) k + off) / len;
        }

        // Fades in place, lengths in samples: the first inLen samples rise and
        // the last outLen fall, along the curve. A fade longer than the audio
        // keeps its length (its shape is cut short, not squeezed).
        inline void fadeSamples (float* const* ch, int numCh, int n, int inLen, int outLen,
                                 FadeCurve curve = FadeCurve::quarterSine, FadePoint point = FadePoint::start)
        {
            const int fi = std::max (0, inLen), fo = std::max (0, outLen);
            for (int c = 0; c < numCh; ++c)
            {
                float* x = ch[c];
                for (int k = 0; k < std::min (fi, n); ++k) x[k] = (float) (x[k] * fadeGain (curve, fadePosition (k, fi, point)));
                for (int k = 0; k < std::min (fo, n); ++k) x[n - 1 - k] = (float) (x[n - 1 - k] * fadeGain (curve, fadePosition (k, fo, point)));
            }
        }

        // The same with the lengths in seconds (rounded to whole samples).
        inline void fades (float* const* ch, int numCh, int n, double fs, double fadeInSec, double fadeOutSec,
                           FadeCurve curve = FadeCurve::quarterSine)
        {
            const int fi = (int) std::min ((double) n, roundHalfUp (fadeInSec * fs));
            const int fo = (int) std::min ((double) n, roundHalfUp (fadeOutSec * fs));
            fadeSamples (ch, numCh, n, fi, fo, curve);
        }

        // Crossfades len samples: into[i] becomes into[i] x the fade-in gain plus
        // from[i] x the fade-out gain at the same t (with quarterSine, the equal
        // power join: sin and cos).
        inline void crossfade (float* const* into, const float* const* from, int numCh, int len,
                               FadeCurve curve = FadeCurve::quarterSine, FadePoint point = FadePoint::centre)
        {
            for (int c = 0; c < numCh; ++c)
                for (int i = 0; i < len; ++i)
                {
                    const double t = fadePosition (i, len, point);
                    into[c][i] = (float) (into[c][i] * fadeGain (curve, t) + from[c][i] * fadeOutGain (curve, t));
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
            const double peakRoom = -1 - db20 ((double) peak, 0.0);
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
            const double g = gainFromDb (gainDb);
            if (std::abs (gainDb) > 1e-6)
                for (int c = 0; c < numCh; ++c)
                    for (int i = 0; i < n; ++i) ch[c][i] = (float) (ch[c][i] * g);
            return r;
        }
    }
}
