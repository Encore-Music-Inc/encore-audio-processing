#pragma once

// De-click and de-crackle for transfers of records (Restore's).
//
// How it hears a click: music is predictable from the last few milliseconds
// (an autoregressive model: each sample is close to a weighted sum of the ones
// before it). A click isn't, so where the prediction error jumps far above its
// usual level for a moment, something was added. The error is checked forwards
// and backwards and the smaller of the two kept, which pins the click down to
// the samples it actually hit.
//
// How it leaves the music alone: a drum hit or an accordion attack is also
// unpredictable, but it stays busy afterwards (the snare rattles on, the reed
// keeps sounding). A click is over in a few milliseconds and the music goes on
// as before. So anything followed by more activity than came before it, or
// that runs longer than a click can, is left untouched.
//
// How it repairs: the damaged samples are thrown away and rebuilt from the
// music on both sides (lpc::gapFill), not muted or smoothed.
//
// Streaming, two channels: push() takes the next block and returns how many
// samples per channel are ready (output (c)), flush() after the last push
// gives the rest. The output is the input, repaired, with no delay.

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include "Lpc.h"
#include "Stats.h"

namespace vs
{
    namespace declick
    {
        // An amount (0..100, 0 off) to how far above its usual level the
        // prediction error must jump to be a click or crackle. Calibrated on
        // polka with hiss: the defaults find nearly every click without
        // touching drum hits.
        inline double clickThreshold (double amount)
        {
            return amount > 0 ? 8.0 + 16.0 * std::pow (1.0 - std::min (100.0, amount) / 100.0, 1.5) : std::numeric_limits<double>::infinity();
        }
        inline double crackleThreshold (double amount)
        {
            return amount > 0 ? 6.0 + 10.0 * std::pow (1.0 - std::min (100.0, amount) / 100.0, 1.5) : std::numeric_limits<double>::infinity();
        }

        inline constexpr double pi = 3.14159265358979323846;

        enum class Kind { click, crackle, longClick };

        // One thing it found: where it starts (input sample), how many samples,
        // which channel (0 or 1).
        struct Event { long at = 0; int len = 0; int ch = 0; Kind kind = Kind::click; };

        struct Settings
        {
            double clicks = 50.0, crackle = 40.0;   // 0..100, 0 off
            double maxMs = 4.0;                     // the longest click it rebuilds
            bool linked = false;                    // both channels carry the same audio (a mono record): repair once
            bool detectOnly = false;                // find and count, change nothing
        };

        inline const char* kindName (Kind k) { return k == Kind::click ? "click" : k == Kind::crackle ? "crackle" : "long"; }
    }

    class Declicker
    {
    public:
        Declicker (double sampleRate, const declick::Settings& s = {})
            : fs (sampleRate), linked (s.linked), detectOnly (s.detectOnly)
        {
            p = std::max (20, std::min (64, (int) std::round (32.0 * fs / 48000.0)));   // about 0.7 ms of memory
            A = (int) std::round (0.01 * fs);                                         // fresh models every 10 ms
            B = 8 * A;                                                                // the block handled at a time
            S = std::max (16, (int) std::round (fs / 1500.0));                       // 0.7 ms steps for the usual level
            Kc = declick::clickThreshold (s.clicks);
            Kk = declick::crackleThreshold (s.crackle);
            maxLen = std::max (4, (int) std::round (s.maxMs / 1000.0 * fs));
            crackleMax = std::max (3, (int) std::round (0.0004 * fs));
            E = maxLen + 10 * S;                                                      // the error is worked out this far past the block
            M = std::max (4 * A, E + p) + 64;                                         // context kept either side of a block
            win.resize ((size_t) (2 * A));
            for (int i = 0; i < 2 * A; ++i) win[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * declick::pi * (i + 0.5) / (2.0 * A));
            for (auto& b : buf) b.assign ((size_t) M, 0.0f);
            base = -M;
            off = std::isinf (Kc) && std::isinf (Kk);
        }

        // The next n samples of each channel (chans[1] may be null: channel 0 is used for both).
        int push (const float* const* chans, int n)
        {
            total += n;
            for (int c = 0; c < 2; ++c)
            {
                const float* x = chans[c] != nullptr ? chans[c] : chans[0];
                buf[c].insert (buf[c].end(), x, x + n);
            }
            return drain (false);
        }

        // The rest, after the last push.
        int flush()
        {
            for (auto& b : buf) b.resize (b.size() + (size_t) (M + B), 0.0f);
            return drain (true);
        }

        // The samples push() or flush() just made, channel 0 or 1.
        const std::vector<float>& output (int c) const { return out[c]; }

