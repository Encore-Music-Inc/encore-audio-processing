#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>
#include "Biquad.h"
#include "Util.h"
#include "Windows.h"

namespace vs
{
    // Streaming resampler: reads the input at a position that moves stepAt (pos)
    // input samples per output sample (constant for a sample rate change or a
    // speed fix, varying for a wow fix). Windowed sinc (the shared SincTable),
    // 128 taps at full bandwidth, wider and with a lower cut-off when it has to
    // squeeze the audio (maxStep above 1), so nothing folds back as aliasing.
    //
    // push() takes the next block of every channel and returns how many output
    // samples it made, readable through output (channel) until the next call;
    // flush() after the last push makes the rest (zeros after the end give the
    // last samples their full window).
    class Resampler
    {
    public:
        using StepFunction = std::function<double (double)>;

        // stepAt (pos): input samples per output sample at input position pos.
        // maxStep: the largest step it will see (sets the anti-alias cut-off).
        Resampler (StepFunction stepFunction, double maxStep = 1.0, int channels = 2)
            : stepAt (std::move (stepFunction)),
              fc (0.97 * std::min (1.0, 1.0 / maxStep)),
              k (sincTable()),
              W ((int) std::ceil (k.halfWidth() / fc)),
              ch (channels),
              base (-W),
              buf ((size_t) channels, std::vector<float> ((size_t) W, 0.0f)),
              out ((size_t) channels),
              w ((size_t) (2 * W + 2), 0.0)
        {
        }

        // A constant step (fsIn / fsOut for a sample rate change).
        Resampler (double step, int channels)
            : Resampler ([step] (double) { return step; }, std::max (1.0, step), channels) {}

        // Takes the next n samples of each channel (a null channel repeats the first).
        int push (const float* const* chans, int n)
        {
            for (int c = 0; c < ch; ++c)
            {
                const float* src = chans[c] != nullptr ? chans[c] : chans[0];
                buf[(size_t) c].insert (buf[(size_t) c].end(), src, src + n);
            }
            inTotal += n;
            return run (false);
        }

        // The rest of the output, after the last push.
        int flush()
        {
            for (auto& b : buf) b.resize (b.size() + (size_t) W + 2, 0.0f);
            return run (true);
        }

        const float* output (int channel) const { return out[(size_t) channel].data(); }
        int outputSize() const { return (int) out[0].size(); }
        int channels() const { return ch; }
        // The cut-off as a fraction of the input's Nyquist, and the input samples each side of a read
        double cutoff() const { return fc; }
        int halfWidth() const { return W; }

    private:
        int run (bool final)
        {
            for (auto& o : out) o.clear();
            const long long end = base + (long long) buf[0].size(); // absolute index past the last buffered sample
            const double limit = final ? (double) inTotal : (double) (end - W - 1);
            while (pos < limit)
            {
                const double p = pos;
                const long long i0 = (long long) std::floor (p);
                const long long lo = i0 - W + 1, hi = i0 + W;
                int n = 0;
                for (long long j = lo; j <= hi; ++j, ++n) w[(size_t) n] = k.at (((double) j - p) * fc);
                const size_t off = (size_t) (lo - base);
                for (int c = 0; c < ch; ++c)
                {
                    const float* b = buf[(size_t) c].data() + off;
                    double s = 0.0;
                    for (int q = 0; q < n; ++q) s += w[(size_t) q] * b[q];
                    out[(size_t) c].push_back ((float) (s * fc));
                }
                pos += stepAt (p);
            }
            // drop what no future output needs
            const long long keepFrom = std::max (base, (long long) std::floor (pos) - W - 2);
            if (keepFrom > base)
            {
                const auto d = (std::ptrdiff_t) (keepFrom - base);
                for (auto& b : buf) b.erase (b.begin(), b.begin() + std::min (d, (std::ptrdiff_t) b.size()));
                base = keepFrom;
            }
            return (int) out[0].size();
        }

        StepFunction stepAt;
        double fc;
        const SincTable& k;
        int W, ch;
        long long base;          // absolute index of buf[0] (zeros before the start)
        long long inTotal = 0;
        double pos = 0.0;
        std::vector<std::vector<float>> buf, out;
        std::vector<double> w;
    };

