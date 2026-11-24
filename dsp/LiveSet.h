#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>
#include "Biquad.h"
#include "Fft.h"
#include "Stats.h"
#include "Util.h"
#include "Windows.h"

// The songs in a single-file live recording. The set is read once, a block at
// a time (a long show never has to sit in memory), and boiled down to a few
// features per 0.1 s frame of its mid: level, the share of its energy below
// 150 Hz, around 1.5 kHz and above 5 kHz, and how tonal it is (the spectral
// flatness of 150 Hz to 5 kHz on an 11-12 kHz copy). Every second is then
// sorted into music, talk, applause, noodling or quiet, by thresholds that
// adapt to this recording (Otsu's split of its own tonality and of a music
// score), and the music runs become songs, their edges refined to 0.1 s.
//
//   LiveSetAnalyzer a (fs);
//   a.push (l, r, n) ... for every block, in order ...
//   const auto set = a.finish();   // songs, what's between them, each second's kind

namespace vs
{
    namespace liveset
    {
        // What a second of the set is
        enum class Kind { quiet, music, applause, talk, noodling };
        inline const char* kindName (Kind k)
        {
            switch (k)
            {
                case Kind::music: return "music";
                case Kind::applause: return "applause";
                case Kind::talk: return "talk";
                case Kind::noodling: return "noodling";
                default: return "quiet";
            }
        }

        struct Song
        {
            int n = 0;                 // 1, 2, ...
            double start = 0, end = 0; // seconds, to 0.1
            double confidence = 0;     // 0..1, to 0.01
            double level = 0;          // dB, to 0.1 (the song's 60th-percentile frame level)
        };

        // The stretch before, between or after the songs, and what it mostly was
        struct Gap
        {
            double start = 0, end = 0;
            Kind kind = Kind::quiet;
            std::vector<std::pair<Kind, int>> counts;   // seconds of each kind, in the order first met
        };

        struct Set
        {
            std::vector<Song> songs;
            std::vector<Gap> gaps;
            int seconds = 0;
            std::vector<Kind> timeline;    // each whole second's kind
            bool judged = false;           // false: under 5 s, nothing judged (threshold and peak unset)
            double threshold = 0;          // the music score's split
            double peak = 0;               // the mid's largest sample (frames read so far)
        };

        struct Options
        {
            double minSongSec = 40;    // shorter music runs are tuning or noodling
            double mergeGapSec = 6;    // a quieter break inside a song is shorter than this
        };

        // Otsu's method: the value that best splits a set of numbers into two groups
        // (64 bins between their least and greatest; the greatest when they're all
        // one value, and for none).
        inline double otsu (const std::vector<double>& values)
        {
            double lo = std::numeric_limits<double>::infinity(), hi = -lo;
            for (double v : values) { lo = std::min (lo, v); hi = std::max (hi, v); }
            const int bins = 64;
            if (hi - lo < 1e-6) return hi;
            std::vector<double> hist ((size_t) bins, 0.0);
            for (double v : values) hist[(size_t) std::min (bins - 1, (int) std::floor (((v - lo) / (hi - lo)) * bins))] += 1;
            const double total = (double) values.size();
            double sumAll = 0;
            for (int i = 0; i < bins; ++i) sumAll += i * hist[(size_t) i];
            double sumB = 0, wB = 0, bestVar = -1;
            int best = 0;
            for (int i = 0; i < bins; ++i)
            {
                wB += hist[(size_t) i];
                if (wB == 0) continue;
                const double wF = total - wB;
                if (wF == 0) break;
                sumB += i * hist[(size_t) i];
                const double mB = sumB / wB, mF = (sumAll - sumB) / wF, between = wB * wF * (mB - mF) * (mB - mF);
                if (between > bestVar) { bestVar = between; best = i; }
            }
            return lo + ((best + 0.5) / bins) * (hi - lo);
        }

        // The median of each value's neighbourhood (half either side, kept inside the
        // array; the upper middle of an even count), stored as float
        inline std::vector<float> medianFilter (const std::vector<double>& x, int half)
        {
            const int n = (int) x.size();
            std::vector<float> out ((size_t) n);
            std::vector<double> w;
            for (int i = 0; i < n; ++i)
            {
                w.assign (x.begin() + std::max (0, i - half), x.begin() + std::min (n, i + half + 1));
                out[(size_t) i] = (float) stats::medianInPlace (w.data(), (int) w.size());
            }
            return out;
        }
    }

    class LiveSetAnalyzer
    {
    public:
        static constexpr double hopSec = 0.1;

