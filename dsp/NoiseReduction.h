#pragma once

// Spectral noise reduction for transfers of records and tapes (Restore's),
// and the noise profile it works from.
//
// SpectralDenoiser: short-time transforms with 75% overlap, both channels
// through one complex transform so the stereo picture never wanders. Each
// frequency is turned down by how much of it is noise, judged against the
// learned profile. To avoid "birdies" (the twittering, underwater sound of
// cheap noise reduction) each frequency's decision is smoothed over time (a
// decision-directed estimate) and across its neighbours, and nothing is turned
// down further than the amount allows: some hiss is always left.
//
// NoiseProfileLearner: the quietest stretches of a transfer (the lead-in
// groove, the gaps between songs, tape leader) are mostly noise. It takes
// frames of them, rejects any with a click or a clear musical note, and keeps
// per frequency the mean of the quieter 80% (the trim's bias undone). Mains
// hum lines aren't noise (de-hum takes them out first), so they are bridged
// with the noise either side of them.

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>
#include "Fft.h"
#include "Windows.h"

namespace vs
{
    namespace denoise
    {
        // The frame size for a sample rate: about 43 ms.
        inline int frameSize (double fs) { return fs <= 50000 ? 2048 : fs <= 100000 ? 4096 : 8192; }

        // An amount (0..100) to how far down noise is turned at most, in dB:
        // 0 gives 3 dB, 40 gives 10.2 dB, 100 gives 21 dB.
        inline double amountToDb (double amount) { return 3.0 + 0.18 * std::max (0.0, std::min (100.0, amount)); }

        // The level a profile (noise power per bin, both channels together,
        // through a Hann window of n) describes: dBFS RMS per channel.
        inline double profileDb (const std::vector<float>& prof, int n)
        {
            double t = 0.0;
            const size_t last = prof.size() - 1;
            for (size_t k = 0; k < prof.size(); ++k) t += (k == 0 || k == last ? 1.0 : 2.0) * prof[k];
            return 10.0 * std::log10 (t / ((double) n * (3.0 * n / 8.0)) / 2.0 + 1e-30);
        }
    }

    class SpectralDenoiser
    {
    public:
        // profile: noise power per bin (n/2 + 1 values, left power + right
        // power, through a Hann window), which also sets the frame size; empty
        // runs nothing. keep: run the frames but change nothing (the same delay,
        // for comparing with and without).
        SpectralDenoiser (double fs, double amount, const std::vector<float>& profile, bool keep = false)
        {
            N = ! profile.empty() ? ((int) profile.size() - 1) * 2 : denoise::frameSize (fs);
            H = N >> 2;
            win = windows::hannPeriodic (N);
            fft = &fftFor (N);
            noise.assign ((size_t) N / 2 + 1, 0.0);
            for (size_t k = 0; k < profile.size() && k < noise.size(); ++k) noise[k] = profile[k];
            floor = std::pow (10.0, -denoise::amountToDb (amount) / 20.0);
            over = 1.0 + 0.01 * std::max (0.0, std::min (100.0, amount));
            bypass = profile.empty() || keep;
            prevA.assign ((size_t) N / 2 + 1, 0.0);
            prevG.assign ((size_t) N / 2 + 1, 1.0);
            inL.assign ((size_t) N, 0.0); inR.assign ((size_t) N, 0.0);
            outL.assign ((size_t) N, 0.0); outR.assign ((size_t) N, 0.0);
            hopL.assign ((size_t) H, 0.0); hopR.assign ((size_t) H, 0.0);
            skip = N - H;
            re.assign ((size_t) N, 0.0); im.assign ((size_t) N, 0.0);
            G.assign ((size_t) N / 2 + 1, 0.0);
            S.assign ((size_t) N / 2 + 1, 0.0);
            release = std::pow (10.0, -15.0 * (H / fs / 0.1) / 20.0);   // gains fall back at most 15 dB per 100 ms
        }

        int frameSize() const { return N; }

        // The next n samples of each channel (r may be null: l for both).
        // Returns how many samples per channel are ready (output (c)); the first
        // frame size less a hop of input is the transform's own delay.
        int push (const float* l, const float* r, int n)
        {
            inTotal += n;
            for (auto& o : out) o.clear();
            run (l, r != nullptr ? r : l, n);
            outTotal += (long) out[0].size();
            return (int) out[0].size();
        }

        // The rest, after the last push.
        int flush()
        {
            for (auto& o : out) o.clear();
            const std::vector<float> pad ((size_t) (N + H), 0.0f);
            run (pad.data(), pad.data(), (int) pad.size());
            const long n = std::min<long> ((long) out[0].size(), inTotal - outTotal);
            for (auto& o : out) o.resize ((size_t) std::max<long> (0, n));
            outTotal += (long) out[0].size();
            return (int) out[0].size();
        }

