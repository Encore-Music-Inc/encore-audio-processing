#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include "Biquad.h"
#include "Loudness.h"
#include "LoudnessReport.h"
#include "Spectrum.h"
#include "TruePeak.h"

namespace vs
{
    // Everything a release check measures on a finished song, in one place so
    // every product reads a file the same way: peaks, DC, overs, channel
    // correlation and mono loss, loudness (BS.1770 integrated, short-term,
    // range, balance), true peak and the seconds over -1 and 0 dBTP,
    // flat-topped (clipped) peaks, mono compatibility per frequency band and
    // the seconds where the low end is out of phase, silence and levels at the
    // start and the end, the high-frequency cliff a lossy encoder leaves, and
    // whether a 24-bit file only holds 16-bit audio.
    //
    // The first two channels are measured (one channel: it is both sides of
    // the stereo figures, and its loudness is measured as one channel).
    // dB values are dBFS unless named otherwise; times are in seconds.
    namespace analysis
    {
        struct ClipStats
        {
            int runs = 0, events = 0, longest = 0;
            bool hasLevel = false;
            double levelDb = 0.0;          // when hasLevel
            std::vector<double> times;     // the first 40 events' starts
        };

        struct BandMeasure
        {
            double corr = 1.0, monoLossDb = 0.0, shareDb = 0.0;
        };

        struct LowEnd
        {
            double corr = 1.0, monoLossDb = 0.0, shareDb = 0.0;
            std::vector<int> outTimes;     // seconds where under 120 Hz is out of phase
        };

        // The bands mono compatibility is measured in (Hz); hi 0: no upper
        // edge. The first three are measured on the decimated copy.
        struct BandSpec { double lo, hi; bool low; };
        inline constexpr BandSpec bands[5] {
            { 20, 60, true }, { 60, 120, true }, { 120, 250, true }, { 250, 2000, false }, { 2000, 0, false } };
        inline constexpr int numBands = 5;

        struct Measurements
        {
            double fs = 48000.0;
            int channels = 0;
            std::int64_t frames = 0;
            double duration = 0.0;
            bool empty = true;

            double samplePeakDb = 0, samplePeakAt = 0, peakLDb = 0, peakRDb = 0, dcL = 0, dcR = 0;
            double dcLDb = -240, dcRDb = -240;    // each channel's DC offset in dBFS: 20 log10 (|dc| + 1e-12)
            int oversFloat = 0;                // samples over full scale
            double corr = 1, monoLossDb = 0, sideToMidDb = 0;
            bool identical = false;
            double maxDiff = 0;
            bool silentL = false, silentR = false, silent = false;

            double lufs = -100, shortTermMax = -100, shortTermMaxAt = 0, lra = 0, balanceDb = 0;
            std::vector<double> timeline;

            double truePeakDb = 0, truePeakAt = 0;
            std::vector<int> tpOvers, tpOverZero;    // the first 40 seconds over -1 dBTP, and over 0
            int tpOverCount = 0, tpOverZeroCount = 0;

            ClipStats clip;
            BandMeasure band[numBands];
            LowEnd low;

            double leadSec = 0, tailSec = 0, firstSampleDb = 0, lastSampleDb = 0, headRmsDb = 0, endRmsDb = 0, endDecayDb = 0;

            bool hasSpectrum = false;          // false: too short to judge
            SpectrumCliff spectrum;

            bool hasPadded16 = false;          // measured only for integer files deeper than 16-bit
            bool padded16 = false;
        };

        struct Options
        {
            int bitDepth = 0;                  // 0: unknown
            bool isFloat = false;
        };
    }

    // Flat-topped peaks (clipping) in one channel: runs of 3 or more samples
    // in a row at the channel's own top level (within tol of its peak, either
    // sign), reached steeply (the samples either side more than 4 tol below
    // it). The channel's peak must be known before (a first pass over it).
    // Runs starting within 50 ms of the last one's start join its event.
    class FlatRuns
    {
    public:
        struct Event { double start, end; int n; };

        // How close to the peak counts as at it: 1.5 steps of an integer
        // format (at least 2e-6), 1e-5 for float or unknown depth (0).
        static double tolerance (int bitDepth, bool isFloat)
        {
            const double bits = isFloat ? 0 : bitDepth;
            return bits > 0 ? std::max (1.5 / std::pow (2.0, bits - 1), 2e-6) : 1e-5;
        }

