#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>
#include "Biquad.h"
#include "Fft.h"
#include "Windows.h"
#include "Util.h"

namespace vs
{
    // Drawing axes for a spectrum, an EQ curve or a frequency knob: a log
    // frequency axis from loHz to hiHz (0 .. 1 across, 20 Hz to 20 kHz unless
    // given) and its inverse, and a dB axis from -rangeDb to +rangeDb (-1 .. 1).
    namespace axis
    {
        inline float logPosition (float hz, float loHz = 20.0f, float hiHz = 20000.0f) { return std::log (hz / loHz) / std::log (hiHz / loHz); }
        inline float logHz (float position, float loHz = 20.0f, float hiHz = 20000.0f) { return loHz * std::pow (hiHz / loHz, position); }
        inline float dbPosition (float db, float rangeDb) { return db / rangeDb; }
        inline float dbAt (float position, float rangeDb) { return position * rangeDb; }
    }

    // Spectrum analysis: the live analyser the products draw behind an EQ
    // (SpectrumAnalyser); the long-term spectrum (LongTermSpectrum: the one
    // averaged power spectrum, under welch(), CliffFinder and tonalBalance());
    // the "cliff" a lossy encoder leaves at the top of a song (CliffFinder);
    // and a mix's tonal balance against a finished mix's (tonalBalance).

    // A live analyser. The newest `size` samples of what it is fed, windowed
    // (Hann), through the FFT, as each bin's magnitude divided by `scale`. On
    // top of that, as wanted:
    //   levelsDb()  the magnitude in dB with meter ballistics (rises by
    //               `attack`, falls by `release` of the difference per update)
    //   power()     the magnitude squared, averaged by a one-pole of
    //               `powerSmoothing` per update (steady peaks stand out,
    //               transients don't)
    //   smoothing   averages the magnitudes themselves over time (each update
    //               keeps `smoothing` of the last), before the two above
    //   decibels()  20 log10 of the magnitudes as they are (no ballistics)
    // levelAt() reads the levels between bins (linear interpolation) for
    // drawing on a frequency axis, with an optional tilt in dB per octave.
    //
    // webAudio() gives the options of the browser's AnalyserNode
    // (getFloatFrequencyData in the Web Audio specification): a Blackman
    // window (alpha 0.16), magnitudes divided by the size, smoothed over time
    // by its smoothingTimeConstant, read as decibels(). The AnalyserNode has
    // size / 2 bins; the one more here is the Nyquist bin.
    class SpectrumAnalyser
    {
    public:
        enum class Window { hannSymmetric, hannPeriodic, blackman };

        struct Options
        {
            int size = 4096;                   // FFT size, a power of two
            int history = 8192;                // samples kept, a power of two, at least size
            Window window = Window::hannSymmetric;
            float scale = 1024.0f;             // magnitudes are divided by this (size / 4 is a full-scale sine's peak bin at 0 dB)
            float attack = 0.5f, release = 0.15f;
            float powerSmoothing = 0.04f;
            float smoothing = 0.0f;            // 0: none
            float floorDb = -120.0f;           // where levelsDb starts
            double blackmanAlpha = 0.16;
        };

        static Options webAudio (int fftSize = 2048, float smoothingTimeConstant = 0.8f)
        {
            Options o;
            o.size = fftSize;
            o.history = fftSize;
            o.window = Window::blackman;
            o.scale = (float) fftSize;
            o.smoothing = smoothingTimeConstant;
            return o;
        }

        void prepare (const Options& o)
        {
            opt = o;
            opt.history = std::max (opt.history, opt.size);
            hist.assign ((size_t) opt.history, 0.0f);
            write = 0;
            const auto w = opt.window == Window::hannSymmetric ? windows::hannSymmetric (opt.size)
                         : opt.window == Window::blackman      ? windows::blackman (opt.size, opt.blackmanAlpha)
                                                               : windows::hannPeriodic (opt.size);
            win.assign (w.begin(), w.end());
            fft.setSize (opt.size);
            frame.assign ((size_t) opt.size, 0.0);
            const size_t bins = (size_t) opt.size / 2 + 1;
            re.assign (bins, 0.0);
            im.assign (bins, 0.0);
            mag.assign (bins, 0.0f);
            db.assign (bins, opt.floorDb);
            pow.assign (bins, 0.0f);
            dbNow.assign (bins, 0.0f);
        }