        const std::vector<float>& output (int c) const { return out[c]; }

    private:
        int N, H, skip;
        const Fft* fft;
        std::vector<double> win, noise, prevA, prevG, inL, inR, outL, outR, hopL, hopR, re, im, G, S;
        double floor, over, release;
        bool bypass;
        int fill = 0;
        long inTotal = 0, outTotal = 0;
        std::vector<float> out[2];

        void frame()
        {
            const int half = N >> 1;
            // the newest hop into the analysis buffer
            std::copy (inL.begin() + H, inL.end(), inL.begin());
            std::copy (inR.begin() + H, inR.end(), inR.begin());
            std::copy (hopL.begin(), hopL.end(), inL.begin() + (N - H));
            std::copy (hopR.begin(), hopR.end(), inR.begin() + (N - H));
            for (int i = 0; i < N; ++i) { re[(size_t) i] = inL[(size_t) i] * win[(size_t) i]; im[(size_t) i] = inR[(size_t) i] * win[(size_t) i]; }
            fft->forward (re.data(), im.data());
            if (! bypass)
            {
                const double alpha = 0.96;
                auto power = [&] (int k)
                {
                    const int k2 = (N - k) % N;
                    return 0.5 * (re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k] + re[(size_t) k2] * re[(size_t) k2] + im[(size_t) k2] * im[(size_t) k2]);
                };
                for (int k = 0; k <= half; ++k)
                {
                    // left + right power at this frequency (one transform carries both channels)
                    const double P = power (k);
                    const double Nk = over * noise[(size_t) k] + 1e-30;
                    const double gamma = P / Nk;
                    const double xi = std::max (alpha * prevA[(size_t) k] + (1 - alpha) * std::max (gamma - 1, 0.0), 1e-4);
                    G[(size_t) k] = xi / (1 + xi);
                }
                // smooth across neighbouring frequencies (wider up high), keep the floor
                for (int k = 0; k <= half; ++k)
                {
                    const int w = k < 32 ? 1 : k < 256 ? 2 : 3;
                    double s = 0.0, c = 0.0;
                    for (int j = std::max (0, k - w); j <= std::min (half, k + w); ++j)
                    {
                        const double wt = w + 1 - std::abs (j - k);
                        s += G[(size_t) j] * wt;
                        c += wt;
                    }
                    // never pull a strong, clearly musical bin down because its neighbours are noise
                    S[(size_t) k] = std::max (floor, std::max (s / c, G[(size_t) k] > 0.9 ? G[(size_t) k] : 0.0));
                }
                for (int k = 0; k <= half; ++k)
                {
                    // a gentle release: a gain falls back no faster than 15 dB per 100 ms (no pumping)
                    const double g = std::max (S[(size_t) k], prevG[(size_t) k] * release);
                    const int k2 = (N - k) % N;
                    const double P = power (k);
                    prevA[(size_t) k] = g * g * P / (over * noise[(size_t) k] + 1e-30);
                    prevG[(size_t) k] = g;
                    re[(size_t) k] *= g; im[(size_t) k] *= g;
                    if (k2 != k) { re[(size_t) k2] *= g; im[(size_t) k2] *= g; }
                }
            }
            fft->inverse (re.data(), im.data());
            const double norm = 1.0 / 1.5;   // Hann x Hann at 75% overlap sums to 1.5
            std::copy (outL.begin() + H, outL.end(), outL.begin());
            std::copy (outR.begin() + H, outR.end(), outR.begin());
            std::fill (outL.begin() + (N - H), outL.end(), 0.0);
            std::fill (outR.begin() + (N - H), outR.end(), 0.0);
            for (int i = 0; i < N; ++i)
            {
                outL[(size_t) i] += re[(size_t) i] * win[(size_t) i] * norm;
                outR[(size_t) i] += im[(size_t) i] * win[(size_t) i] * norm;
            }
        }

