// The measurements (dsp/TruePeak.h, LoudnessReport.h, Analysis.h, Spectrum.h,
// Waveform.h) built without JUCE, as the browser build is: reference values,
// the true-peak meter reading what the limiter holds, and each measurement on
// signals whose answer is known. Each check prints PASS or FAIL; the suite
// exits non-zero on any failure.
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
#include "dsp/Analysis.h"
#include "dsp/Loudness.h"
#include "dsp/LoudnessReport.h"
#include "dsp/Spectrum.h"
#include "dsp/TruePeak.h"
#include "dsp/Waveform.h"

namespace
{
    int failures = 0;
    constexpr double twoPi = 6.283185307179586;

    void expect (const char* name, bool ok, const char* fmt = "", double a = 0, double b = 0, double c = 0)
    {
        std::printf ("%s %-66s ", ok ? "PASS" : "FAIL", name);
        std::printf (fmt, a, b, c);
        std::printf ("\n");
        if (! ok) ++failures;
    }

    std::vector<float> sine (double fs, double seconds, double hz, double amp, double phase = 0.0)
    {
        std::vector<float> x ((size_t) (fs * seconds));
        for (size_t i = 0; i < x.size(); ++i) x[i] = (float) (amp * std::sin (twoPi * hz * (double) i / fs + phase));
        return x;
    }

    std::vector<float> noise (size_t n, float amp, unsigned seed)
    {
        std::mt19937 rng (seed);
        std::uniform_real_distribution<float> u (-1.0f, 1.0f);
        std::vector<float> x (n);
        for (auto& v : x) v = amp * u (rng);
        return x;
    }

    float meterDb (const std::vector<float>& l, const std::vector<float>* r, int block = 700)
    {
        vs::TruePeakMeter m;
        m.prepare (r != nullptr ? 2 : 1, block);
        for (size_t s = 0; s < l.size(); s += (size_t) block)
        {
            const int n = (int) std::min ((size_t) block, l.size() - s);
            const float* ch[2] { l.data() + s, r != nullptr ? r->data() + s : nullptr };
            m.process (ch, n);
        }
        m.finish();
        return m.truePeakDb();
    }

    double db (double x) { return 20.0 * std::log10 (x); }
}

