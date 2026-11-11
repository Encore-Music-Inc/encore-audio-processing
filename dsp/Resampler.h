#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <vector>
#include "Biquad.h"
#include "Util.h"
#include "Windows.h"

namespace vs
{
    // The sample rate an MP3 of a song at songRate is made at: 48 kHz for the
    // 48 kHz family (and 32 kHz), else 44.1 kHz (MPEG audio carries only
    // these rates and their halves; the nearer family keeps the conversion simple)
    inline double mp3Rate (double songRate)
    {
        return std::fmod (songRate, 48000.0) == 0.0 || songRate == 32000.0 ? 48000.0 : 44100.0;
    }

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
    //
    // How it sums. A constant step that is a ratio of small whole numbers, M
    // input samples for every L output samples (every pair of the usual
    // sample rates: 96 kHz to 48 kHz is 2/1, 44.1 kHz to 48 kHz is 147/160),
    // keeps its position as a whole sample and a phase in L parts, so it
    // stays exact however long the recording, and reads its taps from a table
    // of the L phases made once. A whole-number step (L = 1) splits the input
    // into M lines (every M-th sample in each), so each tap of sixteen
    // consecutive outputs reads one contiguous run, and sums the sixteen side
    // by side, each over its taps in order. Any other step adds the step to
    // its position and looks each tap up in the SincTable. Every sum is in
    // double precision and runs over the taps in order, except one read from
    // the table an output at a time (L above 1, or M above 16), which keeps
    // four running sums (taps 0, 4, 8 ... in the first) and adds them at the
    // end.
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
              taps (2 * W),
              ch (channels),
              base (-W),
              buf ((size_t) channels, std::vector<double> ((size_t) W, 0.0)),
              out ((size_t) channels),
              w ((size_t) (2 * W + 2), 0.0)
        {
        }

        // A constant step (fsIn / fsOut for a sample rate change).
        Resampler (double step, int channels)
            : Resampler ([step] (double) { return step; }, std::max (1.0, step), channels)
        {
            findPhases (step);
        }

        // A sample rate change from fsIn to fsOut. Whole-number rates give their ratio M / L
        // exactly (by their greatest common divisor), whatever the compiler does to division;
        // other rates go by the step.
        Resampler (double fsIn, double fsOut, int channels)
            : Resampler ([s = fsIn / fsOut] (double) { return s; }, std::max (1.0, fsIn / fsOut), channels)
        {
            const bool whole = fsIn >= 1 && fsOut >= 1 && fsIn <= 1e9 && fsOut <= 1e9
                               && std::floor (fsIn) == fsIn && std::floor (fsOut) == fsOut;
            if (whole)
            {
                const long long a = (long long) fsIn, b = (long long) fsOut, g = gcd (a, b);
                const long long m = a / g, l = b / g;
                if (l <= maxPhases && l * taps <= maxTable) usePhases (m, (int) l);
                return;
            }
            findPhases (fsIn / fsOut);
        }

    private:
        // The smallest L with step M / L, while the table stays small. Compared to within 1e-12
        // of M rather than exactly: a step within that of M / L is M / L for any recording (it
        // moves a sample after 1e12 of them). For a sample rate change, the constructor from the
        // two rates finds M and L without floating point.
        void findPhases (double step)
        {
            for (int l = 1; l <= maxPhases && (long long) l * taps <= maxTable; ++l)
            {
                const double m = std::round (step * l);
                if (m >= 1.0 && m <= 1e9 && std::abs (step * l - m) <= 1e-12 * m)
                {
                    usePhases ((long long) m, l);
                    return;
                }
            }
        }

        // Sums by the table of L phases (and, for a whole-number step, by lines): step M / L.
        void usePhases (long long m, int l)
        {
            phases = l;
            advance = m;
            // phase r reads sample ip - W + 1 + q with the kernel at (q - W + 1 - r / L) fc
            table.resize ((size_t) phases * (size_t) taps);
            for (int r = 0; r < phases; ++r)
                for (int q = 0; q < taps; ++q)
                    table[(size_t) r * (size_t) taps + (size_t) q] = k.at (((double) (q - W + 1) - (double) r / phases) * fc);
            if (phases == 1 && advance <= maxLines)
            {
                lines.assign ((size_t) advance, {});
                for (int q = 0; q < taps; ++q)
                {
                    const long long j = q - W + 1;
                    lineOf.push_back ((int) floorMod (j, advance));
                    lineAt.push_back (floorDiv (j, advance));
                }
            }
        }