        // Each full hop runs a frame, whose oldest hop of output is then
        // finished (the first N - H outputs are pre-history and dropped).
        void run (const float* l, const float* r, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                hopL[(size_t) fill] = l[i];
                hopR[(size_t) fill] = r[i];
                if (++fill == H)
                {
                    fill = 0;
                    frame();
                    for (int j = 0; j < H; ++j)
                    {
                        if (skip > 0) { --skip; continue; }
                        out[0].push_back ((float) outL[(size_t) j]);
                        out[1].push_back ((float) outR[(size_t) j]);
                    }
                }
            }
        }
    };

    class NoiseProfileLearner
    {
    public:
        // n: the frame size (denoise::frameSize). humHz: the hum lines to set
        // aside (their frequencies), if there is hum.
        NoiseProfileLearner (double sampleRate, int n, std::vector<double> humHz = {})
            : fs (sampleRate), N (n), hum (std::move (humHz))
        {
            win = windows::hannPeriodic (N);
            fft = &fftFor (N);
            re.assign ((size_t) N, 0.0);
            im.assign ((size_t) N, 0.0);
            for (double hz : hum)
            {
                const int k = (int) std::round (hz * N / fs);
                for (int j = k - 3; j <= k + 3; ++j) humBins.push_back (j);
            }
        }

        // Has it heard enough frames (it takes at most 160)?
        bool full() const { return keep.size() >= 160; }

        // One frame of n samples per channel. Returns whether it was kept (no click, no note).
        bool add (const float* l, const float* r)
        {
            if (full()) return false;
            // a click in it? (a peak far above the stretch's own level, after a high-pass)
            double pk = 0.0, ss = 0.0;
            for (int i = 1; i < N; ++i) { const double d = (double) l[i] - (double) l[i - 1]; pk = std::max (pk, std::abs (d)); ss += d * d; }
            if (pk > 8 * std::sqrt (ss / N)) return false;
            for (int i = 0; i < N; ++i) { re[(size_t) i] = l[i] * win[(size_t) i]; im[(size_t) i] = r[i] * win[(size_t) i]; }
            fft->forward (re.data(), im.data());
            const int half = N / 2;
            std::vector<double> P ((size_t) half + 1);
            for (int k = 0; k <= half; ++k)
            {
                const int k2 = (N - k) % N;
                P[(size_t) k] = 0.5 * (re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k] + re[(size_t) k2] * re[(size_t) k2] + im[(size_t) k2] * im[(size_t) k2]);
            }
            // a musical note: a line far above its neighbourhood (hum lines excepted)
            for (int k = 8; k < half - 8; ++k)
            {
                if (std::find (humBins.begin(), humBins.end(), k) != humBins.end() || P[(size_t) k] < P[(size_t) (k - 1)] || P[(size_t) k] < P[(size_t) (k + 1)]) continue;
                const double ring = (P[(size_t) (k - 6)] + P[(size_t) (k + 6)] + P[(size_t) (k - 8)] + P[(size_t) (k + 8)]) / 4;
                if (P[(size_t) k] > 300 * ring && P[(size_t) k] > 1e-9) return false;
            }
            keep.push_back (std::move (P));
            return true;
        }

        int framesKept() const { return (int) keep.size(); }

        // The profile (n/2 + 1 values), or nothing when fewer than six frames were kept.
        std::optional<std::vector<float>> profile() const
        {
            if (keep.size() < 6) return std::nullopt;
            const int half = N / 2;
            std::vector<float> prof ((size_t) half + 1);
            std::vector<double> col (keep.size());
            const size_t m = std::max<size_t> (3, (size_t) std::floor ((double) keep.size() * 0.8));
            for (int k = 0; k <= half; ++k)
            {
                for (size_t j = 0; j < keep.size(); ++j) col[j] = keep[j][(size_t) k];
                std::sort (col.begin(), col.end());
                double sum = 0.0;
                for (size_t j = 0; j < m; ++j) sum += col[j];
                prof[(size_t) k] = (float) (sum / (double) m * 1.25);
            }
            // bridge each hum line with the noise either side of it, straight across in dB
            if (! hum.empty())
            {
                std::vector<char> mask ((size_t) half + 1, 0);
                for (double hz : hum)
                {
                    const int k = (int) std::round (hz * N / fs);
                    for (int j = std::max (1, k - 3); j <= std::min (half, k + 3); ++j) mask[(size_t) j] = 1;
                }
                for (int j = 1; j <= half;)
                {
                    if (! mask[(size_t) j]) { ++j; continue; }
                    int e = j;
                    while (e + 1 <= half && mask[(size_t) (e + 1)]) ++e;
                    // anchored on the clean neighbours (not the DC bin)
                    const double hi = e + 1 <= half ? prof[(size_t) (e + 1)] : prof[(size_t) (j - 1)];
                    const double lo = j - 1 > 0 ? prof[(size_t) (j - 1)] : hi;
                    const double a = std::log (lo + 1e-30), b = std::log (hi + 1e-30);
                    for (int q = j; q <= e; ++q)
                        prof[(size_t) q] = std::min (prof[(size_t) q], (float) std::exp (a + (b - a) * (q - j + 1) / (e - j + 2)));
                    j = e + 1;
                }
            }
            return prof;
        }

    private:
        double fs;
        int N;
        std::vector<double> hum, win, re, im;
        std::vector<int> humBins;
        const Fft* fft;
        std::vector<std::vector<double>> keep;
    };
}