int main()
{
    // ---- the true-peak meter ----
    {
        const double fs = 48000.0;
        const auto s = sine (fs, 1.0, 997.0, 0.5);
        const float tp = meterDb (s, nullptr);
        expect ("true peak: 997 Hz sine at 0.5 reads -6.02 dBTP", std::abs (tp - db (0.5)) < 0.01, "%.3f dBTP", tp);

        // a quarter of the rate at 45 degrees: every sample is at 0.707 of the
        // peak, the 4x copy lands on the peak itself
        // (faded in and out: an abrupt start rings above the sine's own peak)
        auto q = sine (44100.0, 1.0, 44100.0 / 4, 0.95, twoPi / 8);
        for (size_t i = 0; i < 4410; ++i)
        {
            const float g = (float) (0.5 - 0.5 * std::cos (twoPi * 0.5 * (double) i / 4410.0));
            q[i] *= g;
            q[q.size() - 1 - i] *= g;
        }
        const float tq = meterDb (q, nullptr);
        expect ("true peak: fs/4 at 45 degrees (samples 3 dB under) reads the peak", std::abs (tq - db (0.95)) < 0.05,
                "%.3f dBTP (sample peak %.3f)", tq, db (0.95 * std::sqrt (0.5)));

        // the same reading whatever the block size; the click's position is found
        auto n = noise (30000, 0.4f, 3);
        n[12345] = 0.99f;
        const float a = meterDb (n, nullptr, 64), b = meterDb (n, nullptr, 8192);
        vs::TruePeakMeter m;
        m.prepare (1, 512);
        const float* ch[1] { n.data() };
        for (size_t s0 = 0; s0 < n.size(); s0 += 512) { ch[0] = n.data() + s0; m.process (ch, (int) std::min<size_t> (512, n.size() - s0)); }
        m.finish();
        expect ("true peak: same reading in blocks of 64 and 8192", a == b, "%.6f / %.6f", a, b);
        expect ("true peak: never under the sample peak, at the loudest sample",
                m.truePeak() >= m.samplePeak() && std::llabs (m.truePeakPosition() - 12345) <= 2,
                "TP %.4f SP %.4f at %.0f", m.truePeak(), m.samplePeak(), (double) m.truePeakPosition());
        expect ("true peak: the detector's delay is a whole number of samples here",
                vs::TruePeakDetector::groupLatency() * vs::TruePeakDetector::factor - vs::TruePeakDetector::upLatency() <= 2,
                "%.0f sub-samples, %.0f samples", vs::TruePeakDetector::upLatency(), vs::TruePeakDetector::groupLatency());

        // the limiter holds its ceiling on the meter's reading: the same detector
        std::vector<float> l = noise ((size_t) fs * 3, 1.0f, 7), r = noise ((size_t) fs * 3, 1.0f, 8);
        for (size_t i = 0; i < l.size(); ++i) { l[i] = 1.6f * (0.6f * l[i] + 0.4f * (float) std::sin (i * 0.9)); r[i] *= 1.2f; }
        vs::TruePeakLimiter lim;
        lim.prepare (fs, 512);
        lim.setCeilingDb (-1.0f);
        for (size_t s0 = 0; s0 < l.size(); s0 += 512)
        {
            const int len = (int) std::min<size_t> (512, l.size() - s0);
            float* p[2] { l.data() + s0, r.data() + s0 };
            juce::dsp::AudioBlock<float> blk (p, 2, (size_t) len);
            lim.process (blk);
        }
        // leave out the limiter's start-up (its look-ahead and detector delay)
        std::vector<float> lt (l.begin() + 4800, l.end()), rt (r.begin() + 4800, r.end());
        const float out = meterDb (lt, &rt);
        expect ("true peak: a -1 dBTP limited master reads under -1 dBTP", out <= -1.0f, "%.3f dBTP", out);
    }

    // ---- the loudness report ----
    {
        const double fs = 48000.0;
        const auto s = sine (fs, 10.0, 997.0, 0.1);
        const auto st = vs::measureLoudness (s.data(), s.data(), (std::int64_t) s.size(), fs);
        expect ("report: 997 Hz at -20 dBFS on both sides is -20 LUFS", std::abs (st.integrated + 20.0) < 0.05, "%.3f LUFS", st.integrated);
        expect ("report: short-term and momentary maxima of a steady tone, the same",
                std::abs (st.shortTermMax - st.integrated) < 0.05 && std::abs (st.momentaryMax - st.integrated) < 0.05, "%.3f / %.3f", st.shortTermMax, st.momentaryMax);
        expect ("report: a steady tone has no loudness range, no imbalance", st.range < 0.1 && std::abs (st.balanceDb) < 0.01, "%.3f LU, %.3f dB", st.range, st.balanceDb);
        expect ("report: sample peak and true peak of the tone", std::abs (st.samplePeakDb + 20.0) < 0.01 && std::abs (st.truePeakDb + 20.0) < 0.01,
                "%.3f / %.3f", st.samplePeakDb, st.truePeakDb);
        const auto mono = vs::measureLoudness (s.data(), nullptr, (std::int64_t) s.size(), fs);
        expect ("report: one channel is measured as one channel (3 dB under both)", std::abs (mono.integrated - (st.integrated - 3.0103)) < 0.01,
                "%.3f LUFS", mono.integrated);

        // EBU Tech 3342 case 1: 20 s at -20 then 20 s at -30 LUFS gives 10 LU
        auto a = sine (fs, 20.0, 1000.0, 0.1), b = sine (fs, 20.0, 1000.0, 0.1 / std::sqrt (10.0));
        a.insert (a.end(), b.begin(), b.end());
        const auto lr = vs::measureLoudness (a.data(), a.data(), (std::int64_t) a.size(), fs);
        expect ("report: loudness range of -20 then -30 LUFS is 10 LU", std::abs (lr.range - 10.0) < 1.0, "%.2f LU", lr.range);
        expect ("report: the timeline has one value a second", lr.timeline.size() == 38, "%.0f values", (double) lr.timeline.size());

        vs::ChannelLevels lv (3);
        const float* ch[3] { s.data(), nullptr, a.data() };
        lv.add (ch, 3, 4800);
        expect ("levels: per-channel peak and RMS, a missing channel silent",
                std::abs (lv.peakOf (0) - 0.1f) < 1e-3f && std::abs (lv.rmsOf (0) - 0.1f / std::sqrt (2.0f)) < 1e-3f && lv.peakOf (1) == 0.0f,
                "%.4f %.4f", lv.peakOf (0), lv.rmsOf (0));
    }

    // ---- the whole-file analysis ----
    {
        const double fs = 44100.0;
        const size_t n = (size_t) fs * 12;
        std::vector<float> l (n), r (n);
        auto nz = noise (n, 0.05f, 11);
        for (size_t i = 0; i < n; ++i)
        {
            const double t = (double) i / fs;
            const float tone = (float) (0.3 * std::sin (twoPi * 220 * t) + 0.2 * std::sin (twoPi * 60 * t));
            l[i] = tone + nz[i];
            r[i] = tone - nz[i];
        }
        // two seconds of silence in front, one at the end
        for (size_t i = 0; i < (size_t) (2 * fs); ++i) l[i] = r[i] = 0.0f;
        for (size_t i = n - (size_t) fs; i < n; ++i) l[i] = r[i] = 0.0f;
        // three clipped stretches
        for (size_t at : { (size_t) (4 * fs), (size_t) (6 * fs), (size_t) (8 * fs) })
            for (size_t i = at; i < at + 5; ++i) l[i] = 0.95f;
        l[(size_t) (4 * fs) - 1] = 0.2f; l[(size_t) (4 * fs) + 5] = 0.2f;
        l[(size_t) (6 * fs) - 1] = 0.2f; l[(size_t) (6 * fs) + 5] = 0.2f;
        l[(size_t) (8 * fs) - 1] = 0.2f; l[(size_t) (8 * fs) + 5] = 0.2f;

        vs::FileAnalysis a;
        a.prepare (fs, 2, (std::int64_t) n, { 24, false });
        int passes = 0;
        do
        {
            ++passes;
            for (size_t s = 0; s < n; s += 1000) a.process (l.data() + s, r.data() + s, (int) std::min<size_t> (1000, n - s));
        } while (a.nextPass());
        const auto m = a.finish();
        expect ("analysis: a song with peaks near the top takes two passes", passes == 2, "%.0f", passes);
        expect ("analysis: lead-in 2 s, tail 1 s", std::abs (m.leadSec - 2.0) < 1e-3 && std::abs (m.tailSec - 1.0) < 1e-3, "%.4f / %.4f", m.leadSec, m.tailSec);
        expect ("analysis: three clipped stretches found", m.clip.runs == 3 && m.clip.events == 3 && m.clip.longest == 5,
                "%.0f runs, %.0f events, longest %.0f", m.clip.runs, m.clip.events, m.clip.longest);
        expect ("analysis: the low end stays in phase (60 Hz on both sides)", m.band[0].corr > 0.9 && m.low.outTimes.empty(), "corr %.3f", m.band[0].corr);
        expect ("analysis: side noise lowers the correlation", m.corr < 0.99 && m.corr > 0.5, "%.3f", m.corr);
        expect ("analysis: a real 24-bit file is not 16-bit padded", m.hasPadded16 && ! m.padded16);
        expect ("analysis: matches the whole-buffer call", [&] {
            const auto w = vs::FileAnalysis::analyse (l.data(), r.data(), (std::int64_t) n, fs, 2, { 24, false });
            return w.lufs == m.lufs && w.truePeakDb == m.truePeakDb && w.clip.runs == m.clip.runs && w.band[3].corr == m.band[3].corr;
        }());

        // flipped polarity, DC, 16-bit audio in a 24-bit file
        std::vector<float> f (n), g (n);
        for (size_t i = 0; i < n; ++i)
        {
            const float v = std::round ((0.4f * (float) std::sin (twoPi * 110 * (double) i / fs) + 0.01f) * 32768.0f) / 32768.0f;
            f[i] = v; g[i] = -v;
        }
        const auto p = vs::FileAnalysis::analyse (f.data(), g.data(), (std::int64_t) n, fs, 2, { 24, false });
        expect ("analysis: flipped polarity reads correlation -1", p.corr < -0.999 && p.band[1].corr < -0.99 && ! p.low.outTimes.empty(), "%.4f", p.corr);
        expect ("analysis: DC offset", std::abs (p.dcL - 0.01) < 1e-4 && std::abs (p.dcR + 0.01) < 1e-4, "%.5f / %.5f", p.dcL, p.dcR);
        expect ("analysis: 16-bit audio in a 24-bit file", p.hasPadded16 && p.padded16);

        // a lossy encoder's wall at 16 kHz
        auto w = noise (n, 0.3f, 21);
        vs::BiquadChain lp;
        std::vector<vs::BiquadCoefs> secs;
        for (int k = 0; k < 10; ++k) secs.push_back (vs::BiquadCoefs::rbj (vs::BiquadCoefs::Type::lowPass, fs, 16000.0, 0.7071));
        lp.setSections (secs, 1);
        lp.process (0, w.data(), (int) n);
        const auto c = vs::FileAnalysis::analyse (w.data(), w.data(), (std::int64_t) n, fs, 2);
        expect ("analysis: the high-frequency cliff of a 16 kHz wall", c.hasSpectrum && c.spectrum.cliffHz >= 15000 && c.spectrum.cliffHz <= 18000,
                "%.0f Hz, %.1f dB", c.spectrum.cliffHz, c.spectrum.drop);
        const auto open = vs::FileAnalysis::analyse (nz.data(), nz.data(), (std::int64_t) n, fs, 2);
        expect ("analysis: full-band noise has no cliff", open.hasSpectrum && open.spectrum.cliffHz == 0.0);
        const auto shortOne = vs::FileAnalysis::analyse (nz.data(), nullptr, 9000, fs, 1);
        expect ("analysis: a short file has no spectrum verdict, one channel", ! shortOne.hasSpectrum && ! shortOne.empty && shortOne.channels == 1);
        const auto empty = vs::FileAnalysis::analyse (nz.data(), nullptr, 0, fs, 1);
        expect ("analysis: an empty file", empty.empty);
    }

    // ---- the spectrum analyser ----
    {
        const double fs = 48000.0;
        vs::SpectrumAnalyser an;
        an.prepare ({});
        const double hz = 100.0 * fs / 4096.0; // on bin 100
        const auto s = sine (fs, 0.5, hz, 1.0);
        for (size_t at = 0; at + 1600 <= s.size(); at += 1600) { an.push (s.data() + at, 1600); an.update(); }
        expect ("spectrum: a full-scale sine on a bin reads 0 dB", std::abs (an.levelsDb()[100]) < 0.05, "%.3f dB", an.levelsDb()[100]);
        expect ("spectrum: and far from it, far down", an.levelsDb()[400] < -60.0f, "%.1f dB", an.levelsDb()[400]);
        expect ("spectrum: levelAt reads the bin, with a tilt", std::abs (an.levelAt (hz, fs) - an.levelsDb()[100]) < 1e-3f
                    && std::abs (an.levelAt (hz, fs, 4.5f, (float) hz / 2) - an.levelsDb()[100] - 4.5f) < 1e-3f);
        expect ("spectrum: the power average rises toward the level", an.power()[100] > 0.3f && an.power()[100] < 1.0f, "%.3f", an.power()[100]);
    }

    // ---- the waveform overviews ----
    {
        std::vector<float> x (1000, 0.0f);
        x[10] = 0.5f; x[11] = -0.8f; x[995] = 0.25f;
        vs::waveform::Overview o;
        o.prepare (44100.0, 1000, { 10, 1, true, true });
        o.process (x.data(), nullptr, 600);
        o.process (x.data() + 600, nullptr, 400);
        expect ("overview: column peaks", o.peak()[0] == 0.8f && o.peak()[9] == 0.25f && o.peak()[5] == 0.0f);
        vs::waveform::Overview o2;
        o2.prepare (44100.0, 1000, { 10, 2, false, false });
        o2.process (x.data(), nullptr, 1000);
        expect ("overview: every other sample from each column's first", o2.peak()[0] == 0.5f && o2.peak()[9] == 0.0f);
        const auto rms = o.rms();
        expect ("overview: RMS per column", std::abs (rms[0] - std::sqrt ((0.25f + 0.64f) / 100.0f)) < 1e-6f, "%.6f", rms[0]);

        vs::waveform::BlockPeaks bp (100);
        std::vector<short> s16 { 3, -7, 2 };
        bp.process (s16.data(), 3);
        bp.process (x.data(), 1000);
        const auto pk = bp.peaks();
        expect ("block peaks: whole blocks and the last part-block", pk.size() == 11 && pk[0] == 7.0 && pk[9] == 0.25 && pk[10] == 0.0, "%.0f blocks", (double) pk.size());
        const auto shape = vs::waveform::shape (pk.data(), (int) pk.size(), 800);
        expect ("shape: 800 points, the loudest 255", shape.size() == 800 && *std::max_element (shape.begin(), shape.end()) == 255);
        expect ("shape: none from no peaks", vs::waveform::shape (pk.data(), 0, 800).empty());
        const float* ch[1] { x.data() };
        const auto px = vs::waveform::pixelPeaks (ch, 1, 1000, 100);
        expect ("pixel peaks", px.size() == 100 && px[1] == 0.8f && px[99] == 0.25f);
    }

    std::printf ("%s\n", failures == 0 ? "All measurement tests passed" : "MEASUREMENT TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