        // Clears what was heard (the history, the levels, the averages).
        void reset()
        {
            std::fill (hist.begin(), hist.end(), 0.0f);
            write = 0;
            std::fill (mag.begin(), mag.end(), 0.0f);
            std::fill (db.begin(), db.end(), opt.floorDb);
            std::fill (pow.begin(), pow.end(), 0.0f);
        }

        void push (const float* x, int n)
        {
            const int mask = opt.history - 1;
            for (int i = 0; i < n; ++i)
            {
                hist[(size_t) write] = x[i];
                write = (write + 1) & mask;
            }
        }

        // Analyses the newest `size` samples: magnitudes, then the levels and
        // the power average.
        void update()
        {
            analyse();
            for (size_t b = 0; b < mag.size(); ++b)
            {
                const float d = ampToDb (mag[b]);
                followEnvelope (db[b], d, opt.attack, opt.release);
                pow[b] += opt.powerSmoothing * (mag[b] * mag[b] - pow[b]);
            }
        }

        // Only the magnitudes (no levels, no average).
        void analyse()
        {
            const int n = opt.size, mask = opt.history - 1;
            for (int i = 0; i < n; ++i)
                frame[(size_t) i] = (double) (hist[(size_t) ((write - n + i) & mask)] * win[(size_t) i]);
            fft.forward (frame.data(), re.data(), im.data());
            for (size_t b = 0; b < mag.size(); ++b)
            {
                const float m = (float) std::sqrt (re[b] * re[b] + im[b] * im[b]) / opt.scale;
                const float v = opt.smoothing > 0.0f ? opt.smoothing * mag[b] + (1.0f - opt.smoothing) * m : m;
                mag[b] = std::isfinite (v) ? v : 0.0f;
            }
        }

        // 20 log10 of each magnitude as it is (-inf for 0), after analyse() or update()
        const std::vector<float>& decibels()
        {
            for (size_t b = 0; b < mag.size(); ++b) dbNow[b] = ampToDb (mag[b], 0.0f);
            return dbNow;
        }

        int size() const { return opt.size; }
        int bins() const { return (int) mag.size(); }
        const std::vector<float>& magnitude() const { return mag; }
        const std::vector<float>& levelsDb() const { return db; }
        const std::vector<float>& power() const { return pow; }

        // The level at hz (between bins, linearly), plus tiltDbPerOctave for
        // each octave above tiltCentreHz.
        float levelAt (double hz, double sampleRate, float tiltDbPerOctave = 0.0f, float tiltCentreHz = 1000.0f) const
        {
            const float f = (float) hz;
            const float bin = f / (float) sampleRate * (float) opt.size;
            const int b0 = std::max (1, std::min ((int) db.size() - 2, (int) bin));
            const float frac = bin - (float) b0;
            float v = lerp (db[(size_t) b0], db[(size_t) b0 + 1], frac);
            if (tiltDbPerOctave != 0.0f) v += tiltDbPerOctave * std::log2 (f / tiltCentreHz);
            return v;
        }

    private:
        Options opt;
        std::vector<float> hist, win, mag, db, pow, dbNow;
        int write = 0;
        RealFft fft;
        std::vector<double> frame, re, im;
    };

    // A long-term power spectrum: frames of n samples (n a power of two),
    // each times a periodic Hann window, through the FFT; each frame's power
    // |X(k)|^2 added up per bin k = 0..n/2. sum() is the total (average over
    // whatever the caller counts: frames, or segments of several channels),
    // or with setRunningAverage, the running average.
    class LongTermSpectrum
    {
    public:
        // Allocates. floatFrames: each sample is windowed in float (a live
        // analyser's arithmetic), not double.
        void prepare (int frameSize, bool floatFrames = false)
        {
            n = frameSize;
            win = windows::hannPeriodic (n);
            floatWin = floatFrames ? windows::hannPeriodicFloat (n) : std::vector<float> {};
            pow.assign ((size_t) n / 2 + 1, 0.0);
            fft.setSize (n);
            frame.assign ((size_t) n, 0.0);
            re.assign ((size_t) n / 2 + 1, 0.0);
            im.assign ((size_t) n / 2 + 1, 0.0);
            count = 0;
        }

