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
    // The half-band filter's taps at even distances from the centre (all but
    // the centre itself) are 0: they are set to exactly 0 in the design, and
    // left out of the sums.
    //
    // Each stage does only the work whose result is used: up, the
    // zero-stuffed samples and the zero taps are left out of the sums, so
    // one of each pair of outputs is a single tap times its input; down, only
    // the outputs that are kept are computed, from the nonzero taps. Both
    // directions keep a stage's last inputs in front of the new block in one
    // line (down: two lines, the even and the odd samples, so each tap reads
    // one contiguous run) and sum tap by tap over the whole block, so the
    // sums of sixteen outputs run side by side. Each output's sum runs over
    // its taps in order, nearest the newest input first.
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
                // 0 at every even distance from the centre: sin (pi m / 2) is
                // 0 there, though not when computed in floating point
                const bool zero = m != 0.0 && std::fmod (m, 2.0) == 0.0;
                const double sinc = m == 0.0 ? 0.5 : zero ? 0.0 : std::sin (pi * m * 0.5) / (pi * m);
                const double r = 2.0 * i / (n - 1) - 1.0;
                coeff[i] = (T) (sinc * windows::kaiserTaper (r, beta, 30, 0.0) / windows::besselI0 (beta, 30, 0.0));
            }
            for (int j = 0; j < upEvenTaps; ++j) evenTaps[j] = coeff[2 * j];
            for (int j = 0; j < upOddTaps; ++j) oddTaps[j] = coeff[2 * j + 1];
            // the nonzero taps, in order: up, for each of the two outputs per
            // input, tap 2j (+1) reads the input j back; down, tap k reads the
            // even line k / 2 back (k even) or the odd line (k + 1) / 2 back
            for (int k = 0; k < n; ++k)
            {
                if (coeff[k] == T (0)) continue;
                upNonzero[k & 1].push_back ({ 0, k / 2, coeff[k] });
                downNonzero.push_back ({ k & 1, (k + 1) / 2, coeff[k] });
            }
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
                downEven[s].assign ((size_t) channels, std::vector<T> ((size_t) (upOddTaps + (maxBlock << s)), T (0)));
                downOdd[s].assign ((size_t) channels, std::vector<T> ((size_t) (upOddTaps + (maxBlock << s)), T (0)));
            }
            ptrs.assign ((size_t) channels, nullptr);
            reset();
        }

        void reset()
        {
            for (int s = 0; s < stages; ++s)
            {
                for (auto& l : line[s]) std::fill (l.begin(), l.end(), T (0));
                for (auto& l : downEven[s]) std::fill (l.begin(), l.end(), T (0));
                for (auto& l : downOdd[s]) std::fill (l.begin(), l.end(), T (0));
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
                    // output 2i + p is the sum of tap 2j + p times input i - j
                    const T* base = l.data() + keep;
                    for (int p = 0; p < 2; ++p)
                    {
                        const auto& list = upNonzero[p];
                        int i0 = 0;
                        for (; i0 + tile <= n; i0 += tile)
                        {
                            T a[tile] {};
                            for (const auto& t : list)
                            {
                                const T* x = base + i0 - t.back;
                                for (int k = 0; k < tile; ++k) a[k] += t.value * x[k];
                            }
                            for (int k = 0; k < tile; ++k) dst[2 * (i0 + k) + p] = a[k];
                        }
                        for (int i = i0; i < n; ++i)
                        {
                            T a = T (0);
                            for (const auto& t : list) a += t.value * base[i - t.back];
                            dst[2 * i + p] = a;
                        }
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
                const int m = n << s; // outputs of this stage
                for (int c = 0; c < nch; ++c)
                {
                    const T* src = buf[s][(size_t) c].data();
                    T* dst = s == 0 ? out[c] : buf[s - 1][(size_t) c].data();
                    auto& le = downEven[s][(size_t) c];
                    auto& lo = downOdd[s][(size_t) c];
                    const int keep = upOddTaps;
                    for (int i = 0; i < m; ++i)
                    {
                        le[(size_t) (keep + i)] = src[2 * i];
                        lo[(size_t) (keep + i)] = src[2 * i + 1];
                    }
                    // output i is the sum of tap k times input 2i - k: the even
                    // line k / 2 back, or the odd line (k + 1) / 2 back
                    const T* lines[2] { le.data() + keep, lo.data() + keep };
                    int i0 = 0;
                    for (; i0 + tile <= m; i0 += tile)
                    {
                        T a[tile] {};
                        for (const auto& t : downNonzero)
                        {
                            const T* x = lines[t.line] + i0 - t.back;
                            for (int k = 0; k < tile; ++k) a[k] += t.value * x[k];
                        }
                        for (int k = 0; k < tile; ++k) dst[i0 + k] = a[k];
                    }
                    for (int i = i0; i < m; ++i)
                    {
                        T a = T (0);
                        for (const auto& t : downNonzero) a += t.value * lines[t.line][i - t.back];
                        dst[i] = a;
                    }
                    std::copy (le.begin() + m, le.begin() + m + keep, le.begin());
                    std::copy (lo.begin() + m, lo.begin() + m + keep, lo.begin());
                }
            }
        }

    private:
        // A nonzero tap: its value, and how far back (and, down, in which line) its input is.
        struct Tap
        {
            int line, back;
            T value;
        };

        int channels, stages, taps;
        // Up: the zero-stuffed history holds one input every other tap, so
        // (taps + 1) / 2 inputs cover all the taps.
        int upEvenTaps, upOddTaps;
        std::vector<T> coeff, evenTaps, oddTaps;
        std::vector<std::vector<T>> buf[4];
        // up: per stage and channel, the last upEvenTaps - 1 inputs, then the block
        std::vector<std::vector<T>> line[4];
        // down: per stage and channel, the last upOddTaps even and odd
        // inputs, then the block's
        std::vector<std::vector<T>> downEven[4], downOdd[4];
        std::vector<Tap> upNonzero[2], downNonzero;
        static constexpr int tile = 16;
        std::vector<T*> ptrs;
    };
}
