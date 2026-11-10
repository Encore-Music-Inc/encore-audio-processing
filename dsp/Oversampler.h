#pragma once

#include <algorithm>
#include <cmath>
#include <vector>
#include "Windows.h"

namespace vs
{
    // Power-of-two oversampling: 2x, 4x, 8x or 16x in stages of 2x, each stage
    // a linear-phase half-band low-pass (Kaiser-windowed sinc, cut-off at a
    // quarter of the higher rate; 63 taps with beta 8 unless set longer, which
    // keeps more of the top octave). Up: zero-stuffing, then the
    // filter (gain 2 restores the level). Down: the filter, then every other
    // sample. Used where a process would otherwise alias (saturation) and to
    // see the peaks between samples (true-peak limiting); the same filters run
    // on the desktop and in the browser.
    //
    // Each stage does only the work whose result is used: up, the
    // zero-stuffed samples are left out of the sums (each output uses every
    // other tap); down, only the outputs that are kept are computed. Up, a
    // stage keeps its last inputs in front of the new block in one line and
    // sums tap by tap over the whole block (so the sums of several outputs
    // run side by side); down, it keeps its history in a ring buffer written
    // twice (at p and p + length) so the taps always read one contiguous run.
    // Each output's sum runs in the same order as a plain FIR over the
    // zero-stuffed signal, so the results are the same to the bit.
    template <typename T>
    class Oversampler
    {
    public:
        // numTaps: odd, 7 or more.
        Oversampler (int numChannels, int factorLog2, int numTaps = 63, double kaiserBeta = 8.0)
            : channels (numChannels), stages (factorLog2), taps (numTaps),
              upEvenTaps ((numTaps + 1) / 2), upOddTaps (numTaps / 2),
              coeff ((size_t) numTaps), evenTaps ((size_t) upEvenTaps), oddTaps ((size_t) upOddTaps)
        {
            const int n = taps;
            const double beta = kaiserBeta;
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

        // The most an upsampled value can be for inputs of at most 1 in size:
        // each stage's larger tap sum (even or odd, times its gain of 2),
        // multiplied over the stages.
        double upGainBound() const
        {
            double e = 0.0, o = 0.0;
            for (auto t : evenTaps) e += std::abs ((double) t);
            for (auto t : oddTaps) o += std::abs ((double) t);
            return std::pow (2.0 * std::max (e, o), stages);
        }

        // How many inputs (at the base rate) the upsampler remembers: after a
        // reset, upsampling this many inputs leaves it exactly as if it had
        // been running all along.
        int upMemory() const
        {
            int m = 0;
            for (int s = 0; s < stages; ++s) m += (upEvenTaps - 1 + (1 << s) - 1) >> s;
            return m;
        }
        int getNumTaps() const { return taps; }
        int getNumChannels() const { return channels; }

        // Allocates the buffers for blocks of up to maxBlock samples (at the
        // base rate) and clears the filters.
        void initProcessing (int maxBlock)
        {
            for (int s = 0; s < stages; ++s)
                buf[s].assign ((size_t) channels, std::vector<T> ((size_t) maxBlock << (s + 1), T (0)));
            for (int s = 0; s < stages; ++s)
            {
                line[s].assign ((size_t) channels, std::vector<T> ((size_t) (upEvenTaps - 1 + (maxBlock << s)), T (0)));
                down[s].assign ((size_t) channels, Ring (taps));
            }
            ptrs.assign ((size_t) channels, nullptr);
            reset();
        }

        void reset()
        {
            for (int s = 0; s < stages; ++s)
            {
                for (auto& l : line[s]) std::fill (l.begin(), l.end(), T (0));
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
                    auto& l = line[s][(size_t) c];
                    const int keep = upEvenTaps - 1;
                    for (int i = 0; i < n; ++i) l[(size_t) (keep + i)] = src[i] * T (2);
                    // output i reads input i - j at tap j (newest first): with x
                    // pushed, the inputs sit on the even taps; with the zero
                    // after it, on the odd taps. Sixteen outputs at a time,
                    // each summed tap by tap in that order.
                    const T* base = l.data() + keep;
                    int i0 = 0;
                    for (; i0 + tile <= n; i0 += tile)
                    {
                        T ae[tile] {}, ao[tile] {};
                        // (upOddTaps is upEvenTaps - 1: the last even tap on its own)
                        for (int j = 0; j < upOddTaps; ++j)
                        {
                            const T te = evenTaps[(size_t) j], to = oddTaps[(size_t) j];
                            const T* x = base + i0 - j;
                            for (int k = 0; k < tile; ++k) { ae[k] += te * x[k]; ao[k] += to * x[k]; }
                        }
                        for (int j = upOddTaps; j < upEvenTaps; ++j)
                        {
                            const T te = evenTaps[(size_t) j];
                            const T* x = base + i0 - j;
                            for (int k = 0; k < tile; ++k) ae[k] += te * x[k];
                        }
                        for (int k = 0; k < tile; ++k) { dst[2 * (i0 + k)] = ae[k]; dst[2 * (i0 + k) + 1] = ao[k]; }
                    }
                    for (int i = i0; i < n; ++i)
                    {
                        T e = T (0), o = T (0);
                        for (int j = 0; j < upEvenTaps; ++j) e += evenTaps[(size_t) j] * base[i - j];
                        for (int j = 0; j < upOddTaps; ++j) o += oddTaps[(size_t) j] * base[i - j];
                        dst[2 * i] = e;
                        dst[2 * i + 1] = o;
                    }
                    std::copy (l.begin() + n, l.begin() + n + keep, l.begin());
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
        // The last L values, newest first, readable as one run of L.
        struct Ring
        {
            explicit Ring (int length) : L (length), v ((size_t) (2 * length), T (0)) {}

            int L;
            std::vector<T> v;
            int p = 0;

            void clear() { std::fill (v.begin(), v.end(), T (0)); p = 0; }

            const T* push (T x)
            {
                p = p == 0 ? L - 1 : p - 1;
                v[(size_t) p] = x;
                v[(size_t) (p + L)] = x;
                return v.data() + p;
            }
        };

        int channels, stages, taps;
        // Up: the zero-stuffed history holds one input every other tap, so
        // (taps + 1) / 2 inputs cover all the taps.
        int upEvenTaps, upOddTaps;
        std::vector<T> coeff, evenTaps, oddTaps;
        std::vector<std::vector<T>> buf[4];
        // up: per stage and channel, the last upEvenTaps - 1 inputs, then the block
        std::vector<std::vector<T>> line[4];
        static constexpr int tile = 16;
        std::vector<Ring> down[4];
        std::vector<T*> ptrs;
    };
}
