// Offline checks for ENCORE. Strum's audio processing (BodyMatch, GuitarFixes,
// Doubler). Run: build/StrumTests_artefacts/StrumTests (see tests/CMakeLists.txt).
// The whole-plugin checks and --snapshot need the plugin's own sources, so they
// only build with ENCORE_STRUM_PLUGIN_TESTS=1 (the plugin repo does that).
// Everything here is synthesized (noise, Karplus-Strong strings) and measured;
// none of it says anything about how Strum sounds on a real guitar.
#include <juce_dsp/juce_dsp.h>
#include <cstdio>
#include <cfloat>
#include "dsp/BodyMatch.h"
#include "dsp/GuitarFixes.h"
#include "dsp/Doubler.h"
#ifndef ENCORE_STRUM_PLUGIN_TESTS
 #define ENCORE_STRUM_PLUGIN_TESTS 0
#endif
#if ENCORE_STRUM_PLUGIN_TESTS
 #include "PluginProcessor.h"
#endif

namespace
{
    constexpr double fs = 48000.0;
    int failures = 0;

    void expect (const juce::String& name, bool ok, const juce::String& detail = {})
    {
        std::printf ("%s %-58s %s\n", ok ? "PASS" : "FAIL", name.toRawUTF8(), detail.toRawUTF8());
        if (! ok) ++failures;
    }

    juce::String num (double v, int places = 2) { return juce::String (v, places); }
    size_t at (double sec) { return (size_t) (sec * fs); }

    std::vector<float> white (double sec, float amp, int seed)
    {
        juce::Random r (seed);
        std::vector<float> x (at (sec));
        for (auto& v : x) v = (r.nextFloat() * 2.0f - 1.0f) * amp;
        return x;
    }

    // Paul Kellet's pink noise filter (accurate to ~0.05 dB above 10 Hz)
    std::vector<float> pink (double sec, float amp, int seed)
    {
        juce::Random r (seed);
        std::vector<float> x (at (sec));
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        for (auto& v : x)
        {
            const double w = r.nextFloat() * 2.0 - 1.0;
            b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759;
            b2 = 0.96900 * b2 + w * 0.1538520; b3 = 0.86650 * b3 + w * 0.3104856;
            b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.0168980;
            v = (float) ((b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362) * 0.11 * amp);
            b6 = w * 0.115926;
        }
        return x;
    }

    // Karplus-Strong plucked string, added into out at startSec. The delay
    // line starts either as (lowpassed) white noise, the classic KS pluck, which
    // is far brighter than a real string (every harmonic equally loud), or, with
    // shaped = true, as the triangle a pick or finger leaves the string in
    // (plucked 1/5 of the way along, harmonics falling ~12 dB/octave) plus a
    // little noise.
    void pluck (std::vector<float>& out, double startSec, double hz, float amp, int seed, float loss = 0.9985f, float bright = 0.85f,
                bool shaped = false)
    {
        juce::Random r (seed);
        const int len = std::max (2, (int) std::lround (fs / hz));
        std::vector<float> line ((size_t) len);
        float lp = 0.0f;
        const float peakAt = 0.2f * (float) len;
        for (int i = 0; i < len; ++i)
        {
            const float w = r.nextFloat() * 2.0f - 1.0f;
            lp += bright * (w - lp);
            const float tri = (float) i < peakAt ? (float) i / peakAt : (float) (len - i) / ((float) len - peakAt);
            line[(size_t) i] = shaped ? 1.6f * tri + 0.05f * w : lp;
        }
        float mean = 0.0f;
        for (float v : line) mean += v;
        mean /= (float) len;
        for (auto& v : line) v -= mean; // no DC
        int p = 0;
        for (size_t i = at (startSec); i < out.size(); ++i)
        {
            const float y = line[(size_t) p];
            out[i] += amp * y;
            line[(size_t) p] = loss * 0.5f * (y + line[(size_t) ((p + 1) % len)]);
            p = (p + 1) % len;
        }
    }

    // A strum: the chord's strings plucked 8 ms apart, with the pick-shaped
    // pluck (or, realistic = false, the over-bright white-noise one)
    void strum (std::vector<float>& out, double startSec, const std::vector<double>& chord, float amp, int seed, bool realistic = true)
    {
        for (size_t s = 0; s < chord.size(); ++s)
            pluck (out, startSec + 0.008 * (double) s, chord[s], amp, seed + (int) s * 31, 0.9985f, 0.85f, realistic);
    }

    const std::vector<double> eMajor { 82.41, 123.47, 164.81, 207.65, 246.94, 329.63 };
    const std::vector<double> gMajor { 98.0, 123.47, 146.83, 196.0, 293.66, 392.0 };
    const std::vector<double> cMajor { 130.81, 164.81, 196.0, 261.63, 329.63 };
    const std::vector<double> dMajor { 146.83, 220.0, 293.66, 369.99 };
    const std::vector<double> aMinor { 110.0, 164.81, 220.0, 261.63, 329.63 };

    std::vector<float> filter (std::vector<float> x, const std::vector<vs::BiquadCoefs>& stages)
    {
        for (auto& c : stages)
        {
            vs::Biquad b;
            b.c = c;
            for (auto& v : x) v = b.process (0, v);
        }
        return x;
    }

    double rmsDb (const std::vector<float>& x, size_t from, size_t to)
    {
        to = std::min (to, x.size());
        double s = 0.0;
        for (size_t i = from; i < to; ++i) s += (double) x[i] * x[i];
        return 10.0 * std::log10 (s / (double) std::max<size_t> (1, to - from) + 1e-30);
    }