        explicit LiveSetAnalyzer (double sampleRate)
            : fs (sampleRate),
              hop ((int) std::floor (sampleRate * hopSec + 0.5)),
              low ({ BiquadCoefs::rbj (BiquadCoefs::Type::lowPass, sampleRate, 150, 0.7071), BiquadCoefs::rbj (BiquadCoefs::Type::lowPass, sampleRate, 150, 0.7071) }, 1),
              mid ({ BiquadCoefs::rbj (BiquadCoefs::Type::bandPass, sampleRate, 1500, 0.5), BiquadCoefs::rbj (BiquadCoefs::Type::bandPass, sampleRate, 1500, 0.5) }, 1),
              high ({ BiquadCoefs::rbj (BiquadCoefs::Type::highPass, sampleRate, 5000, 0.7071), BiquadCoefs::rbj (BiquadCoefs::Type::highPass, sampleRate, 5000, 0.7071) }, 1),
              dec (std::max (1, (int) std::floor (sampleRate / 11025.0 + 0.5))),
              decRate (sampleRate / dec),
              decFilter ({ BiquadCoefs::rbj (BiquadCoefs::Type::lowPass, sampleRate, 0.4 * (sampleRate / dec), 0.7071),
                           BiquadCoefs::rbj (BiquadCoefs::Type::lowPass, sampleRate, 0.4 * (sampleRate / dec), 0.7071) }, 1),
              ring ((size_t) N, 0.0f),
              win (windows::hannSymmetric (N)),
              re ((size_t) N), im ((size_t) N),
              binLo ((int) std::ceil ((150.0 / decRate) * N)),
              binHi ((int) std::floor ((5000.0 / decRate) * N))
        {
        }

        // The next block of the set (r null: one channel)
        void push (const float* l, const float* r, int n)
        {
            m.resize ((size_t) n); lo.resize ((size_t) n); mi.resize ((size_t) n); hi.resize ((size_t) n); dm.resize ((size_t) n);
            for (int i = 0; i < n; ++i) m[(size_t) i] = midOf ((double) l[i], (double) (r != nullptr ? r[i] : l[i]));
            lo = m; mi = m; hi = m; dm = m;
            low.process (0, lo.data(), n);
            mid.process (0, mi.data(), n);
            high.process (0, hi.data(), n);
            decFilter.process (0, dm.data(), n);
            for (int i = 0; i < n; ++i)
            {
                const double x = m[(size_t) i];
                acc[0] += x * x; acc[1] += lo[(size_t) i] * lo[(size_t) i]; acc[2] += mi[(size_t) i] * mi[(size_t) i]; acc[3] += hi[(size_t) i] * hi[(size_t) i];
                absAcc = std::max (absAcc, std::abs (x));
                if (++decCount >= dec) { decCount = 0; ring[(size_t) ringPos] = (float) dm[(size_t) i]; ringPos = (ringPos + 1) % N; }
                if (++inHop >= hop)
                {
                    const double k = inHop;
                    frames.push_back ({ acc[0] / k, acc[1] / k, acc[2] / k, acc[3] / k, flatness() });
                    peak = std::max (peak, absAcc);
                    acc[0] = acc[1] = acc[2] = acc[3] = 0; absAcc = 0; inHop = 0;
                }
            }
        }

