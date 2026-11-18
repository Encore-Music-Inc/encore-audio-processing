// The restoration of record and tape transfers, built without JUCE as the
// browser build is: gap fill, the de-clicker, noise reduction, the playback
// curves and the chain (dsp/Restoration.h), and the analysis and its detectors
// (dsp/RestorationAnalysis.h), each on a signal whose answer is known: a small
// band (accordion chords, tuba bass, a melody) with damage put in at known
// places. Each check prints PASS or FAIL; the suite exits non-zero on any failure.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>
#include "dsp/Lpc.h"
#include "dsp/Restoration.h"
#include "dsp/RestorationAnalysis.h"

namespace
{
    int failures = 0;
    constexpr double twoPi = 6.283185307179586;

    void expect (const char* name, bool ok, const char* fmt = "", double a = 0, double b = 0, double c = 0)
    {
        std::printf ("%s %-72s ", ok ? "PASS" : "FAIL", name);
        std::printf (fmt, a, b, c);
        std::printf ("\n");
        if (! ok) ++failures;
    }

    struct Stereo { std::vector<float> l, r; };

    // A band: chords of reedy notes changing every half second, a tuba on the
    // beat, an eighth-note melody. cents: the whole thing off by that much;
    // wow (t): a pitch wobble in cents; gaps: silent stretches (seconds).
    Stereo band (double fs, double seconds, double cents = 0, std::function<double (double)> wow = {}, std::vector<std::pair<double, double>> gaps = {})
    {
        const int n = (int) std::round (seconds * fs);
        Stereo s { std::vector<float> ((size_t) n, 0.0f), std::vector<float> ((size_t) n, 0.0f) };
        std::vector<double> warp ((size_t) n + 1, 0.0);   // warped time, so phases follow the wobble
        for (int i = 0; i < n; ++i) warp[(size_t) i + 1] = warp[(size_t) i] + std::pow (2.0, (cents + (wow ? wow (i / fs) : 0.0)) / 1200.0) / fs;
        auto silent = [&] (double t) { for (auto [a, b] : gaps) if (t >= a && t < b) return true; return false; };
        const int chords[4][3] = { { 58, 62, 65 }, { 53, 57, 60 }, { 58, 62, 65 }, { 51, 55, 58 } };
        const int bass[4] = { 34, 29, 34, 27 };
        const int tune[12] = { 70, 72, 74, 75, 77, 74, 72, 70, 69, 72, 77, 74 };
        auto hz = [] (double m) { return 440.0 * std::pow (2.0, (m - 69) / 12); };
        for (int i = 0; i < n; ++i)
        {
            const double t = i / fs;
            if (silent (t)) continue;
            const double w = warp[(size_t) i];
            const int bar = (int) (t / 2.0) % 4, beat = (int) (t / 0.5), eighth = (int) (t / 0.25);
            const double inBeat = t - beat * 0.5, inEighth = t - eighth * 0.25;
            // every note starts with a 5 ms rise and ends with a 5 ms fall, so none starts with a step (a click)
            auto shape = [] (double in, double len) { return std::min ({ 1.0, in / 0.005, (len - in) / 0.005 }); };
            double v = 0;
            for (int m : chords[bar])
                for (int h = 1; h <= 10; ++h) v += 0.05 / h * std::sin (twoPi * hz (m) * h * w) * shape (inBeat, 0.5);
            v += 0.25 * std::sin (twoPi * hz (bass[bar]) * w) * std::exp (-inBeat * 4) * (beat % 2 == 0 ? 1 : 0.2) * shape (inBeat, 0.5);
            for (int h = 1; h <= 6; ++h) v += 0.08 / h * std::sin (twoPi * hz (tune[eighth % 12]) * h * w) * std::exp (-inEighth * 3) * shape (inEighth, 0.25);
            s.l[(size_t) i] = (float) v;
            s.r[(size_t) i] = (float) (0.9 * v);
        }
        return s;
    }

