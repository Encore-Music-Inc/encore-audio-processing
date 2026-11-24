#pragma once

// What's wrong with a transfer of a record or tape (Restore's analysis): one
// streaming pass over the whole file (RestoreAnalyzer), then detectors on its
// low-rate copies (hum, tuning, wow, dropouts, the songs on a side), and
// measurements on short windows read back from the file (the quietest spots
// for the noise profile, how the channels compare, how clicky it is). Also the
// level envelope of a side and the quiet dips in it that could be gaps between
// songs. Numbers only; what they mean in words is the product's.
//
// The results are rounded as Restore reports them (round1 and so on), since
// settings are made from them.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "Biquad.h"
#include "Declicker.h"
#include "Fft.h"
#include "NoiseReduction.h"
#include "Resampler.h"
#include "Spectrum.h"
#include "Stats.h"
#include "Windows.h"

namespace vs
{
    namespace restore
    {
        inline constexpr double kPi = 3.14159265358979323846;

        // Power and amplitude in dB, with a floor so silence is finite.
        inline double dbPower (double p) { return 10.0 * std::log10 (p + 1e-30); }

        // x rounded to `places` decimals, the way the reports round (half up).
        inline double rounded (double x, int places)
        {
            const double m = std::pow (10.0, places);
            const double v = std::abs (x) * m;
            const double r = std::floor (v + 0.5) / m;
            return x < 0 ? -r : r;
        }

        // Math.round: halves go up (towards +infinity).
        inline double roundUp (double x) { return std::floor (x + 0.5); }

        // a / b rounded down (for counts of frames that may come out below zero)
        inline long floorDiv (long a, long b) { return (long) std::floor ((double) a / (double) b); }

        // ---- the streaming pass

        struct Overview { int cols = 0, per = 1; std::vector<float> peakL, peakR, rms; };

        // A waveform overview of a stretch of known length: per column, each
        // channel's peak and the RMS of both; and the loudest sample.
        class OverviewMeter
        {
        public:
            OverviewMeter (std::int64_t frames, int columns = 3000)
            {
                ov.cols = columns;
                ov.per = (int) std::max<std::int64_t> (1, (std::int64_t) std::ceil ((double) frames / columns));
                ov.peakL.assign ((size_t) columns, 0.0f);
                ov.peakR.assign ((size_t) columns, 0.0f);
                rmsSum.assign ((size_t) columns, 0.0);
                cnt.assign ((size_t) columns, 0);
            }

            void push (const float* l, const float* r, int n)
            {
                for (int i = 0; i < n; ++i) sample (l[i], r[i]);
            }

            void sample (float x, float y)
            {
                const int c = (int) std::min<std::int64_t> (ov.cols - 1, pos / ov.per);
                const float ax = std::abs (x), ay = std::abs (y);
                if (ax > ov.peakL[(size_t) c]) ov.peakL[(size_t) c] = ax;
                if (ay > ov.peakR[(size_t) c]) ov.peakR[(size_t) c] = ay;
                rmsSum[(size_t) c] += 0.5 * ((double) x * x + (double) y * y);
                ++cnt[(size_t) c];
                if (ax > mx) mx = ax;
                if (ay > mx) mx = ay;
                ++pos;
            }

            const Overview& result()
            {
                ov.rms.resize ((size_t) ov.cols);
                for (int c = 0; c < ov.cols; ++c) ov.rms[(size_t) c] = (float) std::sqrt (rmsSum[(size_t) c] / std::max (1, cnt[(size_t) c]));
                return ov;
            }

            float peak() const { return mx; }
            double peakDb() const { return 20.0 * std::log10 (mx + 1e-12); }

        private:
            Overview ov;
            std::vector<double> rmsSum;
            std::vector<int> cnt;
            std::int64_t pos = 0;
            float mx = 0;
        };
        struct Second { double ll = 0, rr = 0, lr = 0; };

        class RestoreAnalyzer
        {
        public:
            // frames: the file's length; cols: columns of the waveform overview.
            RestoreAnalyzer (double sampleRate, std::int64_t totalFrames, int columns = 3000)
                : fs (sampleRate), frames (totalFrames),
                  factor (std::max (1, (int) roundUp (sampleRate / 11025.0))),
                  dec (sampleRate, std::max (1, (int) roundUp (sampleRate / 11025.0))),
                  ov (totalFrames, columns)
            {
                fsd = fs / factor;
                N = denoise::frameSize (fs);
                hop = N >> 1;
                secLen = (int) roundUp (fs);
            }

            // The next n samples (r may be null for a mono file: l for both).
            void push (const float* l, const float* r, int n)
            {
                if (r == nullptr) r = l;
                const size_t at0 = decL.size();
                decL.resize (at0 + (size_t) dec.maxOutput (n));
                decR.resize (decL.size());
                const int got = dec.pushPair (l, r, n, decL.data() + at0, decR.data() + at0);
                decL.resize (at0 + (size_t) got);
                decR.resize (decL.size());
                for (int i = 0; i < n; ++i)
                {
                    const double x = l[i], y = r[i];
                    ov.sample (l[i], r[i]);
                    const float ax = std::abs (l[i]), ay = std::abs (r[i]);
                    if (ax > chPeak[0]) chPeak[0] = ax;
                    if (ay > chPeak[1]) chPeak[1] = ay;
                    sum[0] += x; sum[1] += y;
                    acc += x * x + y * y;
                    if (++accN == hop) { hopE.push_back ((float) (acc / (2.0 * hop))); acc = 0; accN = 0; }
                    secAcc.ll += x * x; secAcc.rr += y * y; secAcc.lr += x * y;
                    if (++secN == secLen) { seconds1.push_back (secAcc); secAcc = {}; secN = 0; }
                }
                pos += n;
            }

            // After the last push: the low-rate copies and their mid, the overview's RMS.
            void finish()
            {
                mid.resize (decL.size());
                for (size_t i = 0; i < decL.size(); ++i) mid[i] = (float) (0.5 * ((double) decL[i] + (double) decR[i]));
                overviewDone = ov.result();
            }

            double sampleRate() const { return fs; }
            double lowRate() const { return fsd; }                  // the copies' rate, about 11-12 kHz
            std::int64_t framesRead() const { return pos; }
            const std::vector<float>& lowLeft() const { return decL; }
            const std::vector<float>& lowRight() const { return decR; }
            const std::vector<float>& lowMid() const { return mid; }
            const Overview& overview() const { return overviewDone; }
            double peakDb() const { return ov.peakDb(); }
            float channelPeak (int c) const { return chPeak[c]; }
            double dc (int c) const { return sum[c] / (double) std::max<std::int64_t> (1, pos); }
            // energy per half a noise-reduction frame (mean square of both channels), for the quiet spots
            const std::vector<float>& hopEnergy() const { return hopE; }
            int hopSize() const { return hop; }
            int frameSize() const { return N; }
            const std::vector<Second>& perSecond() const { return seconds1; }

