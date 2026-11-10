#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include "Biquad.h"

namespace vs
{
    // What the processors that turn a sound down by its level share: the gain
    // curve (how much to cut for how far a level is over its threshold), the
    // glide that moves a cut toward where it should be, and the bell whose
    // depth moves (a dynamic EQ band).
    namespace dynamics
    {
        inline constexpr float noLimit = std::numeric_limits<float>::infinity();

        // The gain curve. How many dB to cut a level that is overDb above the
        // threshold: slope dB of cut per dB over (1 - 1 / ratio for a
        // compressor; 1.5 cuts half as much again as the overshoot), with a
        // soft knee kneeDb wide centred on the threshold (0: a hard knee, no
        // cut at or below the threshold), and never more than maxCutDb.
        inline float cutDb (float overDb, float slope, float kneeDb = 0.0f, float maxCutDb = noLimit)
        {
            float cut = 0.0f;
            if (overDb > kneeDb * 0.5f) cut = overDb * slope;
            else if (overDb > -kneeDb * 0.5f) cut = slope * (overDb + kneeDb * 0.5f) * (overDb + kneeDb * 0.5f) / (2.0f * kneeDb);
            return std::min (cut, maxCutDb);
        }

        // The same curve as the level that comes out (dB) for a level going
        // in, for a compressor with this threshold and ratio: unchanged below
        // the knee, thresholdDb + over / ratio above it.
        inline float compressedDb (float levelDb, float thresholdDb, float ratio, float kneeDb)
        {
            const float d = levelDb - thresholdDb;
            if (2.0f * d < -kneeDb) return levelDb;
            if (2.0f * std::abs (d) <= kneeDb)
            {
                const float t = d + kneeDb * 0.5f;
                return levelDb + (1.0f / ratio - 1.0f) * t * t / (2.0f * kneeDb);
            }
            return thresholdDb + d / ratio;
        }

        // Moves a cut (dB, more is deeper) toward its target once per block
        // of blockSec: exponentially, with attackSec while it deepens and
        // releaseSec while it lets go. With snapToZero a cut under 0.001 dB
        // heading for none becomes none.
        inline void glideCut (float& cut, float target, float blockSec, float attackSec, float releaseSec, bool snapToZero = true)
        {
            const float tau = target > cut ? attackSec : releaseSec;
            cut += (1.0f - std::exp (-blockSec / tau)) * (target - cut);
            if (snapToZero && cut < 0.001f && target <= 0.0f) cut = 0.0f;
        }
    }

    // A bell cut whose depth changes over time (a dynamic EQ band): process()
    // takes the cut for each block and redesigns the bell (RBJ peak at
    // -cut x gainScale dB) when it has moved by more than 0.05 dB since the
    // last design. A cut under flatBelowDb is exactly flat (pass-through
    // coefficients) instead of a very shallow bell.
    class DynamicBell
    {
    public:
        void prepare (double sampleRate, float freqHz, double bellQ, float flatBelowDb = 0.01f, float gainScale = 1.0f)
        {
            fs = sampleRate; freq = freqHz; q = bellQ;
            flatBelow = flatBelowDb;
            scale = gainScale;
            eq.reset();
            eq.c = BiquadCoefs {};
            applied = 0.0f;
        }

        // A new frequency, designed in at once.
        void setFreq (float hz)
        {
            if (std::abs (hz - freq) < 0.01f) return;
            freq = hz;
            applied = -1.0f; // force a redesign
        }

        // A new frequency and Q, used from the next redesign.
        void setShape (float hz, double bellQ) { freq = hz; q = bellQ; }

        // setCut, then filter.
        void process (float* const* ch, int numCh, int start, int len, float cutDb)
        {
            setCut (cutDb);
            filter (ch, numCh, start, len);
        }

        // The cut from now on (redesigned if it moved by more than 0.05 dB).
        void setCut (float cutDb)
        {
            if (std::abs (cutDb - applied) > 0.05f)
            {
                applied = cutDb;
                eq.c = cutDb < flatBelow ? BiquadCoefs {} : BiquadCoefs::peak (fs, freq, q, -cutDb * scale);
            }
        }

        // Runs the bell over samples [start, start + len) of each channel, in place.
        void filter (float* const* ch, int numCh, int start, int len)
        {
            for (int c = 0; c < numCh; ++c)
                for (int i = start; i < start + len; ++i)
                    ch[c][i] = eq.process (c, ch[c][i]);
        }

        // Flat again, filter state cleared.
        void reset() { eq.reset(); eq.c = BiquadCoefs {}; applied = 0.0f; }

        // Filter state cleared, and the next process() designs the bell whatever the cut.
        void restart() { eq.reset(); applied = 999.0f; }

        // Filter state cleared, the design kept.
        void clearState() { eq.reset(); }

        // The cut the bell was last designed for.
        float appliedCutDb() const { return applied; }

    private:
        double fs = 48000.0, q = 1.0;
        float freq = 1000.0f, applied = 0.0f, flatBelow = 0.01f, scale = 1.0f;
        Biquad eq;
    };
}