        const std::vector<declick::Event>& events() const { return found; }
        long repairedSamples() const { return repaired; }
        int modelOrder() const { return p; }

    private:
        using Model = std::vector<double>;
        struct Region { long a, e; declick::Kind kind; double sig; };
        struct Cluster { long a, e; std::vector<std::pair<long, long>> spans; long n; double sig; std::vector<long> idx; };

        double fs;
        bool linked, detectOnly, off = false;
        int p, A, B, S, maxLen, crackleMax, E, M;
        double Kc, Kk;
        std::vector<double> win, longWin;
        std::vector<float> buf[2], out[2];
        long base, next = 0, total = 0, repaired = 0;
        std::vector<declick::Event> found;

        int drain (bool final)
        {
            out[0].clear();
            out[1].clear();
            while (final ? next < total : base + (long) buf[0].size() >= next + B + M)
            {
                const long s = next, o = s - base;
                if (! off)
                {
                    block (0, s);
                    if (linked) std::copy (buf[0].begin() + (o - M), buf[0].begin() + (o + B + M), buf[1].begin() + (o - M));
                    else block (1, s);
                }
                const long len = final ? std::min<long> (B, total - s) : B;
                for (int c = 0; c < 2; ++c) out[c].insert (out[c].end(), buf[c].begin() + o, buf[c].begin() + o + len);
                next += B;
            }
            // keep the context the next block needs (including repairs that ran past this block)
            const long keep = next - M - base;
            if (keep > 0)
            {
                for (auto& b : buf) b.erase (b.begin(), b.begin() + keep);
                base += keep;
            }
            return (int) out[0].size();
        }

        Model fit (const std::vector<double>& seg, long from, int n, const std::vector<double>& w, int order) const
        {
            std::vector<double> r ((size_t) order + 1);
            Model a ((size_t) order + 1);
            lpc::windowedAutocorrelation (seg.data(), (long) seg.size(), from, n, w.data(), order, r.data());
            lpc::predictor (r.data(), order, a.data());
            return a;
        }

        // Two models for each of the block's 10 ms frames (plus two either side):
        // one learned from the 20 ms just before the frame, one from the 20 ms just
        // after. Neither has heard the frame itself, so a click can't teach the
        // model to expect it, and one of the two follows the music even where a new
        // note or drum hit starts.
        std::vector<std::pair<Model, Model>> models (const std::vector<double>& seg) const
        {
            std::vector<std::pair<Model, Model>> m;
            for (int j = -2; j <= 9; ++j)
            {
                const long s = M + (long) j * A;
                m.push_back ({ fit (seg, s - 2 * A, 2 * A, win, p), fit (seg, s + A, 2 * A, win, p) });
            }
            return m;
        }

        int frameOf (long n) const
        {
            const long f = (long) std::floor ((double) (n - M) / A);
            return (int) std::max (-2L, std::min (9L, f)) + 2;
        }

        // Prediction errors over [from, to): forwards (from the past) and
        // backwards (from the future), each the smaller of the two models' errors.
        void errors (const std::vector<double>& seg, const std::vector<std::pair<Model, Model>>& m, long from, long to,
                     std::vector<double>& ef, std::vector<double>& eb) const
        {
            ef.assign ((size_t) (to - from), 0.0);
            eb.assign ((size_t) (to - from), 0.0);
            for (long n = from; n < to; ++n)
            {
                const auto& pair = m[(size_t) frameOf (n)];
                double bf = std::numeric_limits<double>::infinity(), bb = bf;
                for (int q = 0; q < 2; ++q)
                {
                    const Model& a = q == 0 ? pair.first : pair.second;
                    double f = seg[(size_t) n], b = seg[(size_t) n];
                    for (int k = 1; k <= p; ++k)
                    {
                        f -= a[(size_t) k] * seg[(size_t) (n - k)];
                        b -= a[(size_t) k] * seg[(size_t) (n + k)];
                    }
                    f = std::abs (f);
                    b = std::abs (b);
                    if (f < bf) bf = f;
                    if (b < bb) bb = b;
                }
                ef[(size_t) (n - from)] = bf;
                eb[(size_t) (n - from)] = bb;
            }
        }