    // 4th-order band (two high-pass + two low-pass sections)
    std::vector<float> band (const std::vector<float>& x, double lo, double hi)
    {
        using B = vs::BiquadCoefs;
        return filter (x, { B::highPass (fs, lo, 0.7071), B::highPass (fs, lo, 0.7071), B::lowPass (fs, hi, 0.7071), B::lowPass (fs, hi, 0.7071) });
    }

    // Welch-averaged power spectrum (8192-point Hann frames, 50% overlap)
    std::vector<double> spectrum (const std::vector<float>& x, size_t from = 0)
    {
        constexpr int order = 13, n = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<float> buf ((size_t) n * 2);
        std::vector<double> p ((size_t) n / 2 + 1, 0.0);
        int frames = 0;
        for (size_t s = from; s + n <= x.size(); s += n / 2, ++frames)
        {
            for (int i = 0; i < n; ++i)
                buf[(size_t) i] = x[s + (size_t) i] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) n));
            std::fill (buf.begin() + n, buf.end(), 0.0f);
            fft.performFrequencyOnlyForwardTransform (buf.data(), true);
            for (int b = 0; b <= n / 2; ++b) p[(size_t) b] += (double) buf[(size_t) b] * buf[(size_t) b];
        }
        for (auto& v : p) v /= std::max (1, frames);
        return p;
    }

    // dB level of a spectrum in a 1/3-octave band around hz
    double thirdOctDb (const std::vector<double>& p, double hz)
    {
        const double binHz = fs / ((p.size() - 1) * 2);
        const int b1 = std::max (1, (int) std::lround (hz * std::pow (2.0, -1.0 / 6.0) / binHz));
        const int b2 = std::max (b1, (int) std::lround (hz * std::pow (2.0, 1.0 / 6.0) / binHz));
        double s = 0.0;
        for (int b = b1; b <= b2; ++b) s += p[(size_t) b];
        return 10.0 * std::log10 (s / (b2 - b1 + 1) + 1e-30);
    }

    std::vector<double> thirdOctaves (double lo, double hi)
    {
        std::vector<double> f;
        for (double v = lo; v <= hi * 1.001; v *= std::pow (2.0, 1.0 / 3.0)) f.push_back (v);
        return f;
    }

    // The "mic" in the Learn tests: a made-up but mic-like difference from the DI
    std::vector<vs::BiquadCoefs> knownEq()
    {
        using B = vs::BiquadCoefs;
        return { B::lowShelf (fs, 150.0, 0.7, -4.0), B::peak (fs, 600.0, 1.5, -5.0), B::peak (fs, 2500.0, 1.2, 6.0), B::highShelf (fs, 8000.0, 0.7, 3.0) };
    }
    double knownEqDb (double hz)
    {
        double d = 0.0;
        for (auto& c : knownEq()) d += c.magnitudeDb (hz, fs);
        return d;
    }
    constexpr float micGain = 0.7f; // the mic arrives 3.1 dB quieter...
    constexpr int micDelay = 96;    // ...and 2 ms late (a mic ~70 cm from the guitar)

    std::vector<float> makeMic (const std::vector<float>& di)
    {
        auto m = filter (di, knownEq());
        std::vector<float> out (m.size(), 0.0f);
        for (size_t i = (size_t) micDelay; i < m.size(); ++i) out[i] = micGain * m[i - (size_t) micDelay];
        return out;
    }

    vs::BodyCurve runLearner (const std::vector<float>& di, const std::vector<float>& mic, float& secondsUsed)
    {
        vs::BodyLearner L;
        L.prepare (fs);
        L.requestStart();
        vs::BodyCurve c;
        for (size_t s = 0; s < di.size(); s += 512)
        {
            const int len = (int) std::min<size_t> (512, di.size() - s);
            L.process (di.data() + s, mic.data() + s, len);
            if (L.takeResult (c)) break;
        }
        secondsUsed = c.seconds;
        return c;
    }

    std::vector<float> applyBody (const vs::BodyCurve& c, float amount, const std::vector<float>& x)
    {
        vs::BodyFilter f;
        f.setCurve (c, amount, fs);
        f.prepare ({ fs, 512, 2 });
        std::vector<float> l = x, r = x;
        for (size_t s = 0; s < x.size(); s += 512)
        {
            const int len = (int) std::min<size_t> (512, x.size() - s);
            float* ch[2] { l.data() + s, r.data() + s };
            f.process (ch, len);
        }
        return l;
    }

    template <typename Stage>
    std::vector<float> runStage (Stage& st, std::vector<float> x, float amount, bool on = true)
    {
        st.prepare (fs);
        for (size_t s = 0; s < x.size(); s += 512)
        {
            const int len = (int) std::min<size_t> (512, x.size() - s);
            float* ch[1] { x.data() + s };
            st.process (ch, 1, len, amount, on);
        }
        return x;
    }

    //==============================================================================
    void learnTests()
    {
        std::printf ("\n-- Body: Learn\n");
        const auto freqs = thirdOctaves (100.0, 10000.0);

        auto check = [&] (const char* what, const std::vector<float>& di, double tol)
        {
            const auto mic = makeMic (di);
            float used = 0.0f;
            const auto c = runLearner (di, mic, used);
            expect (juce::String (what) + ": learning finishes on its own", c.valid && used >= 29.0f, num (used, 1) + " s of active playing used");

            // learned curve + its level offset vs. the true EQ + gain
            double worst = 0.0, worstHz = 0.0;
            for (double f : freqs)
            {
                const double err = (c.at ((float) f) + c.levelDb) - (knownEqDb (f) + 20.0 * std::log10 (micGain));
                if (std::abs (err) > std::abs (worst)) { worst = err; worstHz = f; }
            }
            expect (juce::String (what) + ": learned curve = known EQ, 100 Hz-10 kHz (1/3 oct)", std::abs (worst) < tol,
                    "worst " + num (worst) + " dB at " + num (worstHz, 0) + " Hz; level found " + num (c.levelDb) + " dB");

            // DI through the designed filter vs. the mic (level offset removed), fresh audio
            const auto di2 = pink (12.0, 0.5f, 77);
            const auto out = applyBody (c, 1.0f, di2);
            const auto mic2 = makeMic (di2);
            const auto po = spectrum (out, at (0.5)), pm = spectrum (mic2, at (0.5));
            double worst2 = 0.0, worst2Hz = 0.0;
            for (double f : freqs)
            {
                const double err = thirdOctDb (po, f) - (thirdOctDb (pm, f) - c.levelDb);
                if (std::abs (err) > std::abs (worst2)) { worst2 = err; worst2Hz = f; }
            }
            expect (juce::String (what) + ": DI through Body matches the mic's spectrum", std::abs (worst2) < tol,
                    "worst " + num (worst2) + " dB at " + num (worst2Hz, 0) + " Hz (1/3-octave bands)");
            return c;
        };

        const auto c = check ("pink noise DI", pink (40.0, 0.5f, 5), 1.0);

        // a plucked-string DI: random notes on six strings over 40 s
        {
            std::vector<float> di (at (40.0), 0.0f);
            juce::Random r (11);
            for (double t = 0.0; t < 39.5; t += 0.25)
                pluck (di, t, 82.41 * std::pow (2.0, r.nextInt (30) / 12.0), 0.3f, r.nextInt(), 0.998f, 0.9f);
            check ("plucked-string DI", di, 1.5);
        }

        // nothing is learned from a DI with a silent mic
        {
            vs::BodyLearner L;
            L.prepare (fs);
            L.requestStart();
            const auto di = pink (10.0, 0.5f, 9);
            const std::vector<float> silent (di.size(), 0.0f);
            for (size_t s = 0; s < di.size(); s += 512)
                L.process (di.data() + s, silent.data() + s, (int) std::min<size_t> (512, di.size() - s));
            expect ("silent mic: no progress, nothing learned", L.getProgress() == 0.0f && L.isLearning(),
                    "progress " + num (L.getProgress() * 100.0f, 0) + "% after 10 s");
            L.requestStop();
            L.process (di.data(), silent.data(), 512);
            vs::BodyCurve dummy;
            expect ("stopping with too little playing gives up cleanly", ! L.isLearning() && ! L.takeResult (dummy)
                    && L.getStatus() == vs::BodyLearner::tooShort);
        }

        // Amount scales the curve: 50% gives half the dB
        {
            const auto di = pink (8.0, 0.5f, 21);
            const auto full = spectrum (applyBody (c, 1.0f, di), at (0.5)), half = spectrum (applyBody (c, 0.5f, di), at (0.5)),
                       none = spectrum (applyBody (c, 0.0f, di), at (0.5)), in = spectrum (di, at (0.5));
            double worst = 0.0, worstFlat = 0.0;
            for (double f : freqs)
            {
                const double dFull = thirdOctDb (full, f) - thirdOctDb (in, f), dHalf = thirdOctDb (half, f) - thirdOctDb (in, f);
                worst = std::max (worst, std::abs (dHalf - 0.5 * dFull));
                worstFlat = std::max (worstFlat, std::abs (thirdOctDb (none, f) - thirdOctDb (in, f)));
            }
            expect ("Amount 50% = half the correction (dB)", worst < 0.3, "worst " + num (worst) + " dB off");
            expect ("Amount 0% is flat", worstFlat < 0.1, "worst " + num (worstFlat, 3) + " dB");
        }

        // minimum phase: the filter's energy is at the very start (no delay to report)
        {
            const auto ir = vs::BodyFilter::design (c, 1.0f, fs);
            int peak = 0;
            double total = 0.0, first = 0.0;
            for (int i = 0; i < ir.getNumSamples(); ++i)
            {
                const double v = ir.getSample (0, i);
                total += v * v;
                if (i < 48) first += v * v; // first 1 ms
                if (std::abs (v) > std::abs (ir.getSample (0, peak))) peak = i;
            }
            expect ("Body filter is minimum phase (peak at start, no pre-ring)", peak < 8 && first / total > 0.8,
                    "peak at sample " + juce::String (peak) + ", " + num (100.0 * first / total, 1) + "% of energy in the first 1 ms");
        }

        // the generic curve is what it says and stays modest
        {
            const auto g = vs::BodyCurve::generic();
            float lo = 0, hi = 0;
            for (float v : g.db) { lo = std::min (lo, v); hi = std::max (hi, v); }
            expect ("generic curve: cut near 1.6 kHz, lift near 220 Hz, within +-5 dB",
                    g.at (1600.0f) < -3.0f && g.at (220.0f) > 1.5f && lo > -5.0f && hi < 5.0f,
                    "220 Hz " + num (g.at (220.0f), 1) + ", 1.6 kHz " + num (g.at (1600.0f), 1) + ", 12 kHz " + num (g.at (12000.0f), 1) + " dB");
        }

        // text round-trip (what the plugin state stores)
        {
            const auto back = vs::BodyCurve::fromString (c.toString());
            float worst = 0.0f;
            for (int i = 0; i < vs::BodyCurve::numPoints; ++i) worst = std::max (worst, std::abs (back.db[(size_t) i] - c.db[(size_t) i]));
            expect ("curve survives save/load as text", back.valid && worst < 0.006f && std::abs (back.levelDb - c.levelDb) < 0.006f,
                    "max diff " + num (worst, 4) + " dB");
            expect ("garbage text loads as no curve", ! vs::BodyCurve::fromString ("1 2 nonsense").valid);
        }
    }

    //==============================================================================
    void squeakTests()
    {
        std::printf ("\n-- Squeak\n");
        const double len = 7.2, period = 0.6;
        std::vector<float> chord (at (len), 0.0f);
        int strums = 0;
        for (double t = 0.0; t < len - 0.3; t += period, ++strums)
            strum (chord, t, strums % 2 ? gMajor : eMajor, 0.12f, 100 + strums);

        // squeaks: noise through a 4th-order bandpass (two Q 2 sections) gliding
        // 2 -> 5 kHz over 50 ms, so it's "mostly 1.5-8 kHz" like a real one,
        // late in every other chord (where a chord change would be)
        std::vector<float> squeaks (chord.size(), 0.0f);
        std::vector<std::pair<size_t, size_t>> windows;
        juce::Random r (4);
        for (double t = 0.42; t < len - 0.3; t += period * 2)
        {
            const size_t s0 = at (t), n = at (0.05);
            vs::Biquad bp1, bp2;
            for (size_t i = 0; i < n; ++i)
            {
                const double f = 2000.0 * std::pow (2.5, (double) i / (double) n);
                bp1.c = bp2.c = vs::BiquadCoefs::bandPass (fs, f, 2.0);
                const float env = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) n);
                squeaks[s0 + i] = 0.7f * env * bp2.process (0, bp1.process (0, r.nextFloat() * 2.0f - 1.0f));
            }
            windows.push_back ({ s0, s0 + n });
        }

        auto mix = chord;
        for (size_t i = 0; i < mix.size(); ++i) mix[i] += squeaks[i];

        auto bandIn = band (mix, 1500, 8000);
        vs::SqueakTamer sq;
        const auto out = runStage (sq, mix, 0.5f);
        const auto bandOut = band (out, 1500, 8000);

        double worst = 1e9, avg = 0.0;
        for (auto [a, b] : windows)
        {
            const double red = rmsDb (bandIn, a, b) - rmsDb (bandOut, a, b);
            worst = std::min (worst, red);
            avg += red / (double) windows.size();
        }
        // how loud the squeaks were against the chord in their windows
        double sqOverChord = 0.0;
        {
            const auto bs = band (squeaks, 1500, 8000), bc = band (chord, 1500, 8000);
            for (auto [a, b] : windows) sqOverChord += (rmsDb (bs, a, b) - rmsDb (bc, a, b)) / (double) windows.size();
        }
        expect ("squeak bursts (1.5-8 kHz) come down by >= 6 dB", worst >= 6.0,
                "average " + num (avg, 1) + " dB, least " + num (worst, 1) + " dB (" + juce::String ((int) windows.size())
                + " squeaks, " + num (sqOverChord, 1) + " dB over the chord)");

        // Quieter squeaks: where detection gives up (it needs a 13 dB jump at 50%).
        // Measured and reported, not pass/fail.
        for (float gainDb : { -6.0f, -12.0f, -18.0f })
        {
            auto quiet = chord;
            for (size_t i = 0; i < quiet.size(); ++i) quiet[i] += vs::dbToGain (gainDb) * squeaks[i];
            vs::SqueakTamer sqq;
            const auto qo = runStage (sqq, quiet, 0.5f);
            const auto qi = band (quiet, 1500, 8000), qb = band (qo, 1500, 8000), bsq = band (squeaks, 1500, 8000), bch = band (chord, 1500, 8000);
            double red = 0.0, over = 0.0;
            for (auto [a, b] : windows)
            {
                red += (rmsDb (qi, a, b) - rmsDb (qb, a, b)) / (double) windows.size();
                over += (rmsDb (bsq, a, b) + gainDb - rmsDb (bch, a, b)) / (double) windows.size();
            }
            std::printf ("INFO %-58s %s\n", ("squeaks " + num (over, 1) + " dB over the chord (1.5-8 kHz)").toRawUTF8(),
                         ("come down " + num (red, 1) + " dB on average").toRawUTF8());
        }

        // the strummed chord alone: untouched
        vs::SqueakTamer sq2;
        const auto chordOut = runStage (sq2, chord, 0.5f);
        const double dBand = rmsDb (band (chordOut, 1500, 8000), 0, chord.size()) - rmsDb (band (chord, 1500, 8000), 0, chord.size());
        const double dAll = rmsDb (chordOut, 0, chord.size()) - rmsDb (chord, 0, chord.size());
        // and strum by strum (attacks are where a wrong trigger would show)
        double worstStrum = 0.0;
        const auto bc = band (chord, 1500, 8000), bco = band (chordOut, 1500, 8000);
        for (int k = 0; k < strums; ++k)
        {
            const size_t a = at (k * period), b = at (k * period + 0.1);
            worstStrum = std::max (worstStrum, std::abs (rmsDb (bco, a, b) - rmsDb (bc, a, b)));
        }
        expect ("strummed chords alone change < 0.5 dB (1.5-8 kHz and overall)", std::abs (dBand) < 0.5 && std::abs (dAll) < 0.5,
                "band " + num (dBand, 3) + " dB, overall " + num (dAll, 3) + " dB");
        expect ("strum attacks (first 100 ms of each) change < 0.5 dB", worstStrum < 0.5, "worst strum " + num (worstStrum, 3) + " dB");

        // Over-bright strums (white-noise KS excitation, harsher than a real
        // guitar): for their first ~20 ms they add nothing below 1.2 kHz, so
        // they look exactly like a squeak. Measured and reported, not pass/fail.
        {
            std::vector<float> bright (at (len), 0.0f);
            for (int k = 0; k < strums; ++k) strum (bright, k * period, k % 2 ? gMajor : eMajor, 0.12f, 500 + k, false);
            vs::SqueakTamer sqb;
            const auto bo = runStage (sqb, bright, 0.5f);
            const auto bb = band (bright, 1500, 8000), bbo = band (bo, 1500, 8000);
            double worstB = 0.0;
            for (int k = 0; k < strums; ++k)
                worstB = std::max (worstB, std::abs (rmsDb (bbo, at (k * period), at (k * period + 0.1)) - rmsDb (bb, at (k * period), at (k * period + 0.1))));
            std::printf ("INFO %-58s %s\n", "white-noise KS strums (unrealistically bright): attacks", ("worst strum " + num (worstB, 2) + " dB, " + juce::String (worstB < 0.5 ? "untouched" : "partly cut as squeaks")).toRawUTF8());
        }

        // full amount
        vs::SqueakTamer sq3;
        const auto chordOutMax = runStage (sq3, chord, 1.0f);
        const double dMax = rmsDb (band (chordOutMax, 1500, 8000), 0, chord.size()) - rmsDb (bc, 0, chord.size());
        expect ("at 100% the chord alone still changes < 0.5 dB", std::abs (dMax) < 0.5, num (dMax, 3) + " dB");

        // switched off: bit-exact
        vs::SqueakTamer sq4;
        const auto offOut = runStage (sq4, mix, 1.0f, false);
        double err = 0.0;
        for (size_t i = 0; i < mix.size(); ++i) err = std::max (err, (double) std::abs (offOut[i] - mix[i]));
        expect ("Squeak off is bit-exact pass-through", err == 0.0, "max diff " + num (err, 9));
    }

    //==============================================================================
    void boomTests()
    {
        std::printf ("\n-- Boom\n");
        const double len = 14.0;
        auto progression = [&] (int seed)
        {
            std::vector<float> x (at (len), 0.0f);
            const std::vector<double>* chords[] { &gMajor, &cMajor, &dMajor, &eMajor, &aMinor };
            int k = 0;
            for (double t = 0.0; t < len - 0.5; t += 0.5, ++k)
                strum (x, t, *chords[(k / 4) % 5], 0.1f, seed + k);
            return x;
        };
        const auto resonance = vs::BiquadCoefs::peak (fs, 110.0, 4.0, 12.0);
        const size_t from = at (6.0);

        auto runBoom = [&] (const char* what, const std::vector<float>& x, bool expectCut)
        {
            vs::BoomTamer bt;
            auto out = x;
            bt.prepare (fs);
            for (size_t s = 0; s < out.size(); s += 512)
            {
                float* ch[1] { out.data() + s };
                bt.process (ch, 1, (int) std::min<size_t> (512, out.size() - s), 0.4f, true);
            }
            const double d110 = rmsDb (band (out, 90, 135), from, out.size()) - rmsDb (band (x, 90, 135), from, x.size());
            const double dAll = rmsDb (out, from, out.size()) - rmsDb (x, from, x.size());
            const juce::String found = bt.hasFound() ? num (bt.getFoundHz(), 1) + " Hz (" + num (bt.getProminenceDb(), 1) + " dB prominent)" : juce::String ("nothing");
            if (expectCut)
            {
                expect (juce::String (what) + ": finds the 110 Hz resonance (+-10%)", bt.hasFound() && std::abs (bt.getFoundHz() / 110.0 - 1.0) < 0.1, "found " + found);
                expect (juce::String (what) + ": cuts it (90-135 Hz down >= 3 dB)", d110 <= -3.0,
                        num (d110, 2) + " dB at 110 Hz, " + num (dAll, 2) + " dB overall");
            }
            else
            {
                expect (juce::String (what) + ": no cut (< 0.5 dB anywhere measured)", std::abs (d110) < 0.5 && std::abs (dAll) < 0.5,
                        num (d110, 3) + " dB at 110 Hz, " + num (dAll, 3) + " dB overall; found " + found);
            }
        };

        const auto strums = progression (300);
        runBoom ("strummed chords + 110 Hz body boom", filter (strums, { resonance }), true);
        runBoom ("same chords, no boom", strums, false);
        const auto noise = pink (len, 0.3f, 8);
        runBoom ("pink noise + 110 Hz boom", filter (noise, { resonance }), true);
        runBoom ("pink noise, no boom", noise, false);
    }

    //==============================================================================
    void pickTests()
    {
        std::printf ("\n-- Pick\n");
        const double period = 0.5;
        std::vector<float> x (at (6.0), 0.0f);
        int notes = 0;
        for (double t = 0.0; t < 5.5; t += period, ++notes)
            pluck (x, t, 196.0 * std::pow (2.0, (notes % 5) / 12.0), 0.35f, 900 + notes, 0.999f, 0.95f);

        auto measure = [&] (float amount, double& attackRed, double& sustainChange)
        {
            vs::PickTamer pt;
            const auto out = runStage (pt, x, amount);
            const auto bi = band (x, 2000, 6000), bo = band (out, 2000, 6000);
            attackRed = 0.0; sustainChange = 0.0;
            for (int k = 0; k < notes; ++k)
            {
                const size_t s = at (k * period);
                // attack: loudest 2 ms in the first 30 ms
                double pi = -300, po = -300;
                for (size_t w = s; w < s + at (0.03); w += at (0.001))
                {
                    pi = std::max (pi, rmsDb (bi, w, w + at (0.002)));
                    po = std::max (po, rmsDb (bo, w, w + at (0.002)));
                }
                attackRed += (pi - po) / notes;
                sustainChange = std::max (sustainChange, std::abs (rmsDb (bo, s + at (0.12), s + at (0.48)) - rmsDb (bi, s + at (0.12), s + at (0.48))));
            }
        };

        double red = 0, sus = 0;
        measure (0.5f, red, sus);
        expect ("Pick 50%: attack peak (2-6 kHz) comes down >= 2 dB", red >= 2.0, "average " + num (red, 2) + " dB");
        expect ("Pick 50%: the ringing part changes < 0.5 dB", sus < 0.5, "worst note " + num (sus, 3) + " dB (120-480 ms after the pick)");
        measure (1.0f, red, sus);
        expect ("Pick 100%: attack peak comes down more", red >= 3.0, "average " + num (red, 2) + " dB");
        expect ("Pick 100%: the ringing part changes < 0.5 dB", sus < 0.5, "worst note " + num (sus, 3) + " dB");
    }

    //==============================================================================
    void doubleTests()
    {
        std::printf ("\n-- Double\n");
        const auto in = white (10.0, 0.3f, 12);
        auto run = [&] (float width, std::vector<float>& l, std::vector<float>& r)
        {
            vs::Doubler d;
            d.prepare (fs);
            l = in; r = in;
            for (size_t s = 0; s < in.size(); s += 512)
                d.process (l.data() + s, r.data() + s, (int) std::min<size_t> (512, in.size() - s), width);
        };

        std::vector<float> l, r;
        run (1.0f, l, r);
        std::vector<float> mono (in.size());
        for (size_t i = 0; i < in.size(); ++i) mono[i] = 0.5f * (l[i] + r[i]);
        const size_t from = at (0.5);
        const double loss = rmsDb (mono, from, mono.size()) - rmsDb (in, from, in.size());
        expect ("full width: mono sum loses <= 3 dB", loss > -3.0, num (loss, 2) + " dB");

        // mono transfer function, 1/12 octave, 50 Hz-16 kHz: any comb would show as deep dips
        const auto pi = spectrum (in, from), pmono = spectrum (mono, from);
        double lo = 1e9, hi = -1e9;
        for (double f = 50.0; f <= 16000.0; f *= std::pow (2.0, 1.0 / 12.0))
        {
            const double binHz = fs / 8192.0;
            const int b1 = std::max (1, (int) std::lround (f * 0.97 / binHz)), b2 = std::max (b1, (int) std::lround (f * 1.03 / binHz));
            double a = 0, b = 0;
            for (int k = b1; k <= b2; ++k) { a += pmono[(size_t) k]; b += pi[(size_t) k]; }
            const double d = 10.0 * std::log10 (a / b);
            lo = std::min (lo, d); hi = std::max (hi, d);
        }
        expect ("full width: mono sum has no comb nulls (no dip > 1 dB below its level)", lo - loss > -1.0 && hi - lo < 1.5,
                "mono response spans " + num (lo, 2) + " to " + num (hi, 2) + " dB");

        // and it does make it wider
        double sl = 0, sr = 0, slr = 0;
        for (size_t i = from; i < in.size(); ++i) { sl += (double) l[i] * l[i]; sr += (double) r[i] * r[i]; slr += (double) l[i] * r[i]; }
        const double corr = slr / std::sqrt (sl * sr);
        const double side = rmsDb (l, from, l.size()) - rmsDb (in, from, in.size());
        expect ("full width: left/right decorrelated (correlation < 0.5)", corr < 0.5,
                "L/R correlation " + num (corr, 3) + ", each side " + num (side, 2) + " dB vs input");

        // width 0: untouched
        run (0.0f, l, r);
        double err = 0.0;
        for (size_t i = 0; i < in.size(); ++i) err = std::max (err, (double) std::max (std::abs (l[i] - in[i]), std::abs (r[i] - in[i])));
        expect ("width 0 is bit-exact pass-through", err == 0.0, "max diff " + num (err, 9));
    }