        void prepare (float channelPeak, double tol, double sampleRate)
        {
            *this = {};
            fs = sampleRate;
            tolerance_ = tol;
            level = channelPeak;
            thr = (double) channelPeak - tol;
        }

        // The next count samples of the channel
        void process (const float* x, int count)
        {
            for (int i = 0; i < count; ++i) sample (x[i]);
        }

        // After the last sample: a run reaching the end closes there.
        void finish() { if (fed) closeRun (at, 0.0); }

        int runs() const { return runCount; }
        int longest() const { return longestRun; }
        float peak() const { return level; }
        const std::vector<Event>& events() const { return ev; }

        // Events of several channels as one list, in time order, joined
        // when one starts within `gap` seconds of the last one's end.
        static std::vector<Event> merge (std::vector<Event> all, double gap = 0.05)
        {
            std::stable_sort (all.begin(), all.end(), [] (const Event& a, const Event& b) { return a.start < b.start; });
            std::vector<Event> merged;
            for (const auto& e : all)
            {
                if (! merged.empty() && e.start - merged.back().end <= gap)
                {
                    auto& lst = merged.back();
                    lst.end = std::max (lst.end, e.end);
                    lst.n += e.n;
                }
                else merged.push_back (e);
            }
            return merged;
        }

    private:
        void closeRun (std::int64_t i, double after)
        {
            if (sign != 0 && i - start >= 3)
            {
                if (level - beforeStart > 4 * tolerance_ && level - after > 4 * tolerance_)
                {
                    ++runCount;
                    longestRun = std::max (longestRun, (int) (i - start));
                    const double t = (double) start / fs;
                    if (! ev.empty() && t - ev.back().end <= 0.05) { ev.back().end = t; ++ev.back().n; }
                    else ev.push_back ({ t, t, 1 });
                }
            }
        }

        void sample (float x)
        {
            const double v = x;
            const int s = v >= thr ? 1 : v <= -thr ? -1 : 0;
            fed = true;
            if (s != sign)
            {
                closeRun (at, std::abs (v));
                start = at;
                sign = s;
                beforeStart = at > 0 ? prev : 0.0;
            }
            prev = std::abs (v);
            ++at;
        }

        double fs = 48000.0, tolerance_ = 1e-5, thr = 0.0, beforeStart = 0.0, prev = 0.0;
        float level = 0.0f;
        bool fed = false;
        std::int64_t at = 0, start = -1;
        int sign = 0, runCount = 0, longestRun = 0;
        std::vector<Event> ev;
    };

    // The measuring, block by block. The song's length must be known first
    // (where the analysis windows fall depends on it). Feed every sample once
    // (process), then call nextPass(): when it returns true, feed every sample
    // again (the clipping check needs each channel's peak before it starts);
    // then finish(). r null for one channel.
    class FileAnalysis
    {
    public:
        using Measurements = analysis::Measurements;