    void addHiss (Stereo& s, double db, unsigned seed)
    {
        std::mt19937 rng (seed);
        std::normal_distribution<double> g (0.0, std::pow (10.0, db / 20.0));
        for (size_t i = 0; i < s.l.size(); ++i) { s.l[i] += (float) g (rng); s.r[i] += (float) g (rng); }
    }

    void addHum (Stereo& s, double fs, double hz, double db)
    {
        for (size_t i = 0; i < s.l.size(); ++i)
        {
            double v = 0;
            for (int h = 1; h <= 5; ++h) v += std::pow (10.0, (db - 6 * (h - 1)) / 20.0) * std::sin (twoPi * hz * h * i / fs);
            s.l[i] += (float) v;
            s.r[i] += (float) v;
        }
    }

    // Clicks: a sharp spike ringing for a fraction of a millisecond, the same in both channels. Their starts.
    std::vector<long> addClicks (Stereo& s, double fs, int count, unsigned seed, double amp = 0.3)
    {
        std::mt19937 rng (seed);
        std::uniform_real_distribution<double> u (0, 1);
        std::vector<long> at;
        for (int k = 0; k < count; ++k)
        {
            const long a = (long) (0.1 * fs + u (rng) * ((double) s.l.size() - 0.2 * fs));
            const int len = std::max (2, (int) ((0.0002 + u (rng) * 0.0008) * fs));
            const double sign = u (rng) < 0.5 ? -1 : 1, f = 3000 + u (rng) * 4000;
            for (int i = 0; i < len; ++i)
            {
                const double v = sign * amp * std::exp (-i / (len / 3.0)) * std::cos (twoPi * f * i / fs);
                s.l[(size_t) (a + i)] += (float) v;
                s.r[(size_t) (a + i)] += (float) v;
            }
            at.push_back (a);
        }
        std::sort (at.begin(), at.end());
        return at;
    }

    double rms (const std::vector<float>& x, size_t from = 0, size_t to = SIZE_MAX)
    {
        to = std::min (to, x.size());
        double s = 0;
        for (size_t i = from; i < to; ++i) s += (double) x[i] * x[i];
        return std::sqrt (s / (double) std::max<size_t> (1, to - from));
    }

    // ---- building blocks

    void gapFill()
    {
        const double fs = 48000;
        std::vector<double> x (4800);
        for (size_t i = 0; i < x.size(); ++i) x[i] = 0.5 * std::sin (twoPi * 440 * i / fs) + 0.2 * std::sin (twoPi * 1250 * i / fs);
        const auto clean = x;
        std::vector<long> missing;
        for (long t = 2400; t < 2440; ++t) { missing.push_back (t); x[(size_t) t] = 0.9; }
        std::vector<double> w (2000), r (33), a (33);
        for (int i = 0; i < 2000; ++i) w[(size_t) i] = 0.5 - 0.5 * std::cos (twoPi * (i + 0.5) / 2000);
        vs::lpc::windowedAutocorrelation (clean.data(), (long) clean.size(), 300, 2000, w.data(), 32, r.data());
        vs::lpc::predictor (r.data(), 32, a.data());
        vs::lpc::gapFill (x.data(), (long) x.size(), missing, a.data(), 32);
        double worst = 0;
        for (long t : missing) worst = std::max (worst, std::abs (x[(size_t) t] - clean[(size_t) t]));
        expect ("gap fill rebuilds 40 missing samples of two tones from the music around them", worst < 1e-3, "(largest error %.2g)", worst);
    }

