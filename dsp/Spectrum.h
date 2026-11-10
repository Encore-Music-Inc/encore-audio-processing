#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>
#include "Fft.h"
#include "Windows.h"
#include "Util.h"

namespace vs
{
    // Spectrum analysis: the live analyser the products draw behind an EQ
    // (SpectrumAnalyser), and the long-term spectrum of a whole song with the
    // "cliff" a lossy encoder leaves at the top (CliffFinder).

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
            re.assign ((size_t) opt.size, 0.0);
            im.assign ((size_t) opt.size, 0.0);
            const size_t bins = (size_t) opt.size / 2 + 1;
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
                db[b] += (d > db[b] ? opt.attack : opt.release) * (d - db[b]);
                pow[b] += opt.powerSmoothing * (mag[b] * mag[b] - pow[b]);
            }
        }

        // Only the magnitudes (no levels, no average).
        void analyse()
        {
            const int n = opt.size, mask = opt.history - 1;
            for (int i = 0; i < n; ++i)
            {
                re[(size_t) i] = (double) (hist[(size_t) ((write - n + i) & mask)] * win[(size_t) i]);
                im[(size_t) i] = 0.0;
            }
            fft.forward (re.data(), im.data());
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
            float v = db[(size_t) b0] + frac * (db[(size_t) b0 + 1] - db[(size_t) b0]);
            if (tiltDbPerOctave != 0.0f) v += tiltDbPerOctave * std::log2 (f / tiltCentreHz);
            return v;
        }

    private:
        Options opt;
        std::vector<float> hist, win, mag, db, pow, dbNow;
        int write = 0;
        Fft fft;
        std::vector<double> re, im;
    };

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
            win = windows::hannPeriodic (N);
            pow.assign (N / 2, 0.0);
            frame.assign (N, 0.0);
            next = 0;
            used = 0;
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
            if (frames < 4 || used < 4) return false;
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
            std::vector<double> re ((size_t) N), im ((size_t) N, 0.0);
            double e = 0.0;
            for (int i = 0; i < N; ++i)
            {
                const double v = frame[(size_t) i];
                re[(size_t) i] = v * win[(size_t) i];
                e += v * v;
            }
            if (e / N < 1e-5) return; // quieter than -50 dB: an intro, a gap
            fftFor (N).forward (re.data(), im.data());
            for (int k = 0; k < N / 2; ++k) pow[(size_t) k] += re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k];
            ++used;
        }

        double fs = 48000.0;
        int frames = 0, next = 0, used = 0;
        std::int64_t step = 0, at = 0;
        std::vector<double> win, pow, frame;
    };
}