        void prepare (double sampleRate, int numChannels, std::int64_t totalFrames, analysis::Options options = {})
        {
            fs = sampleRate;
            channelsInFile = numChannels;
            stereo = numChannels > 1;
            n = totalFrames;
            opts = options;
            pass = 1;
            at = 0;

            pkL = pkR = 0.0f; pkMax = 0.0f; pkAt = 0; oversL = oversR = 0;
            sL = sR = eL = eR = xLR = maxDiff = 0.0;

            meter.prepare (fs);
            meter.setRecordBlocks (true);
            tpm.prepare (stereo ? 2 : 1, 8192);
            secOver.clear(); secZero.clear();

            D = std::max (1, (int) std::round (fs / 6000.0));
            fsd = fs / D;
            decCount = n / D;
            const auto lp1k = BiquadCoefs::rbj (BiquadCoefs::Type::lowPass, fs, 1000.0, 0.7071067811865476);
            decim.setSections ({ lp1k, lp1k }, 2);
            for (int b = 0; b < analysis::numBands; ++b)
            {
                const auto& bd = analysis::bands[b];
                const double rate = bd.low ? fsd : fs;
                std::vector<BiquadCoefs> st;
                if (bd.lo > 0) for (int k = 0; k < 2; ++k) st.push_back (BiquadCoefs::rbj (BiquadCoefs::Type::highPass, rate, bd.lo, 0.7071067811865476));
                if (bd.hi > 0) for (int k = 0; k < 2; ++k) st.push_back (BiquadCoefs::rbj (BiquadCoefs::Type::lowPass, rate, bd.hi, 0.7071067811865476));
                band[b] = {};
                band[b].chain.setSections (st, 2);
            }
            low = {};
            low.chain.setSections ({ BiquadCoefs::rbj (BiquadCoefs::Type::highPass, fsd, 20.0, 0.7071067811865476),
                                     BiquadCoefs::rbj (BiquadCoefs::Type::lowPass, fsd, 120.0, 0.7071067811865476),
                                     BiquadCoefs::rbj (BiquadCoefs::Type::lowPass, fsd, 120.0, 0.7071067811865476) }, 2);
            low.perSec = (int) std::round (fsd);

            firstLoud = -1; lastLoud = -1;
            first = last = 0.0f;
            headTo = std::min<std::int64_t> (n, (std::int64_t) std::round (0.05 * fs));
            endFrom = std::max<std::int64_t> (0, n - (std::int64_t) std::round (0.1 * fs));
            decFrom = std::max<std::int64_t> (0, n - (std::int64_t) std::round (1.1 * fs));
            decTo = std::min<std::int64_t> (n, n - (std::int64_t) std::round (1.0 * fs));
            headE = endE = decE = 0.0;

            cliff.prepare (fs, n);

            const bool deep = opts.bitDepth > 16 && ! opts.isFloat;
            gridStride = std::max<std::int64_t> (1, n / 400000);
            gridChecked = 0; gridOff = 0; gridNext = 0;
            decNext = 0; decTaken = 0;
            gridDone[0] = gridDone[1] = ! deep;
            if (! stereo) gridDone[1] = true;

            for (int c = 0; c < 2; ++c) { clipCh[c] = {}; clipOn[c] = false; }
        }

        void process (const float* l, const float* r, int count)
        {
            if (! stereo || r == nullptr) r = l;
            if (pass == 1) firstPass (l, r, count);
            else if (pass == 2) secondPass (l, r, count);
            at += count;
        }

        // Ends a pass. True: feed the song again for the clipping check.
        bool nextPass()
        {
            if (pass == 1)
            {
                pass = 2;
                at = 0;
                const double tol = FlatRuns::tolerance (opts.bitDepth, opts.isFloat);
                const float pk[2] { pkL, pkR };
                bool any = false;
                for (int c = 0; c < (stereo ? 2 : 1); ++c)
                {
                    clipOn[c] = pk[c] >= 0.5f; // only a channel peaking at 0.5 or more is checked
                    clipCh[c].prepare (pk[c], tol, fs);
                    any = any || clipOn[c];
                }
                if (any) return true;
            }
            pass = 3;
            return false;
        }