            // The music's level: the loud end (90%) of the per-hop energies, in dB;
            // nothing for a file with no sound.
            std::optional<double> musicDb() const
            {
                std::vector<float> he;
                for (float v : hopE) if (v > 1e-12f) he.push_back (v);
                if (he.empty()) return std::nullopt;
                std::sort (he.begin(), he.end());
                return 10.0 * std::log10 ((double) he[(size_t) std::floor ((double) he.size() * 0.9)]);
            }

        private:
            double fs, fsd = 0;
            std::int64_t frames, pos = 0;
            int factor;
            Decimator dec;
            std::vector<float> decL, decR, mid;
            OverviewMeter ov;
            Overview overviewDone;
            int N, hop, accN = 0, secLen, secN = 0;
            double acc = 0, sum[2] { 0, 0 };
            float chPeak[2] { 0, 0 };
            std::vector<float> hopE;
            Second secAcc;
            std::vector<Second> seconds1;
        };

        // ---- hum

        struct HumLine { int h; double prom, hz; };
        struct Hum { bool found = false; double hz = 0; std::vector<HumLine> harmonics; std::optional<double> levelDb; double drift = 0, score = 0; };

        namespace detail
        {
            inline int humFrame (double fsd) { return 1 << std::min (17, std::max (12, (int) roundUp (std::log2 (fsd * 5.5)))); }

            struct Prom { double prom, peakHz, level; };

            // how far a line at F stands above its neighbourhood (0.6 to 3 Hz away), in dB
            inline Prom prominence (const AveragedSpectrum& sp, const std::vector<double>& L, double F)
            {
                const double df = sp.binHz;
                const double k = F / df;
                const int k0 = (int) roundUp (k);
                if (k0 < 3 || k0 > (int) L.size() - 40) return { -99, F, -200 };
                int best = k0;
                for (int j = k0 - 1; j <= k0 + 1; ++j) if (L[(size_t) j] > L[(size_t) best]) best = j;
                const double a = L[(size_t) (best - 1)], b = L[(size_t) best], c = L[(size_t) (best + 1)], den = a - 2 * b + c;
                const double shift = std::abs (den) > 1e-9 ? std::max (-0.5, std::min (0.5, 0.5 * (a - c) / den)) : 0.0;
                const int lo = std::max (2, (int) roundUp (0.6 / df)), hi = std::max (lo + 3, (int) roundUp (3.0 / df));
                std::vector<double> ring;
                for (int j = lo; j <= hi; ++j)
                {
                    if (best - j > 0) ring.push_back (L[(size_t) (best - j)]);
                    if (best + j < (int) L.size()) ring.push_back (L[(size_t) (best + j)]);
                }
                // against the loud end of the neighbourhood: a lone needle is hum; a needle
                // with needles either side is a repeating pattern in the music
                return { b - stats::percentile (ring.data(), (int) ring.size(), 0.85), (best + shift) * df, b };
            }

            inline std::vector<double> levels (const AveragedSpectrum& sp)
            {
                std::vector<double> L (sp.power.size());
                for (size_t i = 0; i < L.size(); ++i) L[i] = dbPower (sp.power[i]);
                return L;
            }

            // the spectrum of the quietest stretches on their own (lead-in groove, gaps):
            // hum is always there, the music isn't, so that's where it shows most clearly
            inline std::optional<AveragedSpectrum> quietWelch (const std::vector<float>& l, const std::vector<float>& r, double fsd, int N)
            {
                const int hop = N >> 1;
                const long segs = floorDiv ((long) l.size() - N, hop) + 1;
                if (segs < 6) return std::nullopt;
                std::vector<std::pair<double, long>> e;
                for (long s = 0; s < segs; ++s)
                {
                    double v = 0.0;
                    for (long i = s * hop; i < s * hop + N; i += 4) v += (double) l[(size_t) i] * l[(size_t) i] + (double) r[(size_t) i] * r[(size_t) i];
                    e.push_back ({ v, s });
                }
                std::stable_sort (e.begin(), e.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
                // only stretches about as quiet as the quietest (within 5 dB), at most a fifth of the file
                std::vector<std::int64_t> starts;
                const long most = std::max (1L, (long) roundUp ((double) segs * 0.2));
                for (size_t i = 0; i < e.size(); ++i)
                    if (i == 0 || (e[i].first <= 3 * e[0].first && (long) i < most)) starts.push_back (e[i].second * hop);
                const float* ch[2] = { l.data(), r.data() };
                return welchAt (ch, 2, fsd, N, starts);
            }

            inline double quickHum (const float* l, const float* r, long n, double fsd, double near)
            {
                const int N = humFrame (fsd);
                if (n < N) return 0.0;
                const float* ch[2] = { l, r };
                const auto sp = welch (ch, 2, n, fsd, N, 24);
                const double df = sp.binHz;
                int best = (int) roundUp (near / df);
                for (int k = (int) roundUp ((near - 1) / df); k <= (int) roundUp ((near + 1) / df); ++k)
                    if (sp.power[(size_t) k] > sp.power[(size_t) best]) best = k;
                return best * df;
            }
        }

        // Mains hum is a perfectly steady line at 50 or 60 Hz (or a little off,
        // when the deck ran off speed while the hum was recorded) plus whole-number
        // multiples of it. Music lines are short and wander, so in a long average
        // they're broad hills; hum is a needle. Each candidate frequency is scored
        // by how far its harmonics stand above their immediate neighbourhood.
        inline Hum detectHum (const std::vector<float>& decL, const std::vector<float>& decR, double fsd)
        {
            const int N = detail::humFrame (fsd);   // about 5.5 s: 0.18 Hz resolution
            const float* ch[2] = { decL.data(), decR.data() };
            const auto all = welch (ch, 2, (std::int64_t) decL.size(), fsd, N);
            const auto quiet = detail::quietWelch (decL, decR, fsd, 1 << (int) roundUp (std::log2 (fsd * 2.6)));
            const auto La = detail::levels (all);
            const auto Lq = quiet ? detail::levels (*quiet) : std::vector<double> {};
            auto prominence = [&] (double F)
            {
                const auto a = detail::prominence (all, La, F);
                if (! quiet) return a;
                const auto b = detail::prominence (*quiet, Lq, F);
                return b.prom > a.prom ? detail::Prom { b.prom, a.prom > 6 ? a.peakHz : b.peakHz, b.level } : a;
            };
            double bestF = 0, bestScore = -1;
            for (double f = 45; f <= 66; f += 0.02)
            {
                double score = 0;
                for (int h = 1; h <= 8; ++h) score += std::max (0.0, std::min (40.0, prominence (f * h).prom)) / std::sqrt ((double) h);
                if (score > bestScore) { bestScore = score; bestF = f; }
            }
            // refine on the harmonics that clearly stand out
            double wsum = 0, fsum = 0;
            Hum out;
            for (int h = 1; h <= 24 && bestF * h < 0.45 * fsd; ++h)
            {
                const auto p = prominence (bestF * h);
                if (p.prom >= 8 && std::abs (p.peakHz / h - bestF) < 0.3)
                {
                    out.harmonics.push_back ({ h, rounded (p.prom, 1), p.peakHz });
                    if (h <= 12)
                    {
                        const double w = std::pow (10.0, std::min (p.prom, 40.0) / 20.0) * h;
                        wsum += w;
                        fsum += w * p.peakHz / h;
                    }
                }
            }
            const double hz = wsum != 0 ? fsum / wsum : bestF;
            int strongLow = 0;
            for (const auto& x : out.harmonics) if (x.h <= 6 && x.prom >= 10) ++strongLow;
            out.found = out.harmonics.size() >= 2 && strongLow >= 1;
            // its level: the power in its lines (dBFS), from the averaged spectrum (the
            // quiet stretches' when there are some: there the music doesn't add to it)
            const auto& sp = quiet ? *quiet : all;
            double pw = 0;
            for (const auto& x : out.harmonics)
            {
                const int k = (int) roundUp (x.hz / sp.binHz);
                for (int j = k - 2; j <= k + 2; ++j) if (j >= 0 && j < (int) sp.power.size()) pw += sp.power[(size_t) j];
            }
            const double levelDb = dbPower (2 * pw / ((double) sp.size * 3 * sp.size / 8) / 2);   // Hann window, per channel
            // drift: the first and second half on their own
            double drift = 0;
            if (out.found && (double) decL.size() > fsd * 30)
            {
                const long half = (long) decL.size() >> 1, rest = (long) decL.size() - half;
                const double a = detail::quickHum (decL.data(), decR.data(), half, fsd, hz);
                const double b = detail::quickHum (decL.data() + half, decR.data() + half, rest, fsd, hz);
                if (a != 0 && b != 0) drift = std::abs (a - b);
            }
            out.hz = rounded (hz, 2);
            if (! out.found) out.harmonics.clear();
            if (out.found) out.levelDb = rounded (levelDb, 1);
            out.drift = rounded (drift, 2);
            out.score = rounded (bestScore, 1);
            return out;
        }