    // A whole recording from fsIn to fsOut: round (n fsOut / fsIn) samples per
    // channel (zero where the resampler made fewer). out must hold that many.
    inline int resampledLength (int n, double fsIn, double fsOut) { return (int) roundHalfUp ((n * fsOut) / fsIn); }

    inline void resample (const float* const* in, int numCh, int n, double fsIn, double fsOut, float* const* out)
    {
        const int want = resampledLength (n, fsIn, fsOut);
        if (fsIn == fsOut)
        {
            for (int c = 0; c < numCh; ++c) std::copy (in[c], in[c] + n, out[c]);
            return;
        }
        for (int c = 0; c < numCh; ++c) std::fill (out[c], out[c] + want, 0.0f);
        Resampler r (fsIn / fsOut, numCh);
        int o = 0;
        auto put = [&] (int made)
        {
            const int room = std::max (0, std::min (made, want - o));
            for (int c = 0; c < numCh; ++c) std::copy (r.output (c), r.output (c) + room, out[c] + o);
            o += made;
        };
        constexpr int block = 1 << 16;
        std::vector<const float*> ptr ((size_t) numCh);
        for (int s = 0; s < n; s += block)
        {
            for (int c = 0; c < numCh; ++c) ptr[(size_t) c] = in[c] + s;
            put (r.push (ptr.data(), std::min (block, n - s)));
        }
        put (r.flush());
    }

    // Integer decimation, a block at a time: a low-pass, then every factor-th
    // sample, the phase carried from block to block. For analysis copies at a
    // lower rate. The usual filter is an 8th-order Butterworth at 0.42 fs /
    // factor run in double precision; the filter and whether each section's
    // output is stored as float before the next reads it can be chosen.
    class Decimator
    {
    public:
        Decimator (double fs, int factor)
            : Decimator (factor > 1 ? butterworth (BiquadCoefs::Type::lowPass, fs, (0.42 * fs) / factor, 8) : std::vector<BiquadCoefs> {}, factor) {}

        Decimator (std::vector<BiquadCoefs> sections, int factor, bool floatBetweenSections = false)
            : step (factor), floatStore (floatBetweenSections), filter (std::move (sections), 1) {}

        int factor() const { return step; }

        // The most samples push() makes from n samples
        int maxOutput (int n) const { return n / std::max (1, step) + 1; }

        // Filters n samples and writes the kept ones to out; returns how many.
        int push (const float* x, int n, float* out)
        {
            if (step <= 1 || filter.getSections().empty())
            {
                std::copy (x, x + n, out);
                return n;
            }
            int o = 0, i = phase;
            if (floatStore)
            {
                fbuf.assign (x, x + n);
                filter.process (0, fbuf.data(), n);
                for (; i < n; i += step) out[o++] = fbuf[(size_t) i];
            }
            else
            {
                dbuf.assign (x, x + n);
                filter.process (0, dbuf.data(), n);
                for (; i < n; i += step) out[o++] = (float) dbuf[(size_t) i];
            }
            phase = i - n;
            return o;
        }

        void reset() { filter.reset(); phase = 0; }

    private:
        int step, phase = 0;
        bool floatStore;
        BiquadChain filter;
        std::vector<float> fbuf;
        std::vector<double> dbuf;
    };

    // A low-band analysis copy of a whole signal: two passes of a 1 kHz
    // Butterworth-Q (0.7071) low-pass stored as float, then every d-th sample,
    // floor (n / d) of them. out must hold n / d + 1 samples. Returns the count.
    inline int decimateLowBand (const float* x, int n, double fs, int d, float* out)
    {
        const auto lp = BiquadCoefs::rbj (BiquadCoefs::Type::lowPass, fs, 1000.0, 0.7071067811865476);
        Decimator dec ({ lp, lp }, std::max (1, d), true);
        if (d <= 1)
        {
            // the filter still runs: only the keeping is skipped
            std::vector<float> tmp (x, x + n);
            BiquadChain ({ lp, lp }, 1).process (0, tmp.data(), n);
            std::copy (tmp.begin(), tmp.end(), out);
            return n;
        }
        dec.push (x, n, out);
        return n / d;
    }
}