    void filters()
    {
        const double fs = 48000;
        // zero phase: an impulse comes out symmetric about where it went in
        std::vector<double> imp (4001, 0.0);
        imp[2000] = 1.0;
        const auto y = vs::zeroPhase (vs::butterworth (vs::BiquadCoefs::Type::lowPass, fs, 2000, 4), imp.data(), (int) imp.size());
        double asym = 0;
        for (int k = 1; k < 1000; ++k) asym = std::max (asym, std::abs (y[(size_t) (2000 + k)] - y[(size_t) (2000 - k)]));
        expect ("zero-phase filtering shifts nothing (its impulse response is symmetric)", asym < 1e-9, "(largest difference %.2g)", asym);

        // the DC blocker takes a constant away and leaves 1 kHz alone
        const auto dc = vs::dcBlocker (fs);
        expect ("the DC blocker removes DC and passes 1 kHz",
                vs::sectionsResponse ({ dc }, 0.001, fs) < 1e-3 && std::abs (vs::sectionsResponse ({ dc }, 1000, fs) - 1) < 1e-3,
                "(at 1 kHz %.5f)", vs::sectionsResponse ({ dc }, 1000, fs));

        // a 78 curve: 0 dB at 1 kHz; the inverse RIAA follows the analog curve
        for (double rate : { 44100.0, 48000.0, 96000.0 })
        {
            const auto s = vs::restore::curveSections (rate, 0, 0, 50, true);
            double worst = 0;
            for (double f : { 100.0, 300.0, 3000.0, 10000.0, 16000.0 })
            {
                if (f > 0.42 * rate) continue;
                // RIAA's analog playback curve (time constants 3180, 318, 75 us), relative to 1 kHz, inverted
                auto riaa = [] (double hz)
                {
                    const double w = twoPi * hz;
                    const double t1 = 3180e-6, t2 = 318e-6, t3 = 75e-6;
                    return std::sqrt (1 + std::pow (w * t2, 2)) / (std::sqrt (1 + std::pow (w * t1, 2)) * std::sqrt (1 + std::pow (w * t3, 2)));
                };
                const double want = 20 * std::log10 (riaa (1000) / riaa (f));
                const double got = 20 * std::log10 (vs::sectionsResponse (s, f, rate));
                worst = std::max (worst, std::abs (got - want));
            }
            const double at1k = 20 * std::log10 (vs::sectionsResponse (s, 1000, rate));
            expect ("undoing RIAA follows the analog curve within 0.3 dB, 0 dB at 1 kHz", worst < 0.3 && std::abs (at1k) < 1e-6,
                    "(%.0f Hz: worst %.3f dB)", rate, worst);
        }
        const auto aes = vs::restore::curveSections (48000, 400, -12, 50, false);
        const double at10k = 20 * std::log10 (vs::sectionsResponse (aes, 10000, 48000) / vs::sectionsResponse (aes, 1000, 48000));
        const double bass = 20 * std::log10 (vs::sectionsResponse (aes, 60, 48000));
        expect ("an AES 78 curve: bass boosted below 400 Hz, treble down at 10 kHz", bass > 10 && at10k < -10,
                "(60 Hz %+.1f dB, 10 kHz %+.1f dB)", bass, at10k);
    }

    // ---- the de-clicker