        // ---- tuning (speed)

        struct Tuning { bool found = false; double cents = 0, confidence = 0; int peaks = 0, voiced = 0; std::vector<double> hist; };

        // Bands tuned to A440 (mostly): every steady note sits a whole number of
        // semitones from it, so the cents left over (mod 100) pile up at one
        // value, how far off the whole transfer is. A long average over many notes,
        // so one out-of-tune instrument doesn't decide it. Cents sharp (positive)
        // or flat, -50..50.
        inline Tuning detectTuning (const std::vector<float>& mid, double fsd, double a4 = 440.0)
        {
            const int N = 4096, Z = 8192, hop = 2048;
            const auto w = windows::hannPeriodic (N);
            const auto& f = fftFor (Z);
            const long total = std::max (0L, floorDiv ((long) mid.size() - N, hop) + 1), use = std::min (total, 2400L);
            std::vector<double> hist (100, 0.0), re ((size_t) Z), im ((size_t) Z), mag ((size_t) Z / 2 + 3, 0.0);
            const int lo = (int) std::ceil (100.0 * Z / fsd), hi = std::min (Z / 2 - 2, (int) std::floor (2500.0 * Z / fsd));
            Tuning out;
            for (long s = 0; s < use; ++s)
            {
                const long at = (long) std::floor ((s + 0.5) * (double) total / (double) use) * hop;
                std::fill (re.begin(), re.end(), 0.0);
                std::fill (im.begin(), im.end(), 0.0);
                double e = 0;
                for (int i = 0; i < N; ++i) { const double v = mid[(size_t) (at + i)]; re[(size_t) i] = v * w[(size_t) i]; e += v * v; }
                if (e / N < 1e-7) continue;   // quieter than about -70 dB: nothing to hear
                f.forward (re.data(), im.data());
                double mx = -1e9;
                for (int k = lo - 2; k <= hi + 2; ++k)
                {
                    mag[(size_t) k] = dbPower (re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k]);
                    if (k >= lo && k <= hi && mag[(size_t) k] > mx) mx = mag[(size_t) k];
                }
                // the strongest clear peaks in this frame, standing well clear of their surroundings
                std::vector<int> cand;
                for (int k = lo; k <= hi; ++k)
                {
                    if (mag[(size_t) k] < mx - 30 || mag[(size_t) k] < mag[(size_t) (k - 1)] || mag[(size_t) k] < mag[(size_t) (k + 1)]) continue;
                    const double ring = (mag[(size_t) (k - 8)] + mag[(size_t) (k + 8)] + mag[(size_t) (k - 12)] + mag[(size_t) (k + 12)]) / 4;
                    if (mag[(size_t) k] - ring < 10) continue;
                    cand.push_back (k);
                }
                std::stable_sort (cand.begin(), cand.end(), [&] (int x, int y) { return mag[(size_t) y] < mag[(size_t) x]; });
                if (! cand.empty()) ++out.voiced;
                for (size_t q = 0; q < cand.size() && q < 10; ++q)
                {
                    const int k = cand[q];
                    const double a = mag[(size_t) (k - 1)], b = mag[(size_t) k], c = mag[(size_t) (k + 1)], den = a - 2 * b + c;
                    const double shift = std::abs (den) > 1e-9 ? 0.5 * (a - c) / den : 0.0;
                    const double hz = (k + shift) * fsd / Z, cents = 1200.0 * std::log2 (hz / a4);
                    const double dev = std::fmod (std::fmod (cents, 100.0) + 150.0, 100.0) - 50.0;   // -50..50 from the nearest note
                    const double weight = std::pow (10.0, (b - mx) / 40.0);                         // louder peaks count more (gently)
                    const int bin = (int) std::fmod (std::fmod (roundUp (dev), 100.0) + 100.0, 100.0);
                    hist[(size_t) bin] += weight;
                    ++out.peaks;
                }
            }
            out.hist = hist;
            if (out.peaks < 30) return out;
            // circular smoothing, the peak, then a weighted circular mean around it
            std::vector<double> sm (100, 0.0);
            for (int i = 0; i < 100; ++i)
                for (int j = -6; j <= 6; ++j) sm[(size_t) i] += hist[(size_t) ((i + j + 100) % 100)] * std::exp (-(j * j) / (2 * 2.5 * 2.5));
            int top = 0;
            for (int i = 0; i < 100; ++i) if (sm[(size_t) i] > sm[(size_t) top]) top = i;
            double sx = 0, sy = 0, inside = 0, all = 0;
            for (int i = 0; i < 100; ++i)
            {
                all += hist[(size_t) i];
                const int d = ((i - top + 150) % 100) - 50;
                if (std::abs (d) <= 15) { const double ang = 2 * kPi * i / 100; sx += hist[(size_t) i] * std::cos (ang); sy += hist[(size_t) i] * std::sin (ang); }
                if (std::abs (d) <= 10) inside += hist[(size_t) i];
            }
            double cents = std::atan2 (sy, sx) / (2 * kPi) * 100;
            cents = std::fmod (cents + 150, 100.0) - 50;
            const double dev = std::fmod (std::fmod (cents, 100.0) + 100, 100.0);
            const double offset = dev >= 50 ? dev - 100 : dev;
            const double share = inside / std::max (1e-12, all);   // an even spread would put 21% within 10 cents
            const double confidence = std::max (0.0, std::min (1.0, (share - 0.21) / 0.5));
            out.found = confidence > 0.15;
            out.cents = rounded (offset, 1);
            out.confidence = rounded (confidence, 2);
            return out;
        }