        // The songs and what's between them, from everything pushed
        liveset::Set finish (const liveset::Options& opt = {}) const
        {
            using namespace liveset;
            Set out;
            const int nf = (int) frames.size();
            const int seconds = (int) std::floor (nf * hopSec);
            out.seconds = seconds;
            if (seconds < 5) return out;
            out.judged = true;
            out.peak = peak;
            const int per = (int) std::floor (1.0 / hopSec + 0.5);
            const auto db = [] (double p) { return db10 (p, 1e-12); };
            const auto clamp01 = [] (double v) { return std::max (0.0, std::min (1.0, v)); };

            // frame tonality (log10 flatness), lightly smoothed, and level
            std::vector<double> logFlat ((size_t) nf), frameDb ((size_t) nf);
            for (int k = 0; k < nf; ++k) { logFlat[(size_t) k] = std::log10 (frames[(size_t) k].flat + 1e-9); frameDb[(size_t) k] = db (frames[(size_t) k].total); }
            const auto frameFlat = medianFilter (logFlat, 2);

            // ---- per-second features
            struct Sec { double level, lowRatio, midRatio, highRatio, wobble, flat; };
            std::vector<Sec> sec;
            for (int s = 0; s < seconds; ++s)
            {
                const int a = s * per, b = std::min (nf, (s + 1) * per), cnt = b - a;
                double tot = 0, l = 0, md = 0, h = 0;
                for (int k = a; k < b; ++k) { tot += frames[(size_t) k].total; l += frames[(size_t) k].low; md += frames[(size_t) k].mid; h += frames[(size_t) k].high; }
                tot /= cnt; l /= cnt; md /= cnt; h /= cnt;
                double meanLv = 0;
                for (int k = a; k < b; ++k) meanLv += db (frames[(size_t) k].total);
                meanLv /= cnt;
                double var = 0;
                for (int k = a; k < b; ++k) { const double d = db (frames[(size_t) k].total) - meanLv; var += d * d; }
                std::vector<double> fl (frameFlat.begin() + a, frameFlat.begin() + b);
                sec.push_back ({ db (tot), db (l) - db (tot), db (md) - db (tot), db (h) - db (tot), std::sqrt (var / cnt),
                                 fl.empty() ? 0.0 : stats::medianInPlace (fl.data(), (int) fl.size()) });
            }

            // ---- thresholds for this recording
            std::vector<double> levels;
            for (const auto& s : sec) levels.push_back (s.level);
            const double loud = stats::percentile (levels.data(), (int) levels.size(), 0.95);
            // quiet: well below the loud parts of this set (a wall-to-wall punk set has no quiet percentile to lean on)
            std::vector<bool> isQuiet;
            std::vector<double> audFlat, audWobble, audLow;
            for (const auto& s : sec)
            {
                const bool q = s.level < std::max (loud - 30, -65.0);
                isQuiet.push_back (q);
                if (! q) { audFlat.push_back (s.flat); audWobble.push_back (s.wobble); audLow.push_back (s.lowRatio); }
            }
            const double tonalThr = otsu (audFlat);    // tonal (music) against noisy (applause, breath, room)
            const double wobRef = std::max (0.5, audWobble.empty() ? 0.0 : stats::medianInPlace (audWobble.data(), (int) audWobble.size()));
            // loudness against the surrounding 90 s, not the whole set, so a quiet song between loud ones still counts
            std::vector<double> localMax ((size_t) seconds);
            for (int i = 0; i < seconds; ++i)
            {
                double mx = -std::numeric_limits<double>::infinity();
                for (int j = std::max (0, i - 90); j < std::min (seconds, i + 91); ++j) mx = std::max (mx, levels[(size_t) j]);
                localMax[(size_t) i] = mx;
            }
            std::vector<double> score ((size_t) seconds, 0.0);
            for (int i = 0; i < seconds; ++i)
            {
                if (isQuiet[(size_t) i]) continue;
                const auto& s = sec[(size_t) i];
                const double tonal = clamp01 ((tonalThr - s.flat) / 0.6 + 0.5);
                const double steady = clamp01 (1 - s.wobble / (2.5 * wobRef));
                const double present = clamp01 ((s.level - (localMax[(size_t) i] - 24)) / 24);
                score[(size_t) i] = 1.0 * tonal + 0.45 * steady + 0.45 * present;
            }
            const auto smooth = medianFilter (score, 2);
            std::vector<double> audibleScore;
            for (int i = 0; i < seconds; ++i) if (! isQuiet[(size_t) i]) audibleScore.push_back (smooth[(size_t) i]);
            const double thr = otsu (audibleScore);
            out.threshold = thr;
            std::vector<bool> isMusic ((size_t) seconds), isApplause ((size_t) seconds);
            for (int i = 0; i < seconds; ++i) isMusic[(size_t) i] = ! isQuiet[(size_t) i] && smooth[(size_t) i] > thr;
            // applause: audible, clearly noisy, little low end
            const double lowRef = stats::percentile (audLow.data(), (int) audLow.size(), 0.6);
            for (int i = 0; i < seconds; ++i)
                isApplause[(size_t) i] = ! isQuiet[(size_t) i] && ! isMusic[(size_t) i] && sec[(size_t) i].flat > tonalThr + 0.1 && sec[(size_t) i].lowRatio < lowRef;

            // ---- runs become songs: a quieter break inside a song joins it again; applause never does
            struct Run { int start, end; };
            std::vector<Run> runs;
            for (int s = 0; s < seconds; ++s)
            {
                if (! isMusic[(size_t) s]) continue;
                if (! runs.empty())
                {
                    auto& last = runs.back();
                    int clapping = 0;
                    for (int x = last.end; x < s; ++x) clapping += isApplause[(size_t) x] ? 1 : 0;
                    if (s - last.end <= opt.mergeGapSec && clapping < 2) { last.end = s + 1; continue; }
                }
                runs.push_back ({ s, s + 1 });
            }
            runs.erase (std::remove_if (runs.begin(), runs.end(), [&] (const Run& r) { return r.end - r.start < opt.minSongSec; }), runs.end());

            // edges to 0.1 s: a song starts where tonal sound comes in, and ends where it stops (ring-out kept, applause not)
            const auto to = [] (double v, double m) { return std::round (v * m) / m; };
            for (size_t i = 0; i < runs.size(); ++i)
            {
                const auto& r = runs[i];
                std::vector<double> body (frameDb.begin() + r.start * per, frameDb.begin() + std::min (nf, r.end * per));
                const double songLevel = stats::percentile (body.data(), (int) body.size(), 0.6);
                const auto tonalFrame = [&] (int k) { return frameFlat[(size_t) k] < tonalThr && frameDb[(size_t) k] > songLevel - 22; };
                int a = r.start * per;
                for (int k = a; k >= std::max (0, a - 6 * per); --k) { if (! tonalFrame (k) && frameDb[(size_t) k] < songLevel - 18) break; a = k; }
                int b = std::max (a, (r.end - 1) * per);
                for (int k = b; k < std::min (nf, b + 12 * per); ++k) { if (tonalFrame (k)) b = k; else if (k - b > 8) break; }
                const double startSec = std::max (0.0, a * hopSec - 0.3);
                const double endSec = std::min (nf * hopSec, b * hopSec + 0.6);
                double sum = 0;
                for (int x = r.start; x < r.end; ++x) sum += smooth[(size_t) x];
                const double conf = clamp01 ((sum / (r.end - r.start) - thr) / 0.5);
                out.songs.push_back ({ (int) i + 1, to (startSec, 10), to (endSec, 10), to (conf, 100), to (songLevel, 10) });
            }

            // ---- what's between the songs
            const auto kindOf = [&] (int s)
            {
                if (isQuiet[(size_t) s]) return Kind::quiet;
                if (isMusic[(size_t) s]) return Kind::music;
                if (isApplause[(size_t) s]) return Kind::applause;
                if (sec[(size_t) s].wobble > wobRef) return Kind::talk;
                return Kind::noodling;
            };
            std::vector<double> edges { 0.0 };
            for (const auto& s : out.songs) { edges.push_back (s.start); edges.push_back (s.end); }
            edges.push_back ((double) seconds);
            for (size_t i = 0; i + 1 < edges.size(); i += 2)
            {
                const double a = edges[i], b = edges[i + 1];
                if (b - a < 1) continue;
                Gap g { a, b, Kind::quiet, {} };
                for (int x = (int) std::floor (a); x < std::min (seconds, (int) std::ceil (b)); ++x)
                {
                    const Kind k = kindOf (x);
                    auto it = std::find_if (g.counts.begin(), g.counts.end(), [k] (const auto& c) { return c.first == k; });
                    if (it == g.counts.end()) g.counts.push_back ({ k, 1 }); else ++it->second;
                }
                int most = 0;
                for (const auto& c : g.counts) if (c.second > most) { most = c.second; g.kind = c.first; }
                out.gaps.push_back (g);
            }
            for (int s = 0; s < seconds; ++s) out.timeline.push_back (kindOf (s));
            return out;
        }

    private:
        struct Frame { double total, low, mid, high, flat; };
        static constexpr int N = 1024;

        double fs;
        int hop;
        BiquadChain low, mid, high;
        int dec;
        double decRate;
        BiquadChain decFilter;
        std::vector<float> ring;
        int ringPos = 0, decCount = 0, inHop = 0;
        std::vector<double> win, re, im;
        int binLo, binHi;
        double acc[4] {};
        double absAcc = 0, peak = 0;
        std::vector<double> m, lo, mi, hi, dm;
        std::vector<Frame> frames;

        // The spectral flatness of the last N decimated samples: near 0 for tones and
        // chords, near 1 for noise (applause), between for talk
        double flatness()
        {
            for (int i = 0; i < N; ++i) { re[(size_t) i] = ring[(size_t) ((ringPos + i) % N)] * win[(size_t) i]; im[(size_t) i] = 0; }
            fftFor (N).forward (re.data(), im.data());
            double logSum = 0, sum = 0;
            int n = 0;
            for (int k = binLo; k <= binHi; ++k)
            {
                const double p = re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k] + 1e-12;
                logSum += std::log (p); sum += p; ++n;
            }
            return std::exp (logSum / n) / (sum / n);
        }
    };
}