#if ENCORE_STRUM_PLUGIN_TESTS
    //==============================================================================
    // The whole plugin, as a host would run it
    struct Host
    {
        StrumProcessor p;
        bool sidechain;

        explicit Host (bool withSidechain, double rate = fs, int block = 512) : sidechain (withSidechain)
        {
            if (withSidechain)
            {
                auto layout = p.getBusesLayout();
                layout.inputBuses.getReference (1) = juce::AudioChannelSet::stereo();
                p.setBusesLayout (layout);
            }
            p.prepareToPlay (rate, block);
        }

        void set (const char* id, float v)
        {
            auto* param = p.apvts.getParameter (id);
            param->setValueNotifyingHost (param->convertTo0to1 (v));
        }

        // runs mono input (and optional mic) through, returns L and R
        void run (const std::vector<float>& in, const std::vector<float>* mic, std::vector<float>& l, std::vector<float>& r, int block = 512)
        {
            l.assign (in.size(), 0.0f); r.assign (in.size(), 0.0f);
            juce::AudioBuffer<float> buf (sidechain ? 4 : 2, block);
            juce::MidiBuffer midi;
            for (size_t s = 0; s < in.size(); s += (size_t) block)
            {
                const int len = (int) std::min<size_t> ((size_t) block, in.size() - s);
                juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), buf.getNumChannels(), len);
                for (int c = 0; c < 2; ++c) view.copyFrom (c, 0, in.data() + s, len);
                if (sidechain)
                    for (int c = 2; c < 4; ++c)
                    {
                        if (mic != nullptr) view.copyFrom (c, 0, mic->data() + s, len);
                        else view.clear (c, 0, len);
                    }
                p.processBlock (view, midi);
                std::copy (view.getReadPointer (0), view.getReadPointer (0) + len, l.begin() + (long) s);
                std::copy (view.getReadPointer (1), view.getReadPointer (1) + len, r.begin() + (long) s);
            }
        }
    };

    void pluginTests()
    {
        std::printf ("\n-- Whole plugin\n");

        {
            Host h (true);
            expect ("reports zero latency", h.p.getLatencySamples() == 0, juce::String (h.p.getLatencySamples()) + " samples");
        }

        // Learn through the plugin itself, then save, reload, and use it
        juce::MemoryBlock saved;
        vs::BodyCurve learnedHere;
        {
            Host h (true);
            const auto di = pink (34.0, 0.5f, 31);
            const auto mic = makeMic (di);
            h.p.toggleLearn();
            std::vector<float> l, r;
            h.run (di, &mic, l, r);
            h.p.service(); // what the plugin's timer does
            learnedHere = h.p.getLearned();
            expect ("plugin Learn: finishes and switches Body to Learned",
                    learnedHere.valid && h.p.isUsingLearned() && h.p.apvts.state.hasProperty ("learnedCurve"),
                    num (learnedHere.seconds, 1) + " s learned");

            h.set ("boom", 0.77f);
            h.set ("double_on", 1.0f);
            h.set ("width", 0.33f);
            h.p.getStateInformation (saved);
        }
        {
            Host h (false);
            h.p.setStateInformation (saved.getData(), (int) saved.getSize());
            const auto& c = h.p.getLearned();
            float worst = 0.0f;
            for (int i = 0; i < vs::BodyCurve::numPoints; ++i) worst = std::max (worst, std::abs (c.db[(size_t) i] - learnedHere.db[(size_t) i]));
            const bool params = std::abs (h.p.apvts.getRawParameterValue ("boom")->load() - 0.77f) < 0.001f
                             && h.p.apvts.getRawParameterValue ("double_on")->load() > 0.5f
                             && std::abs (h.p.apvts.getRawParameterValue ("width")->load() - 0.33f) < 0.001f
                             && (int) h.p.apvts.getRawParameterValue ("body_curve")->load() == 1;
            expect ("project save/load: learned curve and settings come back", c.valid && worst < 0.006f && params,
                    "curve max diff " + num (worst, 4) + " dB");

            // the reloaded plugin really applies it: DI-only session, Body on.
            // (Hosts call prepareToPlay after loading a project; that's also
            // what makes JUCE's convolution finish loading the new filter now
            // instead of a few ms later on its background thread.)
            h.p.prepareToPlay (fs, 512);
            h.set ("body_on", 1.0f);
            h.set ("boom_on", 0.0f); h.set ("squeak_on", 0.0f); h.set ("pick_on", 0.0f); h.set ("double_on", 0.0f);
            const auto di = pink (10.0, 0.5f, 55);
            std::vector<float> l, r;
            h.run (di, nullptr, l, r);
            const auto mic = makeMic (di);
            const auto po = spectrum (l, at (2.0)), pm = spectrum (mic, at (2.0));
            double worst2 = 0.0;
            for (double f : thirdOctaves (100.0, 10000.0))
                worst2 = std::max (worst2, std::abs (thirdOctDb (po, f) - (thirdOctDb (pm, f) - c.levelDb)));
            expect ("reloaded project, DI only: output matches the mic within 1 dB", worst2 < 1.0, "worst " + num (worst2) + " dB (1/3 oct, 100 Hz-10 kHz)");
        }

        // user preset: saving keeps the curve; loading it into a fresh plugin brings it back
        {
            Host a (false);
            a.p.setStateInformation (saved.getData(), (int) saved.getSize());
            const juce::String name ("_strum_selftest");
            const bool ok = a.p.presetManager.save (name);
            Host b (false);
            b.p.forgetLearned();
            const bool loaded = b.p.presetManager.load (name);
            b.p.service();
            float worst = 0.0f;
            for (int i = 0; i < vs::BodyCurve::numPoints; ++i) worst = std::max (worst, std::abs (b.p.getLearned().db[(size_t) i] - learnedHere.db[(size_t) i]));
            expect ("user preset carries the learned curve (\"My guitar, mic\")", ok && loaded && b.p.getLearned().valid && worst < 0.006f,
                    "max diff " + num (worst, 4) + " dB");

            // a preset without a curve doesn't throw away the one you just learned
            b.p.forgetLearned();
            b.p.presetManager.save (name + "_plain");
            Host c (false);
            c.p.setStateInformation (saved.getData(), (int) saved.getSize());
            c.p.presetManager.load (name + "_plain");
            expect ("loading a preset without a curve keeps the learned one", c.p.getLearned().valid);

            a.p.presetManager.remove (name);
            a.p.presetManager.remove (name + "_plain");
        }

        // stress: everything on, nasty inputs, two sample rates and block sizes
        for (double rate : { 44100.0, 96000.0 })
        {
            Host h (true, rate, 333);
            h.set ("body_on", 1.0f); h.set ("double_on", 1.0f); h.set ("width", 1.0f);
            h.set ("boom", 1.0f); h.set ("squeak", 1.0f); h.set ("pick", 1.0f);
            h.p.service();
            const size_t n = (size_t) (rate * 3.0);
            juce::Random rnd (2);
            std::vector<float> silence (n, 0.0f), dc (n, 0.5f), loud (n);
            for (auto& v : loud) v = rnd.nextFloat() * 2.0f - 1.0f;

            auto check = [&] (const char* what, const std::vector<float>& in, float maxPeak)
            {
                std::vector<float> l, r;
                h.run (in, &in, l, r, 333);
                bool finite = true;
                int denormals = 0;
                float peak = 0.0f;
                for (auto* ch : { &l, &r })
                    for (float v : *ch)
                    {
                        finite = finite && std::isfinite (v);
                        if (v != 0.0f && std::abs (v) < FLT_MIN) ++denormals;
                        peak = std::max (peak, std::abs (v));
                    }
                expect (juce::String (what) + " @ " + num (rate / 1000.0, 1) + " kHz: finite, no denormals, no blow-up",
                        finite && denormals == 0 && peak <= maxPeak, "peak " + num (peak, 3) + ", denormals " + juce::String (denormals));
                return peak;
            };
            check ("full-scale noise", loud, 4.0f);
            check ("DC 0.5", dc, 2.0f);
            const float silentPeak = check ("silence after all that", silence, 1.0f);
            std::vector<float> l, r;
            h.run (silence, &silence, l, r, 333);
            float tail = 0.0f;
            for (size_t i = n / 2; i < n; ++i) tail = std::max (tail, std::max (std::abs (l[i]), std::abs (r[i])));
            expect (juce::String ("silence stays silent @ ") + num (rate / 1000.0, 1) + " kHz", tail < 1.0e-6f,
                    "peak " + num (vs::ampToDb (tail), 1) + " dBFS (first silent pass " + num (vs::ampToDb (silentPeak), 1) + ")");
        }

        // Hear Original: the untouched input, at the processed level
        {
            Host h (false);
            h.set ("body_on", 1.0f); h.set ("output", 6.0f);
            h.p.service();
            const auto di = pink (6.0, 0.3f, 66);
            std::vector<float> lp, rp, lo, ro;
            h.run (di, nullptr, lp, rp);
            h.set ("compare", 1.0f);
            h.run (di, nullptr, lo, ro);
            const double processed = rmsDb (lp, at (3.0), lp.size()), original = rmsDb (lo, at (3.0), lo.size());
            const auto po = spectrum (lo, at (1.0)), pi = spectrum (di, at (1.0));
            double shape = 0.0;
            const double offset = thirdOctDb (po, 1000.0) - thirdOctDb (pi, 1000.0);
            for (double f : thirdOctaves (100.0, 10000.0))
                shape = std::max (shape, std::abs (thirdOctDb (po, f) - thirdOctDb (pi, f) - offset));
            expect ("Hear Original: untouched tone, matched loudness (< 1 dB)", std::abs (processed - original) < 1.0 && shape < 0.3,
                    "processed " + num (processed) + " dB, original " + num (original) + " dB, tone deviation " + num (shape) + " dB");
        }
    }