        // ---- wow (slow pitch wobble)

        struct Wow
        {
            double rate = 50;               // values per second of `cents`
            std::vector<float> cents;       // over the input's own time (positive: running fast there)
            double confidence = 0, peak = 0, rms = 0, hz = 0, prominence = 0;
            bool found = false;
        };

        namespace detail
        {
            inline double weightedMedian (const std::vector<double>& v, const std::vector<double>& w)
            {
                std::vector<size_t> idx (v.size());
                for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
                std::stable_sort (idx.begin(), idx.end(), [&] (size_t a, size_t b) { return v[a] < v[b]; });
                double total = 0;
                for (double x : w) total += x;
                double acc = 0;
                for (size_t i : idx) { acc += w[i]; if (acc >= total / 2) return v[i]; }
                return v[idx.back()];
            }

            // The strongest repetition rate in a curve (0.3..6 Hz) and how clearly it
            // stands out: a warp or an off-centre hole repeats once a turn, a sharp
            // peak; the measuring noise wanders without one. Whitened (times f
            // squared): that noise adds up like a random walk, falling as 1/f^2.
            inline std::pair<double, double> mainRate (const std::vector<float>& x, double rate)
            {
                int N = 1;
                while (N < (int) x.size()) N <<= 1;
                N = std::min (N, 1 << 16);
                std::vector<double> re ((size_t) N, 0.0), im ((size_t) N, 0.0);
                const int m = std::min (N, (int) x.size());
                const auto w = windows::hannPeriodic (m);
                for (int i = 0; i < m; ++i) re[(size_t) i] = x[(size_t) i] * w[(size_t) i];
                fftFor (N).forward (re.data(), im.data());
                const int lo = (int) std::ceil (0.3 * N / rate), hi = (int) std::floor (6.0 * N / rate);
                std::vector<double> P;
                for (int k = lo; k <= hi; ++k) P.push_back ((re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k]) * k * k);
                const int half = std::max (1, (int) roundUp (0.025 * N / rate));
                std::vector<double> sm (P.size());
                for (int i = 0; i < (int) P.size(); ++i)
                {
                    double s = 0; int c = 0;
                    for (int j = std::max (0, i - half); j <= std::min ((int) P.size() - 1, i + half); ++j) { s += P[(size_t) j]; ++c; }
                    sm[(size_t) i] = s / c;
                }
                int bk = -1;
                const int from = std::max (0, (int) std::ceil (0.4 * N / rate) - lo);
                for (int i = from; i < (int) sm.size(); ++i) if (bk < 0 || sm[(size_t) i] > sm[(size_t) bk]) bk = i;
                if (bk < 0) return { 0.0, 0.0 };
                return { (lo + bk) * rate / N, sm[(size_t) bk] / (stats::median (sm.data(), (int) sm.size()) + 1e-30) };
            }
        }