        Measurements finish()
        {
            if (pass == 1) nextPass();
            if (pass == 2) { pass = 3; }
            Measurements m;
            m.fs = fs;
            m.channels = channelsInFile;
            m.frames = n;
            m.duration = (double) n / fs;
            if (n <= 0) return m;
            m.empty = false;
            using loudness::db10;
            using loudness::db20;

            const double nd = (double) n;
            const double eMid = (eL + eR + 2.0 * xLR) / 4.0;
            const double eSide = std::max (0.0, (eL + eR - 2.0 * xLR) / 4.0);
            m.samplePeakDb = db20 (pkMax);
            m.samplePeakAt = (double) pkAt / fs;
            m.peakLDb = db20 (pkL);
            m.peakRDb = db20 (pkR);
            m.dcL = sL / nd;
            m.dcR = sR / nd;
            m.dcLDb = db20 (std::abs (m.dcL));
            m.dcRDb = db20 (std::abs (m.dcR));
            m.oversFloat = oversL + oversR;
            m.corr = eL > 1e-12 && eR > 1e-12 ? xLR / std::sqrt (eL * eR) : 1.0;
            m.monoLossDb = db10 (eMid) - db10 ((eL + eR) / 2.0);
            m.sideToMidDb = db10 (eSide) - db10 (eMid);
            m.identical = maxDiff == 0.0;
            m.maxDiff = maxDiff;
            m.silentL = channelsInFile > 1 && (double) pkL < 1e-4 && (double) pkR > 1e-3;
            m.silentR = channelsInFile > 1 && (double) pkR < 1e-4 && (double) pkL > 1e-3;
            m.silent = (double) pkMax < 1e-4;

            LoudnessReport rep;
            loudness::fromBlocks (rep, meter.blockEnergy (0), meter.blockEnergy (1), meter.blockLength(), meter.integrated());
            m.lufs = rep.integrated;
            m.shortTermMax = rep.shortTermMax;
            m.shortTermMaxAt = rep.shortTermMaxAt;
            m.lra = rep.range;
            m.timeline = rep.timeline;
            m.balanceDb = rep.balanceDb;

            if (! tailDone) { tpm.finishOver (overReport, [this] (std::int64_t pos, float v) { noteTruePeak (pos, v); }); tailDone = true; }
            m.truePeakDb = db20 (tpm.truePeak());
            m.truePeakAt = (double) tpm.truePeakPosition() / fs;
            m.tpOverCount = (int) secOver.size();
            m.tpOverZeroCount = (int) secZero.size();
            m.tpOvers.assign (secOver.begin(), secOver.begin() + std::min<size_t> (40, secOver.size()));
            m.tpOverZero.assign (secZero.begin(), secZero.begin() + std::min<size_t> (40, secZero.size()));

            // clipping
            {
                for (int c = 0; c < 2; ++c) if (clipOn[c]) clipCh[c].finish();
                std::vector<FlatRuns::Event> all;
                for (int c = 0; c < 2; ++c) if (clipOn[c]) all.insert (all.end(), clipCh[c].events().begin(), clipCh[c].events().end());
                const auto merged = FlatRuns::merge (all);
                int runs = 0, longest = 0;
                double level = 0.0;
                for (int c = 0; c < 2; ++c)
                    if (clipOn[c] && clipCh[c].runs() > 0)
                    {
                        runs += clipCh[c].runs();
                        longest = std::max (longest, clipCh[c].longest());
                        level = std::max (level, (double) clipCh[c].peak());
                    }
                m.clip.runs = runs;
                m.clip.events = (int) merged.size();
                m.clip.longest = longest;
                m.clip.hasLevel = runs > 0;
                m.clip.levelDb = runs > 0 ? db20 (level) : 0.0;
                for (size_t k = 0; k < merged.size() && k < 40; ++k) m.clip.times.push_back (merged[k].start);
            }

            // bands
            const double totalE = (eL + eR) / 2.0 / nd;
            auto measure = [&] (const BandAcc& b, double len, double& corr, double& loss, double& share)
            {
                corr = b.ll > 1e-12 && b.rr > 1e-12 ? b.lr / std::sqrt (b.ll * b.rr) : 1.0;
                loss = db10 ((b.ll + b.rr + 2.0 * b.lr) / 4.0) - db10 ((b.ll + b.rr) / 2.0);
                share = db10 ((b.ll + b.rr) / 2.0 / std::max (1.0, len)) - db10 (totalE);
            };
            for (int b = 0; b < analysis::numBands; ++b)
                measure (band[b], analysis::bands[b].low ? (double) decCount : nd, m.band[b].corr, m.band[b].monoLossDb, m.band[b].shareDb);
            measure (low, (double) decCount, m.low.corr, m.low.monoLossDb, m.low.shareDb);
            {
                double sum = 0.0;
                for (const auto& s : low.secs) sum += s[0] + s[1];
                const double avgSec = sum / std::max<double> (1.0, (double) low.secs.size());
                for (size_t k = 0; k < low.secs.size(); ++k)
                {
                    const auto& s = low.secs[k];
                    const double c = s[0] > 0 && s[1] > 0 ? s[2] / std::sqrt (s[0] * s[1]) : 1.0;
                    if (c < -0.2 && s[0] + s[1] > avgSec * 0.03) m.low.outTimes.push_back ((int) k);
                }
            }

            // the start and the end
            const std::int64_t lead = firstLoud < 0 ? n : firstLoud;
            const std::int64_t tail = firstLoud < 0 ? 0 : n - 1 - lastLoud;
            m.leadSec = (double) lead / fs;
            m.tailSec = (double) tail / fs;
            m.firstSampleDb = db20 (first);
            m.lastSampleDb = db20 (last);
            auto rmsOf = [] (double e, std::int64_t from, std::int64_t to) { return db10 (e / (double) std::max<std::int64_t> (1, to - from)); };
            m.headRmsDb = rmsOf (headE, 0, headTo);
            m.endRmsDb = rmsOf (endE, endFrom, n);
            m.endDecayDb = rmsOf (decE, decFrom, decTo) - m.endRmsDb;

            m.hasSpectrum = cliff.result (m.spectrum);

            if (opts.bitDepth > 16 && ! opts.isFloat)
            {
                m.hasPadded16 = true;
                m.padded16 = gridChecked > 5000 && gridOff == 0;
            }
            return m;
        }

