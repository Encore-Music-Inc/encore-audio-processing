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
        // Pan laws: how a pan position (-1 left .. 0 centre .. 1 right) splits a
        // gain between left and right. linear: the side away from the pan loses
        // pan x gain and the side toward it gains it (centre is both at full
        // gain). The others are equal-power sine laws, quieter in the centre by
        // the amount in their name: sin of the position to each side, raised to
        // the law's power (1 for 3 dB, 2.5 / 3 for 2.5 dB, 1.5 for 4.5 dB, 2 for
        // 6 dB). These are the laws a mixer's pan knob offers, number for number
        // as Tracktion Engine computes them, so songs saved with them sound the same.
        enum class PanLaw { linear, centre3dB, centre2_5dB, centre4_5dB, centre6dB };

        struct PanGains { float left, right; };

        inline PanGains panGains (float gain, float pan, PanLaw law)
        {
            if (law == PanLaw::linear)
            {
                const float pv = pan * gain;
                return { gain - pv, gain + pv };
            }
            constexpr float halfPi = 3.14159265358979323846f * 0.5f;
            const float p = (pan + 1.0f) * 0.5f;
            const float l = std::sin ((1.0f - p) * halfPi), r = std::sin (p * halfPi);
            const float power = law == PanLaw::centre2_5dB ? 2.5f / 3.0f
                              : law == PanLaw::centre4_5dB ? 1.5f
                              : law == PanLaw::centre6dB   ? 2.0f
                              : 1.0f;
            return { std::pow (l, power) * gain, std::pow (r, power) * gain };
        }

        // n samples of each channel times a fixed gain (the gain stage with no ramp)
        inline void applyGain (float gain, float* const* ch, int numCh, int n)
        {
            if (gain == 1.0f) return;
            for (int c = 0; c < numCh; ++c)
                for (int i = 0; i < n; ++i) ch[c][i] *= gain;
        }

        // n samples of x times a gain moving in equal steps from start (on the
        // first sample) toward end, the step (end - start) / n added after each
        // sample: a click-free mute or a short fade, number for number as JUCE's
        // applyGainRamp
        inline void applyGainRamp (float* x, int n, float start, float end)
        {
            if (n <= 0) return;
            const float increment = (end - start) / (float) n;
            while (--n >= 0)
            {
                *x++ *= start;
                start += increment;
            }
        }

        // n samples of src times a fixed gain added into dst
        inline void addWithGain (const float* src, float* dst, int n, float gain)
        {
            for (int i = 0; i < n; ++i) dst[i] += gain * src[i];
        }
    }
}