        // Wow is the slow wobble of a warped record or a stretched tape: the whole
        // band drifts sharp and flat a few times a second. Every steady partial
        // wobbles together, so the frame-to-frame pitch change of many partials
        // (measured precisely from the phase, like a phase vocoder) is combined with
        // a weighted median, added up into a pitch curve, and kept to 0.25..6 Hz.
        inline Wow detectWow (const std::vector<float>& mid, double fsd)
        {
            int f2 = (int) roundUp (fsd / 5800.0);
            if (f2 == 0) f2 = 1;
            const double fs = fsd / f2;
            std::vector<float> d ((size_t) mid.size() + 1);
            {
                Decimator dc (fsd, f2);
                d.resize ((size_t) dc.push (mid.data(), (int) mid.size(), d.data()));
            }
            const int N = 512, H = 128, half = N / 2;   // 87 ms frames: shorter than a polka note
            const auto w = windows::hannPeriodic (N);
            const auto& F = fftFor (N);
            const int frames = (int) std::max (0L, floorDiv ((long) d.size() - N, H) + 1);
            std::vector<double> re ((size_t) N), im ((size_t) N), mag ((size_t) half, 0.0), phase ((size_t) half, 0.0), prevPhase ((size_t) half, 0.0);
            std::vector<double> ifr ((size_t) half, 0.0), prevIf ((size_t) half, 0.0), prevMag ((size_t) half, 0.0);
            std::vector<double> D ((size_t) frames, 0.0), used ((size_t) frames, 0.0), flux ((size_t) frames, 0.0);
            const int lo = (int) std::ceil (150.0 * N / fs), hi = std::min (half - 2, (int) std::floor (2500.0 * N / fs));
            auto expect = [&] (int k) { return 2 * kPi * k * H / N; };
            auto wrap = [] (double a) { return a - 2 * kPi * roundUp (a / (2 * kPi)); };
            for (int t = 0; t < frames; ++t)
            {
                const int at = t * H;
                for (int i = 0; i < N; ++i) { re[(size_t) i] = d[(size_t) (at + i)] * w[(size_t) i]; im[(size_t) i] = 0.0; }
                F.forward (re.data(), im.data());
                double mx = 0, up = 0, tot = 0;
                for (int k = 0; k < half; ++k)
                {
                    mag[(size_t) k] = std::hypot (re[(size_t) k], im[(size_t) k]);
                    phase[(size_t) k] = std::atan2 (im[(size_t) k], re[(size_t) k]);
                    if (k >= lo && k <= hi)
                    {
                        if (mag[(size_t) k] > mx) mx = mag[(size_t) k];
                        up += std::max (0.0, mag[(size_t) k] - prevMag[(size_t) k]);
                        tot += mag[(size_t) k];
                    }
                }
                flux[(size_t) t] = tot > 0 ? up / tot : 0.0;   // how much new sound arrived (a note or a hit starting)
                for (int k = lo; k <= hi; ++k)
                    ifr[(size_t) k] = (k + wrap (phase[(size_t) k] - prevPhase[(size_t) k] - expect (k)) / expect (1)) * fs / N;
                if (t >= 2 && mx > 1e-5)
                {
                    std::vector<double> ds, ws;
                    for (int k = lo; k <= hi; ++k)
                    {
                        if (mag[(size_t) k] < 0.03 * mx || mag[(size_t) k] < mag[(size_t) (k - 1)] || mag[(size_t) k] < mag[(size_t) (k + 1)]) continue;
                        // the same partial a hop ago (it can move a bin)
                        int pk = -1;
                        double pd = 1e9;
                        for (int j = k - 1; j <= k + 1; ++j)
                            if (prevMag[(size_t) j] > 0.3 * mag[(size_t) k] && std::abs (prevIf[(size_t) j] - ifr[(size_t) k]) < pd) { pd = std::abs (prevIf[(size_t) j] - ifr[(size_t) k]); pk = j; }
                        if (pk < 0 || prevIf[(size_t) pk] <= 0 || ifr[(size_t) k] <= 0) continue;
                        const double dd = 1200.0 * std::log2 (ifr[(size_t) k] / prevIf[(size_t) pk]);
                        if (std::abs (dd) > 8 || std::abs (prevMag[(size_t) pk] - mag[(size_t) k]) > 0.5 * mag[(size_t) k]) continue;   // a note change or an onset
                        ds.push_back (dd);
                        ws.push_back (mag[(size_t) k]);
                    }
                    if (ds.size() >= 2) { D[(size_t) t] = detail::weightedMedian (ds, ws); used[(size_t) t] = (double) ds.size(); }
                }
                prevPhase = phase;
                prevIf = ifr;
                prevMag = mag;
            }
            // When a chord changes, a harmonic of the new chord can sit a few cents
            // from one of the old, which reads as a tiny pitch step; hundreds of those
            // would add up to a wobble that isn't there. So the frames around each
            // note or hit starting are skipped, and the slope bridged across them.
            std::vector<double> positive;
            for (double v : flux) if (v > 0) positive.push_back (v);
            const double fluxRef = stats::median (positive.data(), (int) positive.size());
            std::vector<char> bad ((size_t) frames, 0);
            const int span = (int) std::ceil ((double) N / H) + 1;
            for (int t = 1; t < frames; ++t)
                if (flux[(size_t) t] > std::max (0.08, 2.5 * fluxRef))
                    for (int j = t; j < std::min (frames, t + span); ++j) bad[(size_t) j] = 1;
            for (int t = 0; t < frames; ++t) if (used[(size_t) t] == 0) bad[(size_t) t] = 1;
            const double rate = fs / H;
            for (int t = 0; t < frames;)
            {
                if (! bad[(size_t) t]) { ++t; continue; }
                int k = t;
                while (k < frames && bad[(size_t) k]) ++k;
                const double a = t > 0 ? D[(size_t) (t - 1)] : 0.0, b = k < frames ? D[(size_t) k] : a;
                const int gap = k - t;
                const bool fillable = gap <= rate * 0.6;   // bridge up to 0.6 s, else assume no change
                for (int j = t; j < k; ++j) { D[(size_t) j] = fillable ? a + (b - a) * (j - t + 1) / (gap + 1) : 0.0; used[(size_t) j] = fillable ? 2 : 0; }
                t = k;
            }
            // add up the changes into a pitch curve, then keep only the wobble band
            std::vector<double> c ((size_t) frames, 0.0);
            for (int t = 1; t < frames; ++t) c[(size_t) t] = c[(size_t) (t - 1)] + D[(size_t) t];
            Wow out;
            if (frames < rate * 4)
            {
                out.cents.assign ((size_t) std::ceil ((double) mid.size() / fsd * 50.0), 0.0f);
                return out;
            }
            auto curve = zeroPhase (butterworth (BiquadCoefs::Type::highPass, rate, 0.25, 2), c.data(), frames);
            curve = zeroPhase (butterworth (BiquadCoefs::Type::lowPass, rate, 6.0, 4), curve.data(), frames);
            // where too few partials were heard, trust the curve less (fade it out)
            std::vector<double> conf ((size_t) frames);
            for (int t = 0; t < frames; ++t) conf[(size_t) t] = std::min (1.0, used[(size_t) t] / 2);
            const auto confS = zeroPhase (butterworth (BiquadCoefs::Type::lowPass, rate, 1.0, 2), conf.data(), frames);
            // onto a 50-per-second grid over the input's own time
            const double seconds = (double) mid.size() / fsd;
            const size_t len = (size_t) std::ceil (seconds * 50) + 1;
            out.cents.assign (len, 0.0f);
            std::vector<float> cOut (len, 0.0f);
            auto get = [&] (const std::vector<double>& a, int q) { return a[(size_t) std::max (0, std::min (frames - 1, q))]; };
            for (size_t i = 0; i < len; ++i)
            {
                const double t = (((double) i / 50) * fs - N / 2.0) / H + 0.5;
                const int j = (int) std::floor (t);
                const double fr = t - j;
                const double cv = get (curve, j) + fr * (get (curve, j + 1) - get (curve, j));
                const double cc = std::max (0.0, std::min (1.0, get (confS, j) + fr * (get (confS, j + 1) - get (confS, j))));
                out.cents[i] = (float) (cv * cc);
                cOut[i] = (float) cc;
            }
            double peak = 0, ss = 0;
            long cn = 0;
            for (size_t i = 0; i < len; ++i)
                if (cOut[i] > 0.5f) { peak = std::max (peak, (double) std::abs (out.cents[i])); ss += (double) out.cents[i] * out.cents[i]; ++cn; }
            const double rms = cn != 0 ? std::sqrt (ss / cn) : 0.0;
            const auto [hz, prominence] = detail::mainRate (out.cents, 50.0);
            const double confidence = (double) cn / (double) len;
            // On busy polka with no wow at all, the curve wanders 1 to 2.5 cents RMS by
            // itself, slowly and without a beat. Wow counts when it's bigger than that
            // and repeats at a clear rate (a beat 12 dB over the rest, at least 0.4 a second).
            out.found = rms >= 2 && confidence > 0.3 && prominence >= 15 && hz >= 0.4;
            out.confidence = rounded (confidence, 2);
            out.peak = rounded (peak, 1);
            out.rms = rounded (rms, 2);
            out.hz = rounded (hz, 2);
            out.prominence = rounded (prominence, 1);
            return out;
        }

        // ---- tape dropouts

        struct Dropout { double start, end, depthDb; int ch; };   // ch 2: both