#endif
}

#if ENCORE_STRUM_PLUGIN_TESTS
// StrumTests --snapshot <folder>: renders the editor offscreen to PNGs (generic
// curve, then after a Learn), to check the layout without opening an audio device.
static int snapshot (const juce::File& folder)
{
    folder.createDirectory();
    Host h (true);
    h.set ("body_on", 1.0f); h.set ("double_on", 1.0f);
    auto save = [&] (const char* name)
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (h.p.createEditor());
        const auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
        juce::FileOutputStream out (folder.getChildFile (name));
        out.setPosition (0); out.truncate();
        juce::PNGImageFormat().writeImageToStream (img, out);
        std::printf ("wrote %s (%d x %d)\n", folder.getChildFile (name).getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());
    };
    save ("strum-generic.png");
    const auto di = pink (32.0, 0.5f, 31);
    const auto mic = makeMic (di);
    h.p.toggleLearn();
    std::vector<float> l, r;
    h.run (di, &mic, l, r);
    h.p.service();
    save ("strum-learned.png");
    return 0;
}
#endif

int main (int argc, char** argv)
{
   #if ENCORE_STRUM_PLUGIN_TESTS
    juce::ScopedJuceInitialiser_GUI init;
    if (argc > 2 && juce::String (argv[1]) == "--snapshot")
        return snapshot (juce::File (juce::String (argv[2])));
   #else
    juce::ignoreUnused (argc, argv);
   #endif
    learnTests();
    squeakTests();
    boomTests();
    pickTests();
    doubleTests();
   #if ENCORE_STRUM_PLUGIN_TESTS
    pluginTests();
   #endif
    std::printf ("\n%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