        // Instead of a sum, a running average: the plain mean of the frames
        // until firstFrames, then each new frame weighted by `memory` (the
        // older ones fading). Call after prepare().
        void setRunningAverage (int firstFrames, double memory) { avgFirst = firstFrames; avgMemory = memory; }

        // Back to no frames (the sum or the average at zero)
        void clear() { std::fill (pow.begin(), pow.end(), 0.0); count = 0; }

        // A frame from a ring buffer of n samples, the oldest at `start`
        void addRing (const float* ring, int start)
        {
            for (int i = 0; i < n; ++i)
            {
                const float x = ring[(size_t) ((start + i) % n)];
                frame[(size_t) i] = floatWin.empty() ? (double) x * win[(size_t) i] : (double) (x * floatWin[(size_t) i]);
            }
            transformAndAdd();
        }

        // n samples, windowed here
        template <typename T>
        void addFrame (const T* x)
        {
            for (int i = 0; i < n; ++i) frame[(size_t) i] = (double) x[i] * win[(size_t) i];
            transformAndAdd();
        }

        // A stretch shorter than a frame: the window stretched over its
        // `count` samples (sample i takes the window at i n / count), the
        // rest of the frame zero.
        template <typename T>
        void addShort (const T* x, int length)
        {
            std::fill (frame.begin(), frame.end(), 0.0);
            for (int i = 0; i < length; ++i) frame[(size_t) i] = (double) x[i] * win[(size_t) (((std::int64_t) i * n) / length)];
            transformAndAdd();
        }

        int size() const { return n; }
        int frames() const { return count; }
        const std::vector<double>& sum() const { return pow; }

        // The sum divided by `by` (at least 1)
        std::vector<double> averaged (double by) const
        {
            std::vector<double> p (pow);
            for (auto& v : p) v /= std::max (1.0, by);
            return p;
        }

        // The power of a periodic Hann window (sum of w^2: 3 n / 8), for
        // turning bin powers into a mean square.
        double windowPower() const { return 3.0 * n / 8.0; }

    private:
        void transformAndAdd()
        {
            fft.forward (frame.data(), re.data(), im.data());
            ++count;
            const double k = avgFirst <= 0 ? 0.0 : count < avgFirst ? 1.0 / count : avgMemory;
            for (int b = 0; b <= n / 2; ++b)
            {
                const double p = re[(size_t) b] * re[(size_t) b] + im[(size_t) b] * im[(size_t) b];
                if (avgFirst <= 0) pow[(size_t) b] += p;
                else pow[(size_t) b] += k * (p - pow[(size_t) b]);
            }
        }

        int n = 0, count = 0, avgFirst = 0;
        double avgMemory = 0.0;
        RealFft fft;
        std::vector<double> win, pow, frame, re, im;
        std::vector<float> floatWin;
    };

    // An averaged power spectrum and its bin spacing
    struct AveragedSpectrum
    {
        std::vector<double> power;   // per bin, 0..n/2
        double binHz = 0.0;
        int size = 0;                // the frame size n
    };

    // Welch's averaged spectrum of one or more channels of `total` samples:
    // frames of n overlapping by half, up to maxSegments of them spread
    // evenly over the whole (segment s starts at the hop nearest (s + 0.5)
    // of its share), every channel's frame added, the sum divided by the
    // number of segments. Shorter than one frame: one frame with the window
    // stretched over what there is.
    inline AveragedSpectrum welch (const float* const* chans, int numCh, std::int64_t total, double fs, int n, int maxSegments = 64)
    {
        const std::int64_t hop = n >> 1;
        const std::int64_t segs = std::max<std::int64_t> (1, (total - n) / hop + 1);
        const std::int64_t pick = std::min<std::int64_t> (segs, maxSegments);
        LongTermSpectrum lts;
        lts.prepare (n);
        int used = 0;
        for (std::int64_t s = 0; s < pick; ++s)
        {
            const std::int64_t at = (std::int64_t) std::floor ((s + 0.5) * ((double) segs / (double) pick)) * hop;
            if (at + n > total) continue;
            for (int c = 0; c < numCh; ++c) lts.addFrame (chans[c] + at);
            ++used;
        }
        if (total < n)
        {
            for (int c = 0; c < numCh; ++c) lts.addShort (chans[c], (int) total);
            used = 1;
        }
        return { lts.averaged (used), fs / n, n };
    }