    void declicker()
    {
        const double fs = 48000;
        auto clean = band (fs, 6);
        addHiss (clean, -60, 3);
        auto clicked = clean;
        const auto at = addClicks (clicked, fs, 20, 7);
        vs::Declicker d (fs);
        const float* ch[2] = { clicked.l.data(), clicked.r.data() };
        Stereo out;
        d.push (ch, (int) clicked.l.size());
        out.l = d.output (0); out.r = d.output (1);
        d.flush();
        out.l.insert (out.l.end(), d.output (0).begin(), d.output (0).end());
        out.r.insert (out.r.end(), d.output (1).begin(), d.output (1).end());
        expect ("the de-clicker gives back as many samples as it was given", out.l.size() == clicked.l.size(), "(%.0f)", (double) out.l.size());
        int foundAll = 0;
        for (long a : at)
        {
            bool found = false;
            for (const auto& e : d.events()) if (e.ch == 0 && e.at <= a + 60 && e.at + e.len >= a - 10) found = true;
            foundAll += found ? 1 : 0;
        }
        expect ("it finds every click", foundAll == (int) at.size(), "(%.0f of %.0f)", foundAll, (double) at.size());
        // rebuilt from the music either side: not exact (a millisecond of a rich chord), but most of each click gone
        double worstShare = 0, sumBefore = 0, sumAfter = 0;
        for (long a : at)
        {
            double eb = 0, ea = 0;
            for (long i = a - 5; i < a + 60; ++i)
            {
                eb = std::max (eb, (double) std::abs (clicked.l[(size_t) i] - clean.l[(size_t) i]));
                ea = std::max (ea, (double) std::abs (out.l[(size_t) i] - clean.l[(size_t) i]));
            }
            worstShare = std::max (worstShare, ea / eb);
            sumBefore += eb;
            sumAfter += ea;
        }
        expect ("and rebuilds the music under each (its error down by a third or more, 60% or more on average)",
                worstShare < 0.67 && sumAfter < 0.4 * sumBefore, "(worst %.2f, average %.2f of the click)", worstShare, sumAfter / sumBefore);
        // away from the clicks nothing changes
        double untouched = 0;
        for (size_t i = 0; i < out.l.size(); ++i)
        {
            bool near = false;
            for (long a : at) if ((long) i >= a - 200 && (long) i < a + 400) near = true;
            if (! near) untouched = std::max (untouched, (double) std::abs (out.l[i] - clicked.l[i]));
        }
        expect ("and leaves the music between them exactly as it was", untouched == 0, "(largest change %.2g)", untouched);
        // on the clean band (drum-like onsets and all) it finds nothing
        vs::Declicker q (fs);
        const float* cc[2] = { clean.l.data(), clean.r.data() };
        q.push (cc, (int) clean.l.size());
        q.flush();
        expect ("it leaves a clean recording alone (note onsets aren't clicks)", q.events().empty(), "(%.0f found)", (double) q.events().size());
        // counting only changes nothing
        vs::declick::Settings count;
        count.detectOnly = true;
        vs::Declicker c (fs, count);
        c.push (ch, (int) clicked.l.size());
        bool same = c.output (0) == std::vector<float> (clicked.l.begin(), clicked.l.begin() + (long) c.output (0).size());
        c.flush();
        expect ("counting finds the clicks and changes nothing", same && c.events().size() >= at.size() && c.repairedSamples() == 0, "(%.0f found)", (double) c.events().size());
        expect ("amounts set how far above the usual level a click must jump", vs::declick::clickThreshold (50) > vs::declick::clickThreshold (90)
                && std::isinf (vs::declick::clickThreshold (0)), "(50: %.1f, 90: %.1f)", vs::declick::clickThreshold (50), vs::declick::clickThreshold (90));
    }

    // ---- noise reduction

