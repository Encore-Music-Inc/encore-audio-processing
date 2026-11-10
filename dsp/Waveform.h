#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "Biquad.h"

namespace vs
{
    // Waveform overviews: the small arrays a product draws a song's shape
    // from. Levels are linear (0 to 1 for full scale).
    namespace waveform
    {
        // A song in a fixed number of columns, block by block. Column c holds
        // samples [c per, (c + 1) per), per = ceil (frames / columns), so the
        // song's length must be known first. Per column, as asked:
        //   peak        the largest |x| of both channels
        //   peakL/R     each channel's largest |x|
        //   rms         sqrt of the mean of (L^2 + R^2) / 2
        //   bands       the lows (under about 200 Hz), mids and highs (over
        //               about 3 kHz) of (L + R) / 2, each an RMS level: two
        //               RBJ low-passes at 200 Hz, two high-passes at 3 kHz,
        //               mids = the rest; for colouring a waveform by frequency
        // stride: the peaks read every stride-th sample of a column, from its
        // first (2 halves the work; the RMS and bands read every sample).
        class Overview
        {
        public:
            struct Options
            {
                int columns = 1200;
                int stride = 1;
                bool rms = false, bands = false;
            };

            void prepare (double sampleRate, std::int64_t totalFrames, const Options& o)
            {
                opt = o;
                opt.columns = std::max (1, opt.columns);
                opt.stride = std::max (1, opt.stride);
                per = std::max<std::int64_t> (1, (totalFrames + opt.columns - 1) / opt.columns);
                const size_t cols = (size_t) opt.columns;
                peaks.assign (cols, 0.0f);
                peaksL.assign (cols, 0.0f);
                peaksR.assign (cols, 0.0f);
                sumSq.assign (cols, 0.0);
                count.assign (cols, 0);
                bandSum.assign (cols * 3, 0.0);
                if (opt.bands)
                {
                    const auto lp = BiquadCoefs::rbj (BiquadCoefs::Type::lowPass, sampleRate, 200.0, 0.7071);
                    const auto hp = BiquadCoefs::rbj (BiquadCoefs::Type::highPass, sampleRate, 3000.0, 0.7071);
                    lows.setSections ({ lp, lp }, 1);
                    highs.setSections ({ hp, hp }, 1);
                }
                at = 0;
            }

            // r null for one channel.
            void process (const float* l, const float* r, int n)
            {
                if (r == nullptr) r = l;
                if (opt.bands)
                {
                    mid.resize ((size_t) n); lo.resize ((size_t) n); hi.resize ((size_t) n);
                    for (int i = 0; i < n; ++i) mid[(size_t) i] = 0.5 * ((double) l[i] + (double) r[i]);
                    std::copy (mid.begin(), mid.begin() + n, lo.begin());
                    std::copy (mid.begin(), mid.begin() + n, hi.begin());
                    lows.process (0, lo.data(), n);
                    highs.process (0, hi.data(), n);
                }
                for (int i = 0; i < n; ++i)
                {
                    const std::int64_t g = at + i;
                    const size_t c = (size_t) std::min<std::int64_t> (opt.columns - 1, g / per);
                    const double x = l[i], y = r[i];
                    if ((g - (std::int64_t) c * per) % opt.stride == 0)
                    {
                        const float ax = std::abs (l[i]), ay = std::abs (r[i]);
                        peaksL[c] = std::max (peaksL[c], ax);
                        peaksR[c] = std::max (peaksR[c], ay);
                        peaks[c] = std::max (peaks[c], std::max (ax, ay));
                    }
                    if (opt.rms) sumSq[c] += 0.5 * (x * x + y * y);
                    if (opt.bands)
                    {
                        const double a = lo[(size_t) i], h = hi[(size_t) i], m = mid[(size_t) i] - a - h;
                        bandSum[c * 3] += a * a;
                        bandSum[c * 3 + 1] += m * m;
                        bandSum[c * 3 + 2] += h * h;
                    }
                    ++count[c];
                }
                at += n;
            }

            int columns() const { return opt.columns; }
            std::int64_t samplesPerColumn() const { return per; }
            const std::vector<float>& peak() const { return peaks; }
            const std::vector<float>& peakL() const { return peaksL; }
            const std::vector<float>& peakR() const { return peaksR; }

