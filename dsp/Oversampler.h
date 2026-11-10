#pragma once

#include <algorithm>
#include <cmath>
#include <vector>
#include "Windows.h"

namespace vs
{
    // Power-of-two oversampling: 2x, 4x, 8x or 16x in stages of 2x, each stage
    // a 63-tap linear-phase half-band low-pass (Kaiser-windowed sinc, beta 8,
    // cut-off at a quarter of the higher rate). Up: zero-stuffing, then the
    // filter (gain 2 restores the level). Down: the filter, then every other
    // sample. Used where a process would otherwise alias (saturation) and to
    // see the peaks between samples (true-peak limiting); the same filters run
    // on the desktop and in the browser.
    //
    // Each stage keeps its filter's history in a ring buffer written twice
    // (at p and p + length) so the taps always read one contiguous run, and
    // does only the work whose result is used: up, the zero-stuffed samples
    // are left out of the sums (each output uses every other tap); down, only
    // the outputs that are kept are computed. The sums run in the same order
    // as a plain FIR over the zero-stuffed signal, so the results are the same
    // to the bit.
    template <typename T>
    class Oversampler
    {
    public:
        static constexpr int taps = 63;

        Oversampler (int numChannels, int factorLog2) : channels (numChannels), stages (factorLog2)
        {
            const int n = taps;
            const double beta = 8.0;
            const double pi = windows::pi;
            for (int i = 0; i < n; ++i)
            {
                const double m = i - (n - 1) / 2.0;
                const double sinc = m == 0.0 ? 0.5 : std::sin (pi * m * 0.5) / (pi * m);
                const double r = 2.0 * i / (n - 1) - 1.0;
                coeff[i] = (T) (sinc * windows::besselI0 (beta * std::sqrt (1.0 - r * r), 30, 0.0) / windows::besselI0 (beta, 30, 0.0));
            }
            for (int j = 0; j < upEvenTaps; ++j) evenTaps[j] = coeff[2 * j];
            for (int j = 0; j < upOddTaps; ++j) oddTaps[j] = coeff[2 * j + 1];
        }

        int getFactor() const { return 1 << stages; }
        int getNumChannels() const { return channels; }

        // Allocates the buffers for blocks of up to maxBlock samples (at the
        // base rate) and clears the filters.
        void initProcessing (int maxBlock)
        {
            for (int s = 0; s < stages; ++s)
                buf[s].assign ((size_t) channels, std::vector<T> ((size_t) maxBlock << (s + 1), T (0)));
            for (int s = 0; s < stages; ++s)
            {
                up[s].assign ((size_t) channels, Ring<upLength> {});
                down[s].assign ((size_t) channels, Ring<downLength> {});
            }
            ptrs.assign ((size_t) channels, nullptr);
            reset();
        }

        void reset()
        {
            for (int s = 0; s < stages; ++s)
            {
                for (auto& r : up[s]) r.clear();
                for (auto& r : down[s]) r.clear();
            }
        }

        // The delay of an up and down pass, in base-rate samples: each stage's
        // filter delays by (taps - 1) / 2 samples at its own rate, twice.
        float getLatencyInSamples() const
        {
            double lat = 0.0;
            for (int s = 0; s < stages; ++s) lat += 2.0 * ((taps - 1) / 2.0) / (double) (2u << s);
            return (float) lat;
        }

        // Upsamples n samples of each channel. Returns the oversampled
        // channels (n * getFactor() samples each), valid until the next call.
        T* const* processSamplesUp (const T* const* in, int nch, int n)
        {
            nch = std::min (nch, channels);
            for (int s = 0; s < stages; ++s)
            {
                for (int c = 0; c < nch; ++c)
                {
                    const T* src = s == 0 ? in[c] : buf[s - 1][(size_t) c].data();
                    T* dst = buf[s][(size_t) c].data();
                    auto& z = up[s][(size_t) c];
                    for (int i = 0; i < n; ++i)
                    {
                        const T* h = z.push (src[i] * T (2));
                        // with x pushed: x and the earlier inputs sit on the even taps
                        T acc = T (0);
                        for (int j = 0; j < upEvenTaps; ++j) acc += evenTaps[j] * h[j];
                        dst[2 * i] = acc;
                        // then a zero pushed: they move to the odd taps
                        acc = T (0);
                        for (int j = 0; j < upOddTaps; ++j) acc += oddTaps[j] * h[j];
                        dst[2 * i + 1] = acc;
                    }
                }
                n *= 2;
            }
            for (int c = 0; c < nch; ++c) ptrs[(size_t) c] = buf[stages - 1][(size_t) c].data();
            return ptrs.data();
        }

        // Downsamples the result of the last processSamplesUp (changed in
        // place or not) into out: n base-rate samples per channel.
        void processSamplesDown (T* const* out, int nch, int n)
        {
            nch = std::min (nch, channels);
            for (int s = stages; s-- > 0;)
            {
                const int nHigh = n << (s + 1);
                for (int c = 0; c < nch; ++c)
                {
                    const T* src = buf[s][(size_t) c].data();
                    T* dst = s == 0 ? out[c] : buf[s - 1][(size_t) c].data();
                    auto& z = down[s][(size_t) c];
                    for (int i = 0; i < nHigh; i += 2)
                    {
                        const T* h = z.push (src[i]);
                        T acc = T (0);
                        for (int k = 0; k < taps; ++k) acc += coeff[k] * h[k];
                        dst[i / 2] = acc;
                        z.push (src[i + 1]);
                    }
                }
            }
        }

    private:
        // Up: the zero-stuffed history holds one input every other tap, so
        // (taps + 1) / 2 inputs cover all the taps.
        static constexpr int upEvenTaps = (taps + 1) / 2, upOddTaps = taps / 2;
        static constexpr int upLength = upEvenTaps, downLength = taps;

        // The last L values, newest first, readable as one run of L.
        template <int L>
        struct Ring
        {
            T v[2 * L] {};
            int p = 0;

            void clear() { std::fill (v, v + 2 * L, T (0)); p = 0; }

            const T* push (T x)
            {
                p = p == 0 ? L - 1 : p - 1;
                v[p] = x;
                v[p + L] = x;
                return v + p;
            }
        };

        int channels, stages;
        T coeff[taps] {};
        T evenTaps[upEvenTaps] {}, oddTaps[upOddTaps] {};
        std::vector<std::vector<T>> buf[4];
        std::vector<Ring<upLength>> up[4];
        std::vector<Ring<downLength>> down[4];
        std::vector<T*> ptrs;
    };
}