    void noiseReduction()
    {
        const double fs = 48000;
        const int N = vs::denoise::frameSize (fs);
        Stereo hiss { std::vector<float> ((size_t) N * 60, 0.0f), std::vector<float> ((size_t) N * 60, 0.0f) };
        addHiss (hiss, -40, 11);
        vs::NoiseProfileLearner learner (fs, N);
        for (int k = 0; k < 60; ++k) learner.add (hiss.l.data() + k * N, hiss.r.data() + k * N);
        const auto prof = learner.profile();
        expect ("the profile learns from frames of just hiss", prof.has_value() && learner.framesKept() >= 50, "(%.0f frames)", learner.framesKept());
        if (! prof) return;
        const double db = vs::denoise::profileDb (*prof, N);
        expect ("and reads its level back (within 1 dB of the hiss)", std::abs (db - (-40)) < 1, "(%.2f dBFS)", db);

        auto run = [&] (const Stereo& in, double amount, bool keep)
        {
            vs::SpectralDenoiser d (fs, amount, *prof, keep);
            Stereo out;
            d.push (in.l.data(), in.r.data(), (int) in.l.size());
            out.l = d.output (0); out.r = d.output (1);
            d.flush();
            out.l.insert (out.l.end(), d.output (0).begin(), d.output (0).end());
            out.r.insert (out.r.end(), d.output (1).begin(), d.output (1).end());
            return out;
        };
        Stereo fresh { std::vector<float> ((size_t) fs * 3, 0.0f), std::vector<float> ((size_t) fs * 3, 0.0f) };
        addHiss (fresh, -40, 12);
        const auto reduced = run (fresh, 40, false);
        const double down = 20 * std::log10 (rms (reduced.l, (size_t) fs, (size_t) fs * 2) / rms (fresh.l, (size_t) fs, (size_t) fs * 2));
        expect ("hiss alone comes down by about the amount's depth (10.2 dB at 40), no more", down < -8 && down > -12, "(%.1f dB)", down);
        expect ("noise reduction keeps the length (its delay is taken off)", reduced.l.size() == fresh.l.size(), "(%.0f)", (double) reduced.l.size());
        const auto kept = run (fresh, 0, true);
        double worst = 0;
        for (size_t i = 0; i < kept.l.size(); ++i) worst = std::max (worst, (double) std::abs (kept.l[i] - fresh.l[i]));
        expect ("kept in place but off, it gives the input back", worst < 1e-5, "(largest difference %.2g)", worst);
        auto music = band (fs, 3);
        const auto m = run (music, 40, false);
        const double change = 20 * std::log10 (rms (m.l, (size_t) fs, (size_t) fs * 2) / rms (music.l, (size_t) fs, (size_t) fs * 2));
        expect ("music well above the hiss is left nearly as it is", std::abs (change) < 0.5, "(%.2f dB)", change);
    }

    // ---- the chain

    Stereo runChain (double fs, const vs::restore::Settings& s, const Stereo& in, vs::restore::Run run = {})
    {
        vs::restore::Chain chain (fs, s, run);
        Stereo out;
        for (size_t p = 0; p < in.l.size(); p += 65536)
        {
            const int n = (int) std::min<size_t> (65536, in.l.size() - p);
            const int got = chain.push (in.l.data() + p, in.r.data() + p, n);
            out.l.insert (out.l.end(), chain.output (0).begin(), chain.output (0).begin() + got);
            out.r.insert (out.r.end(), chain.output (1).begin(), chain.output (1).begin() + got);
        }
        const int got = chain.flush();
        out.l.insert (out.l.end(), chain.output (0).begin(), chain.output (0).begin() + got);
        out.r.insert (out.r.end(), chain.output (1).begin(), chain.output (1).begin() + got);
        return out;
    }

    // A mono record whose right channel is upside down: flipped back and summed, out
    // comes the recording itself. The largest difference from it.
    double mono78 (double fs, const Stereo& in)
    {
        Stereo flipped { in.l, in.l };
        for (auto& v : flipped.r) v = -v;
        auto s = vs::restore::defaults ("78");
        s.rumble.on = false; s.rumble.dc = false; s.declick.on = false;
        s.channels.flip = true;
        const auto out = runChain (fs, s, flipped);
        double worst = 0;
        for (size_t i = 0; i < out.l.size(); ++i) worst = std::max ({ worst, (double) std::abs (out.l[i] - in.l[i]), (double) std::abs (out.r[i] - in.l[i]) });
        return worst;
    }