        // A dropout is a moment where the tape lost contact with the head: the
        // highs vanish, hiss and all. Music never goes quieter than the tape's own
        // hiss, so a dip of the treble below the hiss floor is a dropout, not a
        // quiet bar. profile: the learned noise profile (frame size n at rate fs),
        // which says how loud the hiss is up there; without one, the quietest 1% of
        // the file stands in.
        inline std::vector<Dropout> detectDropouts (const std::vector<float>& decL, const std::vector<float>& decR, double fsd,
                                                    const std::vector<float>* profile = nullptr, int profileN = 0, double profileFs = 0)
        {
            const int hop = (int) roundUp (0.005 * fsd);
            const double hpHz = std::min (3000.0, 0.3 * fsd);
            std::optional<double> hissDb;
            if (profile != nullptr && ! profile->empty())
            {
                double p = 0;
                for (int k = (int) std::ceil (hpHz * profileN / profileFs); k <= (int) std::floor (0.42 * fsd * profileN / profileFs) && k < (int) profile->size(); ++k) p += (*profile)[(size_t) k];
                hissDb = dbPower (2 * p / ((double) profileN * 3 * profileN / 8) / 2);
            }
            std::vector<Dropout> found;
            for (int ch = 0; ch < 2; ++ch)
            {
                const auto& x = ch == 0 ? decL : decR;
                std::vector<double> y (x.begin(), x.end());
                BiquadChain (butterworth (BiquadCoefs::Type::highPass, fsd, hpHz, 4), 1).process (0, y.data(), (int) y.size());
                const int nf = (int) y.size() / hop;
                std::vector<double> env ((size_t) nf);
                for (int j = 0; j < nf; ++j)
                {
                    double s = 0;
                    for (int i = j * hop; i < (j + 1) * hop; ++i) s += y[(size_t) i] * y[(size_t) i];
                    env[(size_t) j] = dbPower (s / hop);
                }
                std::vector<double> live;
                for (double v : env) if (v > -110) live.push_back (v);   // digital silence aside
                if (live.size() < 200) continue;
                const double floor = hissDb ? *hissDb : stats::percentile (live.data(), (int) live.size(), 0.01);
                if (stats::percentile (live.data(), (int) live.size(), 0.5) < floor + 6) continue;   // highs barely above the hiss
                const double thr = floor - 6;
                for (int j = 0; j < nf;)
                {
                    if (env[(size_t) j] < thr && env[(size_t) j] > -110)
                    {
                        int k = j;
                        double low = env[(size_t) j];
                        while (k + 1 < nf && env[(size_t) (k + 1)] < thr + 2 && env[(size_t) (k + 1)] > -110) { ++k; low = std::min (low, env[(size_t) k]); }
                        const double dur = (double) (k - j + 1) * hop / fsd;
                        if (dur >= 0.01 && dur <= 1.0)
                            found.push_back ({ rounded ((double) j * hop / fsd, 3), rounded ((double) (k + 1) * hop / fsd, 3), rounded (floor - low, 1), ch });
                        j = k + 1;
                    }
                    else ++j;
                }
            }
            // the same dropout in both channels is one event
            std::stable_sort (found.begin(), found.end(), [] (const Dropout& a, const Dropout& b) { return a.start < b.start; });
            std::vector<Dropout> merged;
            for (const auto& dd : found)
            {
                if (! merged.empty() && dd.start <= merged.back().end + 0.01)
                {
                    auto& last = merged.back();
                    last.end = std::max (last.end, dd.end);
                    last.depthDb = std::max (last.depthDb, dd.depthDb);
                    last.ch = last.ch == dd.ch ? dd.ch : 2;
                }
                else merged.push_back (dd);
            }
            return merged;
        }

        // ---- the songs on a side

        struct Span { double start, end; };
        struct Tracks { std::vector<Span> tracks, gaps; double seconds = 0; std::optional<double> floorDb, musicDb; };

        // (findTracks is below, with the gaps it is built on)

        // ---- the quietest spots, for the noise profile

        // Where the quietest hops start (in samples): the lowest-energy hops that
        // aren't digital silence, quietest first, about 3% of them (at least
        // `want`, at most 3 want).
        inline std::vector<std::int64_t> quietSpots (const std::vector<float>& hopEnergy, int hop, int want = 160)
        {
            std::vector<int> idx;
            for (int i = 0; i < (int) hopEnergy.size(); ++i) if (hopEnergy[(size_t) i] > 1e-13f) idx.push_back (i);
            std::stable_sort (idx.begin(), idx.end(), [&] (int a, int b) { return hopEnergy[(size_t) a] < hopEnergy[(size_t) b]; });
            const size_t take = std::min (idx.size(), std::max ((size_t) want, (size_t) roundUp ((double) idx.size() * 0.03)));
            std::vector<std::int64_t> out;
            for (size_t i = 0; i < take && i < (size_t) want * 3; ++i) out.push_back ((std::int64_t) idx[i] * hop);
            return out;
        }

        // ---- check windows

        // Where the check windows fall: up to 40 windows of 1.5 s spread across
        // the file, one per 3 s of it.
        struct Window { std::int64_t at; int length; };
        inline std::vector<Window> checkWindows (std::int64_t frames, double fs)
        {
            const double seconds = (double) frames / fs;
            const int nWin = std::max (1, std::min (40, (int) std::floor (seconds / 3)));
            const int winLen = (int) std::min<std::int64_t> (frames, (std::int64_t) roundUp (1.5 * fs));
            std::vector<Window> out;
            for (int k = 0; k < nWin; ++k)
                out.push_back ({ std::max<std::int64_t> (0, (std::int64_t) roundUp ((k + 0.5) / nWin * frames - winLen / 2.0)), winLen });
            return out;
        }

        // How loud a window is, for picking the loud ones (every 8th sample).
        inline double windowEnergy (const float* l, const float* r, int n)
        {
            double e = 0;
            for (int i = 0; i < n; i += 8) e += (double) l[i] * l[i] + (double) r[i] * r[i];
            return e;
        }

        // ---- how the channels compare

        struct Channels { double corr = 0, sideDb = 0, balanceDb = 0, delay = 0, delayUs = 0; int delayN = 0; bool mono = false, nearlyMono = false, flipped = false; };

        // From loud windows (full rate, each fed with add()): how alike the
        // channels are, which is louder, whether one is upside down, and how far one
        // lags the other (cassette azimuth: a head tilted a fraction of a degree
        // delays one track by tens of microseconds and dulls the highs when summed).
        class ChannelCompare
        {
        public:
            explicit ChannelCompare (double sampleRate) : fs (sampleRate), maxLag (std::max (4, (int) roundUp (0.0008 * sampleRate))) {}

            void add (const float* l, const float* r, int n)
            {
                double a = 0, b = 0, c = 0;
                for (int i = 0; i < n; ++i) { a += (double) l[i] * l[i]; b += (double) r[i] * r[i]; c += (double) l[i] * r[i]; }
                ll += a; rr += b; lr += c;
                if (a > 1e-9 && b > 1e-9) levelDiffs.push_back (10.0 * std::log10 (a / b));
                // the delay from the highs (where it matters, and where the peak is sharp)
                std::vector<double> hl (l, l + n), hr (r, r + n);
                const auto f = butterworth (BiquadCoefs::Type::highPass, fs, std::min (1000.0, 0.2 * fs), 4);
                BiquadChain (f, 1).process (0, hl.data(), n);
                BiquadChain (f, 1).process (0, hr.data(), n);
                std::vector<double> xc ((size_t) (2 * maxLag + 1));
                double el = 0, er = 0;
                for (int i = maxLag; i < n - maxLag; ++i) { el += hl[(size_t) i] * hl[(size_t) i]; er += hr[(size_t) i] * hr[(size_t) i]; }
                for (int d = -maxLag; d <= maxLag; ++d)
                {
                    double s = 0;
                    for (int i = maxLag; i < n - maxLag; ++i) s += hl[(size_t) i] * hr[(size_t) (i + d)];
                    xc[(size_t) (d + maxLag)] = s / std::sqrt (el * er + 1e-30);
                }
                int bi = 0;
                for (int k = 1; k < (int) xc.size(); ++k) if (std::abs (xc[(size_t) k]) > std::abs (xc[(size_t) bi])) bi = k;
                if (std::abs (xc[(size_t) bi]) > 0.4 && bi > 0 && bi < (int) xc.size() - 1)
                {
                    const double y0 = std::abs (xc[(size_t) (bi - 1)]), y1 = std::abs (xc[(size_t) bi]), y2 = std::abs (xc[(size_t) (bi + 1)]), den = y0 - 2 * y1 + y2;
                    delays.push_back ({ bi - maxLag + (std::abs (den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0.0), xc[(size_t) bi] });
                }
            }

            Channels result() const
            {
                Channels out;
                const double corr = lr / std::sqrt (ll * rr + 1e-30);
                // the side with the two levels matched first, so a mono record with one
                // channel quieter still reads as mono: 2 (1 - corr) / 2 (1 + corr)
                const double sideDb = 10.0 * std::log10 ((1 - corr + 1e-12) / (1 + corr + 1e-12));
                std::vector<double> good;
                for (const auto& [d, c] : delays) if (std::abs (c) > 0.5) good.push_back (d);
                const double delay = good.size() >= 3 ? stats::median (good.data(), (int) good.size()) : 0.0;
                out.corr = rounded (corr, 3);
                out.sideDb = rounded (sideDb, 1);
                out.balanceDb = rounded (levelDiffs.empty() ? 0.0 : stats::median (levelDiffs.data(), (int) levelDiffs.size()), 2);
                out.delay = rounded (delay, 2);
                out.delayUs = rounded (delay / fs * 1e6, 1);
                out.delayN = (int) good.size();
                // stricter than it needs to be: better to leave a narrow stereo mix alone than to fold it to mono
                out.mono = sideDb < -26;
                out.nearlyMono = sideDb < -14;
                out.flipped = corr < -0.5;
                return out;
            }

        private:
            double fs;
            int maxLag;
            double ll = 0, rr = 0, lr = 0;
            std::vector<double> levelDiffs;
            std::vector<std::pair<double, double>> delays;
        };

