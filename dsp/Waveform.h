#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "Biquad.h"
#include "Util.h"

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
                    for (int i = 0; i < n; ++i) mid[(size_t) i] = midOf ((double) l[i], (double) r[i]);
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

        // A DAW's waveform picture: the lowest and highest sample of every
        // block of blockSize samples, per channel, added as audio arrives (a
        // file read in the background, or a recording as it is made), and read
        // back for any stretch of samples (a pixel's worth at any zoom) as the
        // lowest and highest of the blocks it touches.
        class MinMax
        {
        public:
            struct Range { float lo = 0.0f, hi = 0.0f; };

            void reset (int numChannels, int blockSize = 256, std::int64_t expectedSamples = 0)
            {
                size = std::max (1, blockSize);
                ch = std::max (1, numChannels);
                lows.assign ((size_t) ch, {});
                highs.assign ((size_t) ch, {});
                for (int c = 0; c < ch; ++c)
                {
                    lows[(size_t) c].reserve ((size_t) (expectedSamples / size + 1));
                    highs[(size_t) c].reserve ((size_t) (expectedSamples / size + 1));
                }
                partLo.assign ((size_t) ch, 0.0f);
                partHi.assign ((size_t) ch, 0.0f);
                inBlock = 0;
                total = 0;
            }

            // n more samples of each channel (a missing channel repeats the first)
            void add (const float* const* chans, int numChans, int n)
            {
                for (int i = 0; i < n; ++i)
                {
                    for (int c = 0; c < ch; ++c)
                    {
                        const float v = chans[std::min (c, numChans - 1)][i];
                        if (inBlock == 0) { partLo[(size_t) c] = v; partHi[(size_t) c] = v; }
                        else { partLo[(size_t) c] = std::min (partLo[(size_t) c], v); partHi[(size_t) c] = std::max (partHi[(size_t) c], v); }
                    }
                    if (++inBlock == size)
                    {
                        for (int c = 0; c < ch; ++c) { lows[(size_t) c].push_back (partLo[(size_t) c]); highs[(size_t) c].push_back (partHi[(size_t) c]); }
                        inBlock = 0;
                    }
                }
                total += n;
            }

            int channels() const { return ch; }
            int blockSize() const { return size; }
            std::int64_t samples() const { return total; }

            // The lowest and highest of channel c over samples [from, to)
            Range range (int c, std::int64_t from, std::int64_t to) const
            {
                Range r;
                if (c < 0 || c >= ch || to <= from) return r;
                const auto& lo = lows[(size_t) c];
                const auto& hi = highs[(size_t) c];
                const std::int64_t full = (std::int64_t) lo.size();
                std::int64_t b0 = std::max<std::int64_t> (0, from / size), b1 = (to - 1) / size;
                bool any = false;
                for (std::int64_t b = b0; b <= b1 && b < full; ++b)
                {
                    r.lo = any ? std::min (r.lo, lo[(size_t) b]) : lo[(size_t) b];
                    r.hi = any ? std::max (r.hi, hi[(size_t) b]) : hi[(size_t) b];
                    any = true;
                }
                if (b1 >= full && inBlock > 0)
                {
                    r.lo = any ? std::min (r.lo, partLo[(size_t) c]) : partLo[(size_t) c];
                    r.hi = any ? std::max (r.hi, partHi[(size_t) c]) : partHi[(size_t) c];
                }
                return r;
            }

            // The largest |x| of every channel so far
            float peak() const
            {
                float p = 0.0f;
                for (int c = 0; c < ch; ++c)
                {
                    const auto r = range (c, 0, total);
                    p = std::max ({ p, std::abs (r.lo), std::abs (r.hi) });
                }
                return p;
            }

        private:
            int size = 256, ch = 1, inBlock = 0;
            std::int64_t total = 0;
            std::vector<std::vector<float>> lows, highs;
            std::vector<float> partLo, partHi;
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