    // The same over frames that start at the given positions (every channel's
    // frame added, divided by the number of starts).
    inline AveragedSpectrum welchAt (const float* const* chans, int numCh, double fs, int n, const std::vector<std::int64_t>& starts)
    {
        LongTermSpectrum lts;
        lts.prepare (n);
        for (auto at : starts)
            for (int c = 0; c < numCh; ++c) lts.addFrame (chans[c] + at);
        return { lts.averaged ((double) starts.size()), fs / n, n };
    }

    // A mix's tonal balance: its level in ten octave bands (40 Hz to 12 kHz)
    // against a finished mix's (roughly equal energy per octave from 100 Hz
    // to 2 kHz, 1.05 dB per octave more below 100 Hz, 2.5 dB per octave
    // less above 2 kHz), relative to the 125 Hz to 4 kHz average, and the four
    // figures a mix check reports from it: lows (40, 63 Hz), mud (250, 500
    // Hz), harshness (2, 4 kHz) and top (8, 12 kHz), in dB above a finished
    // mix. Each band's level is the mean square the band would have after
    // two cascaded RBJ band-passes (Q 1.41) at its centre, read from the
    // long-term spectrum of the mid signal (Welch, frames of about 0.35 s).
    struct TonalBalance
    {
        static constexpr int numBands = 10;
        static constexpr double bandHz[numBands] { 40, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 12000 };
        double levelDb[numBands] {}, relDb[numBands] {};
        double lows = 0, mud = 0, harsh = 0, top = 0;
    };

    // The reference curve: a finished mix's level in a band at hz (dB)
    inline double finishedMixDb (double hz)
    {
        return (hz < 100 ? 1.05 * std::log2 (100 / hz) : 0) - (hz > 2000 ? 2.5 * std::log2 (hz / 2000) : 0);
    }

    inline TonalBalance tonalBalance (const float* mid, std::int64_t n, double fs)
    {
        TonalBalance t;
        const int size = 1 << (int) std::lround (std::log2 (fs * 0.35));
        const float* ch[1] { mid };
        const auto sp = welch (ch, 1, n, fs, size, std::numeric_limits<int>::max());
        const double norm = (double) size * (3.0 * size / 8.0);
        double diff[TonalBalance::numBands];
        for (int b = 0; b < TonalBalance::numBands; ++b)
        {
            const double f = TonalBalance::bandHz[b];
            const std::vector<BiquadCoefs> bp { BiquadCoefs::rbj (BiquadCoefs::Type::bandPass, fs, f, 1.41), BiquadCoefs::rbj (BiquadCoefs::Type::bandPass, fs, f, 1.41) };
            double e = 0.0;
            for (int k = 0; k <= size / 2; ++k)
            {
                const double h = sectionsResponse (bp, k * sp.binHz, fs);
                e += (k == 0 || k == size / 2 ? 1.0 : 2.0) * h * h * sp.power[(size_t) k];
            }
            t.levelDb[b] = db10 (e / norm);
            diff[b] = t.levelDb[b] - finishedMixDb (f);
        }
        double normDb = 0.0;
        for (int b = 2; b <= 7; ++b) normDb += diff[b];
        normDb /= 6;
        for (int b = 0; b < TonalBalance::numBands; ++b) t.relDb[b] = diff[b] - normDb;
        t.lows = (t.relDb[0] + t.relDb[1]) / 2;
        t.mud = (t.relDb[3] + t.relDb[4]) / 2;
        t.harsh = (t.relDb[6] + t.relDb[7]) / 2;
        t.top = (t.relDb[8] + t.relDb[9]) / 2;
        return t;
    }

    // Where a song's highs stop like a wall, and how far they drop there (dB).
    struct SpectrumCliff
    {
        double cliffHz = 0.0;   // 0: no cliff
        double drop = 0.0;
    };

    // The average spectrum of a song's middle (L + R) / 2, from up to 160
    // Hann-windowed 8192-point frames spread over the song (frames quieter
    // than -50 dB left out), in 250 Hz bands, and the sharpest cliff above
    // 10 kHz: MP3 and AAC encoders cut everything above a fixed frequency
    // (about 16 kHz at 128 kbps, 19 to 20 kHz at 320), which no natural mix
    // does that steeply. The song's length must be known first; feed it block
    // by block.
    class CliffFinder
    {
    public:
        static constexpr int N = 8192;