    public:
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
            for (auto& b : buf) b.resize (b.size() + (size_t) W + 2, 0.0);
            return run (true);
        }

        const float* output (int channel) const { return out[(size_t) channel].data(); }
        int outputSize() const { return (int) out[0].size(); }
        int channels() const { return ch; }
        // The cut-off as a fraction of the input's Nyquist, and the input samples each side of a read
        double cutoff() const { return fc; }
        int halfWidth() const { return W; }

    private:
        static constexpr int maxPhases = 4096;         // L at most
        static constexpr long long maxTable = 1 << 18; // L times the taps at most
        static constexpr long long maxLines = 16;      // whole-number steps summed by lines, M at most
        static constexpr int tile = 16;                // outputs summed side by side

        static long long gcd (long long a, long long b) { while (b != 0) { const long long t = a % b; a = b; b = t; } return a; }
        static long long floorDiv (long long a, long long b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
        static long long floorMod (long long a, long long b) { return a - floorDiv (a, b) * b; }

        // The sum over n taps, in four running sums (taps 0, 4, 8 ... in the
        // first) added at the end.
        static double dot (const double* wt, const double* x, int n)
        {
            double a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
            int q = 0;
            for (; q + 4 <= n; q += 4)
            {
                a0 += wt[q] * x[q];
                a1 += wt[q + 1] * x[q + 1];
                a2 += wt[q + 2] * x[q + 2];
                a3 += wt[q + 3] * x[q + 3];
            }
            for (; q < n; ++q) a0 += wt[q] * x[q];
            return (a0 + a1) + (a2 + a3);
        }

        int run (bool final)
        {
            for (auto& o : out) o.clear();
            const long long end = base + (long long) buf[0].size(); // absolute index past the last buffered sample
            const long long limit = final ? inTotal : end - W - 1;
            if (! lines.empty()) runLines (end, limit);
            else if (phases > 0) runPhases (limit);
            else runAnyStep ((double) limit);
            // drop what no future output needs
            const long long keepFrom = std::max (base, ip - W - 2);
            if (keepFrom > base)
            {
                const auto d = (std::ptrdiff_t) (keepFrom - base);
                for (auto& b : buf) b.erase (b.begin(), b.begin() + std::min (d, (std::ptrdiff_t) b.size()));
                base = keepFrom;
            }
            return (int) out[0].size();
        }

        // Any step: each tap looked up at its distance from the position.
        void runAnyStep (double limit)
        {
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
                    const double* b = buf[(size_t) c].data() + off;
                    double s = 0.0;
                    for (int q = 0; q < n; ++q) s += w[(size_t) q] * b[q];
                    out[(size_t) c].push_back ((float) (s * fc));
                }
                pos += stepAt (p);
            }
            ip = (long long) std::floor (pos);
        }

        // Step M / L: position ip + phase / L, that phase's taps from the table.
        void runPhases (long long limit)
        {
            while (ip < limit)
            {
                const double* wt = table.data() + (size_t) phase * (size_t) taps;
                const size_t off = (size_t) (ip - W + 1 - base);
                for (int c = 0; c < ch; ++c)
                    out[(size_t) c].push_back ((float) (dot (wt, buf[(size_t) c].data() + off, taps) * fc));
                phase += advance;
                ip += phase / phases;
                phase %= phases;
            }
        }

        // Whole-number step M: output o is at sample M o and its tap q reads
        // sample M o - W + 1 + q, which is in line lineOf[q] at o + lineAt[q].
        // Each line holds the buffered samples M i + m from i = first.
        void runLines (long long end, long long limit)
        {
            const long long M = advance;
            const long long first = floorDiv (base, M);
            const long long count = floorDiv (end - 1, M) - first + 1 + tile;
            const long long oFirst = ip / M, oEnd = (limit + M - 1) / M; // outputs with M o < limit
            if (oEnd <= oFirst) return;
            std::array<double, tile> acc {};
            for (int c = 0; c < ch; ++c)
            {
                const auto& b = buf[(size_t) c];
                for (long long m = 0; m < M; ++m)
                {
                    auto& ln = lines[(size_t) m];
                    ln.assign ((size_t) count, 0.0);
                    for (long long i = 0; i < count; ++i)
                    {
                        const long long a = M * (first + i) + m;
                        if (a >= base && a < end) ln[(size_t) i] = b[(size_t) (a - base)];
                    }
                }
                auto& o = out[(size_t) c];
                for (long long t0 = oFirst; t0 < oEnd; t0 += tile)
                {
                    acc.fill (0.0);
                    for (int q = 0; q < taps; ++q)
                    {
                        const double wq = table[(size_t) q];
                        const double* x = lines[(size_t) lineOf[(size_t) q]].data() + (t0 + lineAt[(size_t) q] - first);
                        for (int t = 0; t < tile; ++t) acc[(size_t) t] += wq * x[t];
                    }
                    const int made = (int) std::min<long long> (tile, oEnd - t0);
                    for (int t = 0; t < made; ++t) o.push_back ((float) (acc[(size_t) t] * fc));
                }
            }
            ip = oEnd * M;
        }

        StepFunction stepAt;
        double fc;
        const SincTable& k;
        int W, taps, ch;
        long long base;          // absolute index of buf[0] (zeros before the start)
        long long inTotal = 0;
        double pos = 0.0;        // any step: the position
        long long ip = 0;        // the whole sample of the position (of the next output)
        int phases = 0;          // L for a step M / L (0: any step)
        long long advance = 0;   // M
        int phase = 0;           // the position's phase, in L parts
        std::vector<std::vector<double>> buf, lines;
        std::vector<std::vector<float>> out;
        std::vector<double> w, table;
        std::vector<int> lineOf;
        std::vector<long long> lineAt;
    };

    // The same windowed sinc read live, for playback that converts as it
    // plays (a clip recorded at another rate, or played faster or slower):
    // each call gives the step (input samples per output sample, which may
    // change from call to call), the input for the block and how many outputs
    // to make. It keeps the last taps of input between calls, so the output
    // is latency() input samples behind; prepare() and reset() don't run on
    // the audio thread, process() never allocates. The kernel is the shared
    // sinc's shape with fewer crossings (liveSincTable) so many clips can
    // play at once, its cut-off lowered for a step above 1 as the streaming
    // resampler's is.
    inline const SincTable& liveSincTable()
    {
        static const SincTable table (16, 8.0, 4096);
        return table;
    }

    class LiveResampler
    {
    public:
        // maxStep: the largest step it will be given
        void prepare (double maxStep = 1.0)
        {
            fc = 0.97 * std::min (1.0, 1.0 / std::max (1.0e-6, maxStep));
            W = (int) std::ceil (k.halfWidth() / fc);
            buf.assign ((size_t) (4 * W), 0.0f);
            reset();
        }

        void reset()
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            w = 2 * W;
            pos = 1.0;
        }

        // How far behind the input the output runs, in input samples
        int latency() const { return W; }

        // Makes nOut samples into out (added times gain when adding, else
        // written times gain), reading up to inAvailable samples of in; returns
        // how many it read.
        int process (double step, const float* in, int inAvailable, float* out, int nOut, float gain = 1.0f, bool adding = false)
        {
            int used = 0;
            const int taps = 2 * W;
            for (int i = 0; i < nOut; ++i)
            {
                while (pos >= 1.0)
                {
                    push (used < inAvailable ? in[used] : (used > 0 ? in[inAvailable - 1] : 0.0f));
                    if (used < inAvailable) ++used;
                    pos -= 1.0;
                }
                const float* x = buf.data() + (w - taps);
                double sum = 0.0;
                for (int q = 0; q < taps; ++q) sum += k.at (((double) (q - W + 1) - pos) * fc) * x[q];
                const float y = (float) (sum * fc) * gain;
                out[i] = adding ? out[i] + y : y;
                pos += step;
            }
            return used;
        }

    private:
        void push (float v)
        {
            if (w == (int) buf.size())
            {
                std::copy (buf.end() - 2 * W, buf.end(), buf.begin());
                w = 2 * W;
            }
            buf[(size_t) w++] = v;
        }

        const SincTable& k = liveSincTable();
        double fc = 0.97, pos = 1.0;
        int W = 0, w = 0;
        std::vector<float> buf;
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
        Resampler r (fsIn, fsOut, numCh);
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