        // ---- how clicky it is

        struct ClickCount { double seconds = 0; int click[2] { 0, 0 }, crackle[2] { 0, 0 }; double perMinute[2] { 0, 0 }; std::vector<std::int64_t> positions; };

        // The de-clicker in counting mode on short windows across the file (each fed
        // with add()), each channel on its own (for picking the better channel of a
        // mono record).
        class ClickCounter
        {
        public:
            ClickCounter (double sampleRate, double clicks = 50, double crackle = 40) : fs (sampleRate)
            {
                settings.clicks = clicks;
                settings.crackle = crackle;
                settings.detectOnly = true;
            }

            void add (const float* l, const float* r, int n, std::int64_t at)
            {
                Declicker d (fs, settings);
                const float* ch[2] = { l, r };
                d.push (ch, n);
                d.flush();
                for (const auto& e : d.events())
                {
                    if (e.kind == declick::Kind::click) ++out.click[e.ch];
                    if (e.kind == declick::Kind::crackle) ++out.crackle[e.ch];
                    if (e.ch == 0) out.positions.push_back (at + e.at);
                }
                secs += n / fs;
            }

            ClickCount result() const
            {
                ClickCount r = out;
                for (int c = 0; c < 2; ++c) r.perMinute[c] = roundUp ((out.click[c] + out.crackle[c]) / std::max (1e-9, secs) * 60);
                r.seconds = rounded (secs, 1);
                return r;
            }

        private:
            double fs, secs = 0;
            declick::Settings settings;
            ClickCount out;
        };

        // ---- a side's level envelope and the gaps in it

        // The mid channel's level every `hop` seconds (dB), the median of five
        // sub-blocks so a click in a gap doesn't make it look loud, after a gentle
        // high-pass (rumble and DC aren't music).
        class LevelEnvelope
        {
        public:
            explicit LevelEnvelope (double fs, double hopSeconds = 0.05)
                : sub (std::max (1, (int) roundUp (fs * hopSeconds / 5))), a (std::exp (-2 * kPi * 100 / fs))
            {
                hop = 5.0 * sub / fs;
            }

            void push (const float* l, const float* r, int n)
            {
                for (int i = 0; i < n; ++i)
                {
                    const double x = 0.5 * ((double) l[i] + (double) (r != nullptr ? r[i] : l[i]));
                    y = a * (y + x - xp);
                    xp = x;
                    acc += y * y;
                    if (++cnt == sub)
                    {
                        subs[nsub++] = acc / cnt;
                        acc = 0;
                        cnt = 0;
                        if (nsub == 5)
                        {
                            std::sort (subs, subs + 5);
                            out.push_back ((float) (10.0 * std::log10 (subs[2] + 1e-14)));
                            nsub = 0;
                        }
                    }
                }
            }

            double hopSeconds() const { return hop; }
            const std::vector<float>& levels() const { return out; }

        private:
            int sub, cnt = 0, nsub = 0;
            double a, hop, y = 0, xp = 0, acc = 0, subs[5] {};
            std::vector<float> out;
        };

        // Recording a side: is it over (the run-out groove, or the tape running on)?
        // Fed the level (dB) of each short batch while recording, and its length. Says
        // it's over once the last `seconds` have all sat down at the side's own noise
        // floor (the quietest it has been, from a half-dB histogram), well under its
        // music, or in digital silence, and only after `minMusic` seconds of music.
        // Judged against the side itself, so a hissy 78 or a humming transfer stops as
        // surely as a quiet LP.
        class RunoutWatch
        {
        public:
            RunoutWatch (double seconds = 15, double minMusic = 20) : wantQuiet (seconds), wantMusic (minMusic) {}

            // The next batch's level and length; true once the side is over.
            bool push (double db, double sec)
            {
                ++hist[(size_t) std::max (0, std::min ((int) hist.size() - 1, (int) roundUp ((db + 120) * 2)))];
                ++count;
                recent.push_back (db);
                if (recent.size() > 8) recent.erase (recent.begin());
                std::vector<double> sorted (recent);
                std::sort (sorted.begin(), sorted.end());
                const double med = sorted[sorted.size() >> 1];
                const double floor = percentile (0.03), ref = percentile (0.95), range = ref - floor;
                if (range >= 12 && med > floor + 0.5 * range) music += sec;
                const bool quiet = med < -90 || (range >= 12 && med < floor + std::max (3.0, 0.25 * range));
                quietFor = quiet ? quietFor + sec : 0;
                return music >= wantMusic && quietFor >= wantQuiet;
            }

        private:
            double wantQuiet, wantMusic, music = 0, quietFor = 0;
            std::vector<unsigned> hist = std::vector<unsigned> (240, 0);   // -120..0 dBFS in half dB
            long count = 0;
            std::vector<double> recent;

            double percentile (double p) const
            {
                const long want = std::max (1L, (long) std::ceil ((double) count * p));
                long acc = 0;
                for (size_t i = 0; i < hist.size(); ++i)
                {
                    acc += hist[i];
                    if (acc >= want) return -120 + (double) i * 0.5;
                }
                return 0;
            }
        };

        struct GapCandidate { double start, end, at, min, depth, score; };
        struct Gaps { std::vector<GapCandidate> cands; double musicStart = 0, musicEnd = 0, seconds = 0, floor = -120, music = -120; };