        void prepare (double sampleRate, std::int64_t totalFrames)
        {
            fs = sampleRate;
            frames = (int) std::min<std::int64_t> (160, totalFrames / N);
            if (frames < 4) frames = 0; // too short to judge
            step = frames > 0 ? (totalFrames - N) / (frames - 1) : 0;
            spectrum.prepare (N);
            frame.assign (N, 0.0);
            next = 0;
            at = 0;
        }

        void process (const float* l, const float* r, int n)
        {
            // frames don't overlap: copy the part of the next frame in this block
            std::int64_t i = 0;
            while (next < frames)
            {
                const std::int64_t s = (std::int64_t) next * step;
                i = std::max (i, s - at);
                if (i >= n) break;
                const std::int64_t end = std::min<std::int64_t> (n, s + N - at);
                for (; i < end; ++i) frame[(size_t) (at + i - s)] = midOf ((double) l[i], (double) r[i]);
                if (at + end - s == N) { analyseFrame(); ++next; }
                else break;
            }
            at += n;
        }

        // False when the song is too short (or too quiet) to judge.
        bool result (SpectrumCliff& out) const
        {
            out = {};
            if (frames < 4 || spectrum.frames() < 4) return false;
            const auto& pow = spectrum.sum();
            const double bw = 250.0;
            const int nb = (int) std::floor (fs / 2 / bw);
            std::vector<double> lev ((size_t) nb), lin ((size_t) nb);
            for (int b = 0; b < nb; ++b)
            {
                const int k0 = (int) std::floor ((b * bw * N) / fs);
                const int k1 = std::max (k0 + 1, (int) std::floor (((b + 1) * bw * N) / fs));
                double s = 0.0;
                for (int k = k0; k < k1 && k < N / 2; ++k) s += pow[(size_t) k];
                lin[(size_t) b] = s / (k1 - k0);
                lev[(size_t) b] = db10 (lin[(size_t) b]);
            }
            auto meanDb = [&] (int a, int b)
            {
                double s = 0.0;
                int c = 0;
                for (int k = a; k <= b; ++k) { s += lin[(size_t) k]; ++c; }
                return db10 (s / std::max (1, c));
            };
            const double ref = meanDb (4, 15); // 1-4 kHz
            const double scanTo = fs > 50000 ? 24500 : 19000; // 44.1 and 48 kHz files have a natural resampling edge around 20 kHz
            bool found = false;
            int bestB = 0;
            double bestDrop = 0.0, bestBelow = 0.0;
            for (int b = 40; b * bw <= scanTo; ++b) // from 10 kHz up
            {
                if (b + 7 > nb - 1) break;
                const double below = meanDb (b - 6, b - 1);
                double above = -std::numeric_limits<double>::infinity();
                for (int k = b + 3; k <= std::min (nb - 2, b + 14); ++k) above = std::max (above, lev[(size_t) k]);
                const double drop = below - above;
                if (drop > 24 && below > ref - 50 && (! found || drop > bestDrop)) { found = true; bestB = b; bestDrop = drop; bestBelow = below; }
            }
            if (! found) return true;
            // the edge itself: the first band well under the music just below it
            int edge = bestB;
            for (int k = bestB - 6; k <= bestB + 2; ++k)
            {
                bool gone = true;
                for (int j = k; j <= bestB + 2; ++j) if (lev[(size_t) j] >= bestBelow - 20) gone = false;
                if (gone) { edge = k; break; }
            }
            out.cliffHz = edge * bw;
            out.drop = bestDrop;
            return true;
        }

    private:
        void analyseFrame()
        {
            double e = 0.0;
            for (int i = 0; i < N; ++i) e += frame[(size_t) i] * frame[(size_t) i];
            if (e / N < 1e-5) return; // quieter than -50 dB: an intro, a gap
            spectrum.addFrame (frame.data());
        }

        double fs = 48000.0;
        int frames = 0, next = 0;
        std::int64_t step = 0, at = 0;
        LongTermSpectrum spectrum;
        std::vector<double> frame;
    };
}