        // The whole song in memory: both passes. r null for one channel.
        static Measurements analyse (const float* l, const float* r, std::int64_t frames, double sampleRate, int numChannels,
                                     analysis::Options options = {})
        {
            FileAnalysis a;
            a.prepare (sampleRate, numChannels, frames, options);
            const int block = 65536;
            for (int pass = 0; pass < 2; ++pass)
            {
                for (std::int64_t s = 0; s < frames; s += block)
                {
                    const int len = (int) std::min<std::int64_t> (block, frames - s);
                    a.process (l + s, r != nullptr ? r + s : nullptr, len);
                }
                if (! a.nextPass()) break;
            }
            return a.finish();
        }

    private:
        struct BandAcc
        {
            BiquadChain chain;
            double ll = 0, rr = 0, lr = 0;
            int perSec = 0, inSec = 0;
            double wl = 0, wr = 0, wlr = 0;
            std::vector<std::array<double, 3>> secs;
        };

        // the meter reports what's above this; noteTruePeak keeps what's over -1 dBTP (0.8913)
        static constexpr float overReport = 0.89f;

        void noteTruePeak (std::int64_t pos, float v)
        {
            if ((double) v > 0.8913)
            {
                const int s = (int) std::floor ((double) pos / fs);
                if (secOver.empty() || secOver.back() != s) secOver.push_back (s);
                if (v > 1.0f && (secZero.empty() || secZero.back() != s)) secZero.push_back (s);
            }
        }

        void firstPass (const float* l, const float* r, int count)
        {
            for (int i = 0; i < count; ++i)
            {
                const double a = l[i], b = r[i];
                const float aa = std::abs (l[i]), ab = std::abs (r[i]);
                if (aa > pkL) pkL = aa;
                if (ab > pkR) pkR = ab;
                if (aa > pkMax || ab > pkMax) { pkMax = std::max (aa, ab); pkAt = at + i; }
                if (aa > 1.0f) ++oversL;
                if (ab > 1.0f) ++oversR;
                sL += a; sR += b;
                eL += a * a; eR += b * b;
                xLR += a * b;
                const double d = std::abs (a - b);
                if (d > maxDiff) maxDiff = d;

                // the start and the end
                const std::int64_t g = at + i;
                if ((double) std::max (aa, ab) >= 0.001) { if (firstLoud < 0) firstLoud = g; lastLoud = g; }
                if (g == 0) first = std::max (aa, ab);
                if (g == n - 1) last = std::max (aa, ab);
                const double e = (a * a + b * b) / 2.0;
                if (g < headTo) headE += e;
                if (g >= endFrom) endE += e;
                if (g >= decFrom && g < decTo) decE += e;

                // 24-bit file holding 16-bit audio: every value on the 16-bit grid
                if (g == gridNext)
                {
                    gridNext += gridStride;
                    for (int c = 0; c < 2; ++c)
                    {
                        if (gridDone[c]) continue;
                        const double v = (double) (c == 0 ? l[i] : r[i]) * 32768.0;
                        if (v != 0.0)
                        {
                            ++gridChecked;
                            if (std::abs (v - roundHalfUp (v)) > 1e-4) { ++gridOff; gridDone[c] = true; }
                        }
                    }
                }
            }

            meter.process (l, stereo ? r : nullptr, count);
            const float* ch[2] { l, r };
            tpm.processOver (ch, count, overReport, [this] (std::int64_t pos, float v) { noteTruePeak (pos, v); });
            cliff.process (l, r, count);
            bands (l, r, count);
        }

