#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
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
        // linear t (out is 1 - t), concave 1 - cos (t pi / 2) (slow start, fast
        // finish), sCurve (1 - t)(1 - cos (t pi / 2)) + t sin (t pi / 2) (slow at
        // both ends). Every fade out is the same curve read backwards, 1 - t.
        // quarterSine is the convex curve (fast start, slow finish) a DAW offers
        // beside linear, concave and S-curve.
        enum class FadeCurve { quarterSine, sineSquared, linear, concave, sCurve };

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
                case FadeCurve::concave:     return 1.0 - std::cos (t * 3.141592653589793 / 2);
                case FadeCurve::sCurve:      return (1.0 - t) * (1.0 - std::cos (t * 3.141592653589793 / 2)) + t * std::sin (t * 3.141592653589793 / 2);
                case FadeCurve::quarterSine: break;
            }
            return std::sin (t * 3.141592653589793 / 2);
        }

        // The same curves in single precision, number for number as Tracktion
        // Engine renders a clip's fades and crossfades (the angle in double, the
        // sine in float), so songs saved with them sound the same.
        inline float fadeGainFloat (FadeCurve c, double t)
        {
            constexpr double halfPi = 0.5 * 3.141592653589793;
            constexpr float halfPiF = 0.5f * 3.14159265358979323846f;
            switch (c)
            {
                case FadeCurve::linear:      return (float) t;
                case FadeCurve::concave:     return 1.0f - std::cos ((float) (t * halfPi));
                case FadeCurve::sCurve:      { const float a = (float) t; return (1.0f - a) * (1.0f - std::cos (a * halfPiF)) + a * std::sin (a * halfPiF); }
                case FadeCurve::sineSquared: { const float s = std::sin ((float) (t * halfPi)); return s * s; }
                case FadeCurve::quarterSine: break;
            }
            return std::sin ((float) (t * halfPi));
        }

        // The two gains of an equal-power crossfade at t (0: all the first, 1:
        // all the second), the convex curve each way.
        struct CrossfadeGains { float first, second; };
        inline CrossfadeGains crossfadeGains (float t)
        {
            return { fadeGainFloat (FadeCurve::quarterSine, t), fadeGainFloat (FadeCurve::quarterSine, 1.0f - t) };
        }

        // The other side of a crossfade at the same t: the gain of what fades out.
        inline double fadeOutGain (FadeCurve c, double t)
        {
            switch (c)
            {
                case FadeCurve::sineSquared: { const double s = std::cos (t * 3.141592653589793 / 2); return s * s; }
                case FadeCurve::linear:      return 1.0 - t;
                case FadeCurve::concave:
                case FadeCurve::sCurve:      return fadeGain (c, 1.0 - t);
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

        // The RMS level of one channel's n samples (0 for none), every
        // stride-th value for a channel interleaved with others.
        inline float rmsOf (const float* x, int n, int stride = 1)
        {
            if (n <= 0) return 0.0f;
            double sum = 0.0;
            for (int i = 0; i < n; ++i) { const double v = x[(std::ptrdiff_t) i * stride]; sum += v * v; }
            return (float) std::sqrt (sum / n);
        }

        // Whether a block is as good as silent: its first, middle and peak
        // samples all within 1/8000 of zero (about -78 dB), the test a DAW makes
        // before it skips work on a quiet block.
        inline bool isNearlySilent (const float* x, int n)
        {
            if (n <= 0) return true;
            constexpr float thresh = 1.0f / 8000.0f;
            const float* one[1] { x };
            return std::abs (x[0]) <= thresh && std::abs (x[n / 2]) <= thresh && peakOf (one, 1, n) <= thresh;
        }

        // A DAW's clean-up after a plugin, number for number as Tracktion Engine's:
        // every sample closer to zero than minAbs (about -108 dBFS by default), and
        // every NaN, becomes zero, and any sample beyond maxAbs is held at it.
        inline void sanitise (float* const* ch, int numCh, int n, float maxAbs, float minAbs = 1.0f / 262144.0f)
        {
            for (int c = 0; c < numCh; ++c)
                for (int i = 0; i < n; ++i)
                {
                    const float v = ch[c][i];
                    if (v >= minAbs) { if (v > maxAbs) ch[c][i] = maxAbs; }
                    else if (v <= -minAbs) { if (v < -maxAbs) ch[c][i] = -maxAbs; }
                    else ch[c][i] = 0.0f;
                }
        }

        // Every NaN sample becomes zero (what a DAW does to each plugin's output)
        inline void zeroNaN (float* const* ch, int numCh, int n)
        {
            for (int c = 0; c < numCh; ++c)
                for (int i = 0; i < n; ++i)
                    if (std::isnan (ch[c][i])) ch[c][i] = 0.0f;
        }

        // Every channel averaged into one (what a detector or an analyser listens to).
        inline void monoAverage (const float* const* ch, int numCh, int n, float* out)
        {
            if (numCh <= 0) { std::fill (out, out + n, 0.0f); return; }
            for (int i = 0; i < n; ++i)
            {
                float m = 0.0f;
                for (int c = 0; c < numCh; ++c) m += ch[c][i];
                out[i] = m / (float) numCh;
            }
        }

        // The gain in dB that brings a sample peak to targetDb (0 for silence),
        // kept within minDb .. maxDb.
        inline float gainToPeakDb (float peak, float targetDb, float minDb = -60.0f, float maxDb = 24.0f)
        {
            if (! (peak > 0.00001f)) return 0.0f;
            return std::clamp (targetDb - 20.0f * std::log10 (peak), minDb, maxDb);
        }

        // The running peak of |v|, NaN-aware as a browser's Math.max is: a NaN
        // sample makes the peak NaN.
        inline float runningPeak (float peak, float v)
        {
            return std::isnan (peak) || std::isnan (v) ? std::numeric_limits<float>::quiet_NaN() : std::max (peak, std::abs (v));
        }

        // A sound from where it first gets loud (0.3 of its peak), keeping
        // the two samples before: a synthesized hit lands right on its time.
        inline std::vector<float> fromOnset (const std::vector<float>& x)
        {
            float peak = 0;
            for (float v : x) peak = runningPeak (peak, v);
            size_t i = 0;
            while (i < x.size() && std::abs (x[i]) < 0.3 * peak) ++i;
            return std::vector<float> (x.begin() + (std::ptrdiff_t) (i >= 2 ? i - 2 : 0), x.end());
        }

        // A synthesized sound finished: its peak brought to `level` (a NaN or
        // silent sound left at its level), then a straight fade to silence
        // over its last fadeOut samples (fewer if it is shorter).
        inline void toPeakWithEndFade (std::vector<float>& x, double level, int fadeOut)
        {
            float peak = 0;
            for (float v : x) peak = runningPeak (peak, v);
            const double g = level / (peak != 0 && ! std::isnan (peak) ? (double) peak : 1.0);
            for (auto& v : x) v = (float) (v * g);
            float* ch[1] { x.data() };
            fadeSamples (ch, 1, (int) x.size(), 0, std::min ((int) x.size(), fadeOut), FadeCurve::linear, FadePoint::end);
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