        // Quiet dips in a level envelope that could be gaps between songs, each
        // scored (deeper and longer is likelier), and where the music starts and
        // ends on the side. The next song starts at the end of a dip; its split
        // sits a moment before that.
        // A side's level envelope median-smoothed over seven steps (so one click in a
        // gap doesn't split it), and its noise floor and music level: what the songs
        // and the gaps between them are both judged on.
        struct SideLevels
        {
            std::vector<float> sm;
            double floor = -120, music = -120;
            // quieter than this is not music (well below the music, near the floor)
            double quiet() const { return floor + std::max (6.0, 0.3 * (music - floor)); }
        };

        inline SideLevels sideLevels (const std::vector<float>& lv)
        {
            const int n = (int) lv.size();
            SideLevels out;
            out.sm.resize ((size_t) n);
            std::vector<float> buf;
            for (int j = 0; j < n; ++j)
            {
                buf.assign (lv.begin() + std::max (0, j - 3), lv.begin() + std::min (n - 1, j + 3) + 1);
                std::sort (buf.begin(), buf.end());
                out.sm[(size_t) j] = buf[buf.size() >> 1];
            }
            std::vector<double> live;
            for (float v : out.sm) if (v > -120) live.push_back (v);
            std::sort (live.begin(), live.end());
            auto pct = [&] (double p) { return live.empty() ? -120.0 : live[(size_t) std::min ((double) live.size() - 1, std::max (0.0, std::floor (p * ((double) live.size() - 1))))]; };
            out.floor = pct (0.05);
            out.music = pct (0.9);
            return out;
        }

        inline Gaps gapCandidates (const std::vector<float>& lv, double hop = 0.05)
        {
            const int n = (int) lv.size();
            const double seconds = n * hop;
            const auto side = sideLevels (lv);
            const auto& sm = side.sm;
            const double floor = side.floor, music = side.music, range = music - floor;
            Gaps out;
            out.floor = floor;
            out.music = music;
            if (n < 20 || range < 6) { out.musicEnd = seconds; out.seconds = seconds; return out; }
            const double strict = side.quiet(), loose = floor + 0.55 * range;
            int ms = 0;
            while (ms < n && sm[(size_t) ms] < strict) ++ms;
            int me = n - 1;
            while (me > ms && sm[(size_t) me] < strict) --me;
            for (int j = ms; j <= me;)
            {
                if (sm[(size_t) j] >= loose) { ++j; continue; }
                int k = j;
                while (k <= me && sm[(size_t) k] < loose) ++k;
                double mn = 1e300;
                for (int q = j; q < k; ++q) if (sm[(size_t) q] < mn) mn = sm[(size_t) q];
                const double dur = (k - j) * hop, depth = std::max (0.0, std::min (1.0, (music - mn) / range));
                if (dur >= 0.2)
                {
                    const double start = j * hop, end = k * hop, at = std::max (start, end - std::min (0.5, dur / 3));
                    out.cands.push_back ({ rounded (start, 2), rounded (end, 2), rounded (at, 2), rounded (mn, 1), rounded (depth, 3),
                                           rounded (depth * depth * std::min (1.0, 0.35 + dur / 2), 3) });
                }
                j = k;
            }
            out.musicStart = rounded (ms * hop, 2);
            out.musicEnd = rounded ((me + 1) * hop, 2);
            out.seconds = rounded (seconds, 2);
            out.floor = rounded (floor, 1);
            out.music = rounded (music, 1);
            return out;
        }

        // The songs on an LP or tape side are separated by a few seconds of groove
        // noise or hiss: quieter than any music, louder than silence. Stretches of the
        // level envelope below the music and near the side's own noise floor, for at
        // least `minGap` seconds, are a gap. The tracks butt together, each split just
        // before the next song (where gapCandidates puts its split).
        inline Tracks findTracks (const std::vector<float>& lv, double hop, double minGap = 1.2, double minSong = 20)
        {
            const int nf = (int) lv.size();
            Tracks out;
            out.seconds = nf * hop;
            if (out.seconds < 2) { out.tracks.push_back ({ 0, out.seconds }); return out; }
            const auto side = sideLevels (lv);
            const double thr = side.quiet();
            // music runs, joined across short dips
            std::vector<std::pair<int, int>> runs;
            for (int j = 0; j < nf;)
            {
                if (side.sm[(size_t) j] < thr) { ++j; continue; }
                int k = j;
                while (k < nf && ! (side.sm[(size_t) k] < thr)) ++k;
                runs.push_back ({ j, k });
                j = k;
            }
            const int gapF = (int) roundUp (minGap / hop);
            std::vector<std::pair<int, int>> joined;
            for (const auto& r : runs)
            {
                if (! joined.empty() && r.first - joined.back().second < gapF) joined.back().second = r.second;
                else joined.push_back (r);
            }
            // drop slivers (a needle drop, a thump) and merge songs shorter than minSong into a neighbour
            std::vector<std::pair<int, int>> songs;
            for (const auto& r : joined) if ((r.second - r.first) * hop >= 3) songs.push_back (r);
            for (bool changed = true; changed && songs.size() > 1;)
            {
                changed = false;
                for (size_t i = 0; i < songs.size(); ++i)
                {
                    if ((songs[i].second - songs[i].first) * hop >= minSong) continue;
                    const double prevGap = i > 0 ? songs[i].first - songs[i - 1].second : 1e9;
                    const double nextGap = i < songs.size() - 1 ? songs[i + 1].first - songs[i].second : 1e9;
                    if (prevGap <= nextGap && i > 0) { songs[i - 1].second = songs[i].second; songs.erase (songs.begin() + (long) i); }
                    else if (i < songs.size() - 1) { songs[i + 1].first = songs[i].first; songs.erase (songs.begin() + (long) i); }
                    else continue;
                    changed = true;
                    break;
                }
            }
            if (songs.empty()) { out.tracks.push_back ({ 0, out.seconds }); return out; }
            for (size_t i = 1; i < songs.size(); ++i) out.gaps.push_back ({ rounded (songs[i - 1].second * hop, 2), rounded (songs[i].first * hop, 2) });
            for (size_t i = 0; i < songs.size(); ++i)
            {
                const double start = i == 0 ? std::max (0.0, songs[i].first * hop - 0.5)
                                            : std::max (out.gaps[i - 1].start, out.gaps[i - 1].end - std::min (0.5, (out.gaps[i - 1].end - out.gaps[i - 1].start) / 3));
                out.tracks.push_back ({ rounded (start, 2), 0 });
            }
            for (size_t i = 0; i < out.tracks.size(); ++i)
                out.tracks[i].end = i + 1 < out.tracks.size() ? out.tracks[i + 1].start : rounded (std::min (out.seconds, songs[i].second * hop + 2), 2);
            out.floorDb = rounded (side.floor, 1);
            out.musicDb = rounded (side.music, 1);
            return out;
        }

        // The songs on a side from its mid channel at rate fs (the analyzer's lowMid),
        // through the same level envelope a recording keeps.
        inline Tracks findTracksIn (const std::vector<float>& mid, double fs, double minGap = 1.2, double minSong = 20)
        {
            LevelEnvelope env (fs);
            env.push (mid.data(), nullptr, (int) mid.size());
            return findTracks (env.levels(), env.hopSeconds(), minGap, minSong);
        }
    }
}