        void bands (const float* l, const float* r, int count)
        {
            // full-rate bands
            for (int b = 0; b < analysis::numBands; ++b)
                if (! analysis::bands[b].low) accumulate (band[b], l, r, count);

            // decimated copy: the 1 kHz low-pass twice, then every D-th sample
            if ((int) bufL.size() < count) { bufL.resize ((size_t) count); bufR.resize ((size_t) count); }
            std::copy (l, l + count, bufL.begin());
            if (stereo) std::copy (r, r + count, bufR.begin());
            decim.processPair (bufL.data(), stereo ? bufR.data() : nullptr, count);
            decL.clear(); decR.clear();
            // the decimated samples are those at multiples of D, the first decCount of them
            for (; decNext < at + count && decTaken < decCount; decNext += D, ++decTaken)
            {
                const size_t i = (size_t) (decNext - at);
                decL.push_back (bufL[i]);
                decR.push_back (stereo ? bufR[i] : bufL[i]);
            }
            if (decL.empty()) return;
            for (int b = 0; b < analysis::numBands; ++b)
                if (analysis::bands[b].low) accumulate (band[b], decL.data(), decR.data(), (int) decL.size());
            accumulate (low, decL.data(), decR.data(), (int) decL.size());
        }

        void accumulate (BandAcc& b, const float* x1, const float* x2, int count)
        {
            if ((int) tmpA.size() < count) { tmpA.resize ((size_t) count); tmpB.resize ((size_t) count); }
            std::copy (x1, x1 + count, tmpA.begin());
            const float* pb = tmpA.data();
            if (stereo)
            {
                std::copy (x2, x2 + count, tmpB.begin());
                pb = tmpB.data();
            }
            b.chain.processPair (tmpA.data(), stereo ? tmpB.data() : nullptr, count);
            for (int i = 0; i < count; ++i)
            {
                const double p = tmpA[(size_t) i], q = pb[i];
                b.ll += p * p; b.rr += q * q; b.lr += p * q;
                if (b.perSec > 0)
                {
                    b.wl += p * p; b.wr += q * q; b.wlr += p * q;
                    if (++b.inSec == b.perSec) { b.secs.push_back ({ b.wl, b.wr, b.wlr }); b.wl = b.wr = b.wlr = 0; b.inSec = 0; }
                }
            }
        }

        void secondPass (const float* l, const float* r, int count)
        {
            for (int c = 0; c < (stereo ? 2 : 1); ++c)
            {
                if (clipOn[c]) clipCh[c].process (c == 0 ? l : r, count);
            }
        }

        double fs = 48000.0;
        int channelsInFile = 2;
        bool stereo = true;
        std::int64_t n = 0, at = 0;
        analysis::Options opts;
        int pass = 1;

        float pkL = 0, pkR = 0, pkMax = 0;
        std::int64_t pkAt = 0;
        int oversL = 0, oversR = 0;
        double sL = 0, sR = 0, eL = 0, eR = 0, xLR = 0, maxDiff = 0;

        LoudnessMeter meter;
        TruePeakMeter tpm;
        bool tailDone = false;
        std::vector<int> secOver, secZero;

        FlatRuns clipCh[2];
        bool clipOn[2] { false, false };

        int D = 8;
        double fsd = 6000.0;
        std::int64_t decCount = 0;
        BiquadChain decim;
        BandAcc band[analysis::numBands], low;
        std::vector<float> bufL, bufR, decL, decR, tmpA, tmpB;

        std::int64_t firstLoud = -1, lastLoud = -1;
        float first = 0, last = 0;
        std::int64_t headTo = 0, endFrom = 0, decFrom = 0, decTo = 0;
        double headE = 0, endE = 0, decE = 0;

        CliffFinder cliff;

        std::int64_t gridStride = 1, gridChecked = 0, gridOff = 0, gridNext = 0, decNext = 0, decTaken = 0;
        bool gridDone[2] { true, true };
    };
}