    void chain()
    {
        const double fs = 48000;
        auto in = band (fs, 4);
        // azimuth: the right channel arrives 0.37 samples after the left; lining them up
        Stereo tape { in.l, std::vector<float> (in.l.size(), 0.0f) };
        vs::FracDelay late ({ 0.37 });
        tape.r = tape.l;
        late.process (0, tape.r.data(), (int) tape.r.size());
        tape.r.erase (tape.r.begin(), tape.r.begin() + late.latency());
        tape.r.resize (tape.l.size(), 0.0f);
        auto s = vs::restore::defaults ("cassette");
        s.rumble.on = false; s.rumble.dc = false;
        s.channels.on = true; s.channels.alignOn = true; s.channels.delay = 0.37;
        const auto out = runChain (fs, s, tape);
        double diff = 0, finite = 1;
        for (size_t i = 2000; i + 2000 < out.l.size(); ++i) { diff = std::max (diff, (double) std::abs (out.l[i] - out.r[i])); if (! std::isfinite (out.l[i])) finite = 0; }
        expect ("lining up azimuth brings a channel 0.37 samples late back into line", out.l.size() == tape.l.size() && diff < 2e-3 && finite == 1,
                "(%.0f samples, largest difference %.2g)", (double) out.l.size(), diff);

        // a mono 78: summed, one channel turned upside down put right first
        const double m = mono78 (fs, in);
        expect ("a flipped channel is turned back and summed for a mono record", m < 1e-6, "(largest difference %.2g)", m);

        // speed: a fix of -100 cents gives 2^(1/12) times the samples
        auto sp = vs::restore::defaults ("other");
        sp.rumble.on = false; sp.rumble.dc = false;
        sp.speed.on = true; sp.speed.cents = -100;
        const auto slow = runChain (fs, sp, in);
        const double want = in.l.size() * std::pow (2.0, 100.0 / 1200);
        expect ("a speed fix of -100 cents makes it a semitone longer", std::abs ((double) slow.l.size() - want) < 64, "(%.0f samples, want %.0f)", (double) slow.l.size(), want);
        expect ("a 45 played at 33 is fixed by 45 / 33.3 (about 520 cents)", std::abs (vs::restore::rpmCents ("45at33") - 1200 * std::log2 (45 / (100.0 / 3))) < 1e-9,
                "(%.2f)", vs::restore::rpmCents ("45at33"));

        // what was removed: the chain run with and without the de-clicker lines up sample for sample
        auto clicked = in;
        addClicks (clicked, fs, 10, 9);
        auto lp = vs::restore::defaults ("lp");
        vs::restore::Run keep;
        keep.keepDelays = true;
        const auto with = runChain (fs, lp, clicked, keep);
        keep.offDeclick = true;
        const auto without = runChain (fs, lp, clicked, keep);
        expect ("with and without the de-clicker line up, so their difference is what it took out",
                with.l.size() == without.l.size() && rms (with.l) != rms (without.l), "(%.0f samples)", (double) with.l.size());

        // the settings round-trip through JSON
        vs::json::Out o;
        auto full = vs::restore::defaults ("78");
        full.dehum.on = true; full.dehum.harmonics = { 1, 2, 3 };
        vs::restore::settingsToJson (full, o);
        const auto back = vs::restore::settingsFromJson (*vs::json::parse (o.text));
        vs::json::Out o2;
        vs::restore::settingsToJson (back, o2);
        expect ("settings come back the same through JSON", o.text == o2.text, "");
    }

    // ---- the analysis