        // The clicks starting in [M, M + B) of seg, in seg indices. A click throws
        // the forward error off from its first sample (and for a while after),
        // the backward error up to its last sample (and for a while before). So an
        // event is a run where either is high; it starts where the forward error
        // first jumps and ends where the backward error last does.
        std::vector<Region> detect (const std::vector<double>& seg, const std::vector<std::pair<Model, Model>>& m) const
        {
            const long from = M - E, to = M + B + E;
            std::vector<double> ef, eb;
            errors (seg, m, from, to, ef, eb);
            const int len = (int) ef.size();
            std::vector<double> u ((size_t) len);
            for (int i = 0; i < len; ++i) u[(size_t) i] = ef[(size_t) i] > eb[(size_t) i] ? ef[(size_t) i] : eb[(size_t) i];
            // the usual level: the median error in each 0.7 ms step, then the median
            // of the six steps just before (so the click itself never raises it)
            const int nSub = (len + S - 1) / S;
            std::vector<double> med ((size_t) nSub), tmp ((size_t) S), level ((size_t) nSub);
            for (int j = 0; j < nSub; ++j)
            {
                const int a = j * S, b = std::min (len, a + S);
                for (int i = a; i < b; ++i) tmp[(size_t) (i - a)] = u[(size_t) i];
                med[(size_t) j] = stats::medianInPlace (tmp.data(), b - a);
            }
            double six[6];
            for (int j = 0; j < nSub; ++j)
            {
                const int lo = std::max (0, j - 6), n = j - lo;
                if (n < 2) { level[(size_t) j] = std::max (med[(size_t) j], 1e-7); continue; }
                for (int q = 0; q < n; ++q) six[q] = med[(size_t) (lo + q)];
                level[(size_t) j] = std::max (stats::medianInPlace (six, n), 1e-7);
            }
            const double Kmin = std::min (Kc, Kk);
            std::vector<Region> regions;
            const int merge = std::max (2, (int) std::round (p / 4.0)), afterLen = (int) std::round (0.008 * fs);
            const int span = maxLen + 2 * p + merge;
            for (long n = M; n < M + B; ++n)
            {
                const long i = n - from;
                const double sig = level[(size_t) (i / S)];
                if (u[(size_t) i] <= Kmin * sig) continue;
                // the run where either error stays well above the usual level
                const double hi = 4 * sig;
                long end = i;
                for (long q = i + 1; q < len && q - i <= span; ++q)
                {
                    if (u[(size_t) q] > hi) end = q;
                    else if (q - end > merge) break;
                }
                // its edges: first forward jump, last backward jump. A click that
                // jumps and then dies away smoothly into the music leaves no backward
                // jump at its end; then the forward error's last jump (less its own
                // short echo) marks it.
                long a = -1, eb1 = -1, ef1 = -1;
                double peak = 0.0;
                for (long q = i; q <= end; ++q)
                {
                    if (ef[(size_t) q] > hi) { if (a < 0) a = q; ef1 = q; }
                    if (eb[(size_t) q] > hi) eb1 = q;
                }
                n = end + from;   // carry on after it
                if (a < 0) continue;
                for (long q = a; q <= std::max ({ a, eb1, ef1 }); ++q) if (u[(size_t) q] > peak) peak = u[(size_t) q];
                const double ratio = peak / sig;
                const bool big = ratio >= Kc;
                long e = big ? std::max (eb1, ef1 - (p >> 2)) : eb1;
                if (e < a) { if (! big) continue; e = a; }
                const long clen = e - a + 1;
                // busier afterwards than before (in most of the next 8 ms): a drum hit
                // or a new note starting, not a click. A click that close before a
                // loud hit is masked by it anyway. (Most of, so another click just
                // after doesn't count.)
                const long j0 = (long) std::floor ((double) (end + merge) / S) + 1;
                const long j1 = std::min<long> (nSub - 1, (long) std::floor ((double) (end + afterLen) / S));
                double after = 0.0;
                if (j1 >= j0)
                {
                    std::vector<double> t (med.begin() + j0, med.begin() + j1 + 1);
                    std::sort (t.begin(), t.end());
                    after = t[(size_t) std::floor (0.7 * (double) (t.size() - 1))] / sig;
                }
                bool keep = false;
                declick::Kind kind = declick::Kind::click;
                if (after > 3) keep = false;
                else if (clen > maxLen) { keep = ratio >= Kc; kind = declick::Kind::longClick; }
                else if (big) { keep = true; kind = declick::Kind::click; }
                else if (ratio >= Kk && clen <= crackleMax) { keep = true; kind = declick::Kind::crackle; }
                if (keep) regions.push_back ({ a + from, e + from, kind, sig });
            }
            return regions;
        }

