#pragma once

#include <algorithm>
#include <cmath>
#include "Ramp.h"

namespace vs
{
    // Gains and mixes, sample by sample: what a plugin's output stage, its
    // dry/wet knob or an on/off crossfade does. Each ramp moves once per
    // sample (one value for every channel at that sample), so a level change
    // never clicks.
    namespace mix
    {
        // n samples of each channel times a ramping gain (the gain moves once per sample)
        template <bool multiplicative>
        inline void applyGain (Ramp<multiplicative>& gain, float* const* ch, int numCh, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                const float g = gain.next();
                for (int c = 0; c < numCh; ++c) ch[c][i] *= g;
            }
        }

        // n samples of src times a fixed gain into dst
        inline void copyWithGain (const float* src, float* dst, int n, float gain)
        {
            for (int i = 0; i < n; ++i) dst[i] = gain * src[i];
        }

        // Moves each channel toward `other` by a ramping amount (0 keeps ch,
        // 1 is other): ch += amount x (other - ch)
        template <bool multiplicative>
        inline void crossfadeTo (Ramp<multiplicative>& amount, float* const* ch, const float* const* other, int numCh, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                const float m = amount.next();
                for (int c = 0; c < numCh; ++c) ch[c][i] += m * (other[c][i] - ch[c][i]);
            }
        }

        // A send-style dry/wet law for a mix amount (0 dry only .. 1 wet only):
        // dry stays full up to the middle, wet reaches full at the middle, so
        // the middle of the knob is "both at full"
        struct DryWet { float dry, wet; };
        inline DryWet sendLaw (float amount)
        {
            return { std::min (1.0f, 2.0f * (1.0f - amount)), std::min (1.0f, 2.0f * amount) };
        }

        // Mixes a stereo wet signal into the dry channels in place by the send
        // law, the amount ramping once per sample. One dry channel takes the
        // wet signal's mid (L + R) / 2. Returns the largest |wet| after its gain.
        template <bool multiplicative>
        inline float sendMix (Ramp<multiplicative>& amount, float* const* dry, int numDry, const float* wetL, const float* wetR, int n)
        {
            float peak = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const auto g = sendLaw (amount.next());
                const float wl = wetL[i] * g.wet, wr = wetR[i] * g.wet;
                peak = std::max ({ peak, std::abs (wl), std::abs (wr) });
                if (numDry == 1)
                    dry[0][i] = dry[0][i] * g.dry + 0.5f * (wl + wr);
                else
                {
                    dry[0][i] = dry[0][i] * g.dry + wl;
                    dry[1][i] = dry[1][i] * g.dry + wr;
                }
            }
            return peak;
        }
    }
}