    void analysis()
    {
        const double fs = 48000;
        // 40 s: off by +18 cents, a 0.55 Hz wobble of 10 cents, songs with two 4 s gaps,
        // 60.4 Hz hum, hiss, clicks and a 60 ms dropout at 20 s
        auto s = band (fs, 40, 18, [] (double t) { return 10 * std::sin (twoPi * 0.55 * t); }, { { 12, 16 }, { 26, 30 } });
        addClicks (s, fs, 60, 21, 0.2);
        addHum (s, fs, 60.4, -46);
        addHiss (s, -58, 5);
        for (int i = (int) (20 * fs); i < (int) (20.06 * fs); ++i) { s.l[(size_t) i] *= 0.001f; s.r[(size_t) i] *= 0.001f; }
        const long n = (long) s.l.size();
        vs::restore::RestoreAnalyzer a (fs, n);
        for (long p = 0; p < n; p += 1 << 17) a.push (s.l.data() + p, s.r.data() + p, (int) std::min<long> (1 << 17, n - p));
        a.finish();
        expect ("the pass keeps 11-12 kHz copies and a 3000-column overview", a.lowRate() >= 11000 && a.lowRate() <= 12100 && a.overview().cols == 3000,
                "(%.0f Hz)", a.lowRate());

        const auto hum = vs::restore::detectHum (a.lowLeft(), a.lowRight(), a.lowRate());
        expect ("hum: found at 60.4 Hz with its harmonics", hum.found && std::abs (hum.hz - 60.4) < 0.1 && hum.harmonics.size() >= 3,
                "(%.2f Hz, %.0f harmonics)", hum.hz, (double) hum.harmonics.size());

        const auto t = vs::restore::detectTuning (a.lowMid(), a.lowRate());
        expect ("tuning: the transfer runs about 18 cents sharp", t.found && std::abs (t.cents - 18) < 6, "(%.1f cents, confidence %.2f)", t.cents, t.confidence);

        const auto w = vs::restore::detectWow (a.lowMid(), a.lowRate());
        expect ("wow: found, about 0.55 times a second", w.found && std::abs (w.hz - 0.55) < 0.1, "(%.2f Hz, peak %.1f cents)", w.hz, w.peak);
        expect ("and its curve covers the file, 50 values a second", std::abs ((double) w.cents.size() - 40 * 50) <= 2, "(%.0f values)", (double) w.cents.size());

        const auto tr = vs::restore::findTracks (a.lowMid(), a.lowRate(), 1.2, 5);
        expect ("songs: three, split in the two gaps", tr.tracks.size() == 3 && tr.gaps.size() == 2
                && tr.gaps[0].start >= 11.5 && tr.gaps[0].end <= 16.5 && tr.gaps[1].start >= 25.5 && tr.gaps[1].end <= 30.5,
                "(%.0f songs)", (double) tr.tracks.size());

        const auto spots = vs::restore::quietSpots (a.hopEnergy(), a.hopSize());
        int inGaps = 0;
        for (auto at : spots) { const double sec = (double) at / fs; if ((sec > 12 && sec < 16) || (sec > 26 && sec < 30)) ++inGaps; }
        expect ("the quietest spots are in the gaps between songs", ! spots.empty() && inGaps >= (int) spots.size() * 8 / 10, "(%.0f of %.0f)", inGaps, (double) spots.size());

        std::vector<double> humHz;
        for (const auto& h : hum.harmonics) humHz.push_back (h.hz);
        vs::NoiseProfileLearner learner (fs, a.frameSize(), humHz);
        for (auto at : spots) if (at + a.frameSize() <= n) learner.add (s.l.data() + at, s.r.data() + at);
        const auto prof = learner.profile();
        const double profDb = prof ? vs::denoise::profileDb (*prof, a.frameSize()) : 0;
        expect ("the noise profile, learned in the gaps, reads the hiss's level (hum bridged)", prof && std::abs (profDb - (-58)) < 2, "(%.1f dBFS)", profDb);

        const auto drops = vs::restore::detectDropouts (a.lowLeft(), a.lowRight(), a.lowRate(), prof ? &*prof : nullptr, a.frameSize(), fs);
        bool at20 = false;
        for (const auto& d : drops) if (d.start <= 20.06 && d.end >= 20.0) at20 = true;
        expect ("dropouts: the one at 20 s, in both channels", at20 && drops.size() <= 2, "(%.0f found)", (double) drops.size());

        // channels: the right 0.9 of the left and 0.4 samples late
        Stereo late { s.l, s.l };
        vs::FracDelay fd ({ 0.4 });
        fd.process (0, late.r.data(), (int) late.r.size());
        late.r.erase (late.r.begin(), late.r.begin() + fd.latency());
        late.r.resize (late.l.size(), 0.0f);
        for (auto& v : late.r) v *= 0.9f;
        vs::restore::ChannelCompare cc (fs);
        for (const auto& win : vs::restore::checkWindows (n, fs))
            if (win.at > fs && win.at + fs / 2 < n) cc.add (late.l.data() + win.at, late.r.data() + win.at, (int) fs / 2);
        const auto c = cc.result();
        expect ("channels: one recording, the right 0.4 samples late and 0.9 dB quieter", c.mono && std::abs (c.delay - 0.4) < 0.05 && std::abs (c.balanceDb - 0.92) < 0.1,
                "(delay %.2f, balance %.2f dB)", c.delay, c.balanceDb);

        vs::restore::ClickCounter counter (fs);
        for (const auto& win : vs::restore::checkWindows (n, fs)) counter.add (s.l.data() + win.at, s.r.data() + win.at, win.length, win.at);
        const auto clicks = counter.result();
        expect ("clicks: counted per minute in windows across the file", clicks.perMinute[0] > 20 && clicks.seconds > 15, "(%.0f a minute in %.1f s)", clicks.perMinute[0], clicks.seconds);

        vs::restore::LevelEnvelope env (fs);
        env.push (s.l.data(), s.r.data(), (int) n);
        const auto g = vs::restore::gapCandidates (env.levels(), env.hopSeconds());
        int hits = 0;
        for (const auto& cand : g.cands) if ((cand.at > 12 && cand.at < 16.5) || (cand.at > 26 && cand.at < 30.5)) ++hits;
        expect ("a side's level envelope: 20 levels a second, its best dips the two gaps", std::abs ((double) env.levels().size() - 800) <= 1 && hits == 2,
                "(%.0f levels, %.0f candidates)", (double) env.levels().size(), (double) g.cands.size());
        // recording a side: quarter-second batches of a lead-in, four songs with short gaps, the run-out
        auto run = [] (double floorDb, double musicDb, double seconds, double gapLen)
        {
            vs::restore::RunoutWatch q (seconds, 20);
            double t = 0, stopAt = -1;
            unsigned rnd = 1;
            auto r = [&] { rnd = rnd * 1664525u + 1013904223u; return rnd / 4294967296.0 - 0.5; };
            auto feed = [&] (double db) { t += 0.25; if (q.push (db + 2 * r(), 0.25) && stopAt < 0) stopAt = t; };
            for (int i = 0; i < 8; ++i) feed (floorDb);
            for (int song = 0; song < 4; ++song)
            {
                for (int i = 0; i < 160; ++i) feed (musicDb + 4 * std::sin (i / 3.0));
                if (song < 3) for (int i = 0; i < (int) (4 * gapLen); ++i) feed (floorDb);
            }
            const double end = t;
            for (int i = 0; i < 120; ++i) feed (floorDb + 1);
            return std::pair<double, double> { stopAt, end };
        };
        const auto lp = run (-55, -16, 15, 3), hiss78 = run (-34, -15, 15, 3), long12 = run (-52, -18, 15, 12);
        expect ("recording stops by itself 15 s into the run-out, hissy 78 too, never in a 12 s gap",
                lp.first > 0 && std::abs (lp.first - lp.second - 15) < 2.5 && hiss78.first > hiss78.second && long12.first > long12.second,
                "(LP %.2f s after the music)", lp.first - lp.second);
        vs::restore::RunoutWatch silent (5, 20);
        bool stopped = false;
        for (int i = 0; i < 240; ++i) stopped = silent.push (-140, 0.25) || stopped;
        expect ("and not before there has been music", ! stopped, "");
        expect ("an export over -0.1 dBFS is brought down to it, under it left alone",
                std::abs (20 * std::log10 (vs::restore::exportGain (1.0)) - (-1.1)) < 1e-9 && vs::restore::exportGain (-3) == 1.0, "");
    }
}

int main()
{
    gapFill();
    filters();
    declicker();
    noiseReduction();
    chain();
    analysis();
    std::printf ("%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