        // The samples to rebuild for each region, grouped where they're close
        // enough to solve together.
        std::vector<Cluster> clusters (const std::vector<Region>& regions, long segLen) const
        {
            const int padClick = std::max (2, (int) std::round (0.00025 * fs)), padCrackle = std::max (1, (int) std::round (0.00006 * fs));
            std::vector<Cluster> outC;
            for (const auto& r : regions)
            {
                if (r.kind == declick::Kind::longClick) continue;
                const int pad = r.kind == declick::Kind::click ? padClick : padCrackle;
                const long a = std::max<long> (p, r.a - pad), e = std::min<long> (segLen - p - 1, r.e + pad);
                if (! outC.empty() && a - outC.back().e <= p && outC.back().n + (e - a + 1) <= 3 * maxLen)
                {
                    auto& last = outC.back();
                    last.e = std::max (last.e, e);
                    last.spans.push_back ({ a, e });
                    last.n += e - a + 1;
                    last.sig = std::max (last.sig, r.sig);
                }
                else outC.push_back ({ a, e, { { a, e } }, e - a + 1, r.sig, {} });
            }
            for (auto& c : outC)
            {
                std::vector<char> mark ((size_t) (c.e - c.a + 1), 0);
                for (const auto& [a, e] : c.spans) std::fill (mark.begin() + (a - c.a), mark.begin() + (e - c.a + 1), 1);
                for (size_t i = 0; i < mark.size(); ++i) if (mark[i]) c.idx.push_back (c.a + (long) i);
            }
            return outC;
        }

        void fill (std::vector<double>& seg, const std::vector<Cluster>& cl, const std::vector<std::pair<Model, Model>>& m) const
        {
            for (const auto& c : cl)
                lpc::gapFill (seg.data(), (long) seg.size(), c.idx, m[(size_t) frameOf ((c.a + c.e) >> 1)].first.data(), p);
        }

        void block (int ch, long s)
        {
            auto& b = buf[ch];
            const long o = s - base;
            std::vector<double> seg (b.begin() + (o - M), b.begin() + (o + B + M));
            auto m = models (seg);
            auto regions = detect (seg, m);
            if (regions.empty()) return;
            // a loud click nearby can still pull a neighbouring frame's model towards
            // itself: rebuild a copy with what was found, model that, and look again
            auto trial = seg;
            fill (trial, clusters (regions, (long) seg.size()), m);
            m = models (trial);
            const auto first = regions;
            regions = detect (seg, m);
            for (const auto& r : first)
            {
                bool overlaps = false;
                for (const auto& q : regions) if (q.a <= r.e && q.e >= r.a) { overlaps = true; break; }
                if (! overlaps) regions.push_back (r);
            }
            std::stable_sort (regions.begin(), regions.end(), [] (const Region& x, const Region& y) { return x.a < y.a; });
            if (detectOnly)
            {
                for (const auto& r : regions) found.push_back ({ s - M + r.a, (int) (r.e - r.a + 1), ch, r.kind });
                return;
            }
            const auto cl = clusters (regions, (long) seg.size());
            fill (seg, cl, m);
            // bigger repairs: model the repaired surroundings once more and rebuild,
            // with a longer memory for longer gaps (an order of about three times
            // the gap rebuilds rich harmonic music noticeably better)
            for (const auto& c : cl)
            {
                if (c.n <= crackleMax * 2) continue;
                const int q = (int) std::max<long> (p, std::min<long> (200, 3 * c.n));
                const int W = (int) std::round (0.06 * fs);
                const long centre = (c.a + c.e) >> 1;
                const long from = std::max<long> (q, std::min<long> ((long) seg.size() - W - q, centre - (W >> 1)));
                if ((int) longWin.size() != W)
                {
                    longWin.resize ((size_t) W);
                    for (int i = 0; i < W; ++i) longWin[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * declick::pi * (i + 0.5) / W);
                }
                if (c.a - q < 0 || c.e + q >= (long) seg.size()) continue;
                const auto a = fit (seg, from, W, longWin, q);
                lpc::gapFill (seg.data(), (long) seg.size(), c.idx, a.data(), q);
            }
            // what was rebuilt, for the timeline (after any extensions)
            for (const auto& r : regions)
            {
                const Cluster* c = nullptr;
                for (const auto& q : cl) if (q.a <= r.e && q.e >= r.a) { c = &q; break; }
                const long e = c != nullptr && r.kind == declick::Kind::click ? std::max (r.e, c->e) : r.e;
                found.push_back ({ s - M + r.a, (int) (e - r.a + 1), ch, r.kind });
            }
            for (const auto& c : cl)
            {
                repaired += (long) c.idx.size();
                for (long t : c.idx) b[(size_t) (o - M + t)] = (float) seg[(size_t) t];
            }
        }
    };
}