            std::vector<float> rms() const
            {
                std::vector<float> out (sumSq.size());
                for (size_t c = 0; c < out.size(); ++c) out[c] = (float) std::sqrt (sumSq[c] / (double) std::max<std::int64_t> (1, count[c]));
                return out;
            }

            // lows, mids, highs per column (3 values each); 0 for an empty column
            std::vector<float> bands() const
            {
                std::vector<float> out (bandSum.size(), 0.0f);
                for (size_t c = 0; c < count.size(); ++c)
                    if (count[c] > 0)
                        for (size_t k = 0; k < 3; ++k) out[c * 3 + k] = (float) std::sqrt (bandSum[c * 3 + k] / (double) count[c]);
                return out;
            }

        private:
            Options opt;
            std::int64_t per = 1, at = 0;
            std::vector<float> peaks, peaksL, peaksR;
            std::vector<double> sumSq, bandSum, mid, lo, hi;
            std::vector<std::int64_t> count;
            BiquadChain lows, highs;
        };

        // The largest |x| of every block of blockSize samples, for a song of
        // any length (the last, part-filled block counts too). One channel.
        class BlockPeaks
        {
        public:
            explicit BlockPeaks (int blockSize = 100) : size (std::max (1, blockSize)) {}

            template <typename T>
            void process (const T* x, int n)
            {
                for (int i = 0; i < n; ++i)
                {
                    const double v = std::abs ((double) x[i]);
                    if (v > blockPeak) blockPeak = v;
                    if (++inBlock == size) { out.push_back (blockPeak); blockPeak = 0.0; inBlock = 0; }
                }
            }

            // The peaks, the part-filled last block included.
            std::vector<double> peaks() const
            {
                auto p = out;
                if (inBlock > 0) p.push_back (blockPeak);
                return p;
            }

        private:
            int size, inBlock = 0;
            double blockPeak = 0.0;
            std::vector<double> out;
        };

        // A song's shape in `points` bytes from its block peaks: each point
        // the largest peak of its stretch (blocks are repeated when there are
        // fewer than points), scaled so the loudest point is 255 (silence
        // stays 0). Empty when there are no peaks.
        inline std::vector<std::uint8_t> shape (const double* peaks, int n, int points = 800)
        {
            if (n <= 0 || points <= 0) return {};
            std::vector<double> buckets ((size_t) points);
            for (int i = 0; i < points; ++i)
            {
                const int from = (int) (((std::int64_t) i * n) / points);
                const int to = std::max (from + 1, (int) (((std::int64_t) (i + 1) * n) / points));
                double loudest = 0.0;
                for (int j = from; j < to; ++j) loudest = std::max (loudest, std::abs (peaks[j]));
                buckets[(size_t) i] = loudest;
            }
            const double top = *std::max_element (buckets.begin(), buckets.end());
            std::vector<std::uint8_t> out ((size_t) points, 0);
            if (top > 0.0)
                for (int i = 0; i < points; ++i) out[(size_t) i] = (std::uint8_t) std::floor ((buckets[(size_t) i] / top) * 255.0 + 0.5);
            return out;
        }

        // Peaks for drawing a short sound `width` pixels wide: pixel x covers
        // samples [x n / w, max (that + 1, (x + 1) n / w)), read every
        // (span / 64)-th sample, the largest |x| of all the channels.
        inline std::vector<float> pixelPeaks (const float* const* ch, int numChannels, std::int64_t n, int width)
        {
            std::vector<float> out ((size_t) std::max (0, width), 0.0f);
            if (n <= 0) return out;
            for (int x = 0; x < width; ++x)
            {
                const std::int64_t a = (std::int64_t) std::floor ((double) x / width * (double) n);
                const std::int64_t e = std::max (a + 1, (std::int64_t) std::floor ((double) (x + 1) / width * (double) n));
                const std::int64_t step = std::max<std::int64_t> (1, (e - a) >> 6);
                float pk = 0.0f;
                for (int c = 0; c < numChannels; ++c)
                    for (std::int64_t i = a; i < e && i < n; i += step) pk = std::max (pk, std::abs (ch[c][i]));
                out[(size_t) x] = pk;
            }
            return out;
        }
    }
}
