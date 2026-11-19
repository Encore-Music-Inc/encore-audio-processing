// A score's instruments, built without JUCE as the browser build is: the
// synthesizer's options for them (a burst at the start of a note, a cutoff
// following the note, a second decay, a vibrato that fades in, a velocity
// curve, a tone peak, waves starting together, expression per note), the
// envelope's second decay and slurred start, a triangle starting on its own
// line, and every family's sound. Each check prints PASS or FAIL; the suite
// exits non-zero on any failure.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "dsp/Instruments.h"

namespace
{
    int failures = 0;

    void expect (const std::string& name, bool ok, const char* fmt = "", double a = 0, double b = 0, double c = 0)
    {
        std::printf ("%s %-72s ", ok ? "PASS" : "FAIL", name.c_str());
        std::printf (fmt, a, b, c);
        std::printf ("\n");
        if (! ok) ++failures;
    }

    constexpr double fs = 48000.0;

    double rms (const std::vector<float>& x, double from, double to)
    {
        const size_t a = (size_t) (from * fs), b = std::min (x.size(), (size_t) (to * fs));
        double s = 0;
        for (size_t i = a; i < b; ++i) s += (double) x[i] * x[i];
        return std::sqrt (s / std::max<size_t> (1, b - a));
    }
    double mean (const std::vector<float>& x, double from, double to)
    {
        const size_t a = (size_t) (from * fs), b = std::min (x.size(), (size_t) (to * fs));
        double s = 0;
        for (size_t i = a; i < b; ++i) s += x[i];
        return s / std::max<size_t> (1, b - a);
    }

    // One note: held for `held` seconds, then `tail` more after its release; the left channel
    std::vector<float> note (const vs::Synth::Settings& set, int midi, float vel, double held, double tail, const vs::Synth::Expression& e = {})
    {
        vs::Synth s;
        s.prepare (fs);
        s.setSettings (set);
        const int on = (int) (held * fs), n = on + (int) (tail * fs);
        std::vector<float> l ((size_t) n, 0.0f), r ((size_t) n, 0.0f);
        const int v = s.noteOn (midi, vel, e);
        s.render (l.data(), r.data(), on);
        s.noteOffVoice (v, midi);
        s.render (l.data() + on, r.data() + on, n - on);
        return l;
    }

    // A plain sound: one oscillator, no filter, a flat envelope
    vs::Synth::Settings plain (vs::Oscillator::Wave w)
    {
        vs::Synth::Settings s;
        s.osc[0].wave = w;
        s.osc[0].levelDb = 0;
        s.filter.type = vs::Synth::FilterType::off;
        s.ampEnv = { 0.0001f, 0.1f, 1.0f, 0.01f };
        s.levelDb = 0;
        return s;
    }

    void triangle()
    {
        // a triangle starting a quarter into its cycle: no offset from the first cycle on
        for (float phase : { 0.0f, 0.25f, 0.6f })
        {
            auto s = plain (vs::Oscillator::Wave::triangle);
            s.osc[0].startPhase = phase;
            const auto x = note (s, 69, 1.0f, 0.2, 0.0);   // 440 Hz: 0.1 s is 44 cycles
            expect ("a triangle has no offset, wherever in its cycle it starts", std::abs (mean (x, 0.0, 0.1)) < 0.003, "start %.2f: mean %.4f", phase, mean (x, 0.0, 0.1));
            // the oscillator on its own: its first cycle centred too, and even peaks
            vs::Oscillator o;
            o.setWave (vs::Oscillator::Wave::triangle);
            o.setPhase (phase);
            double first = 0, lo = 0, hi = 0;
            for (int i = 0; i < 4800; ++i)
            {
                const double v = o.next (440.0, fs);
                if (i < 109) first += v / 109.0;   // 109 samples: one cycle at 440 Hz
                if (i > 2400) { lo = std::min (lo, v); hi = std::max (hi, v); }
            }
            expect ("its first cycle is centred and its peaks even", std::abs (first) < 0.005 && std::abs (hi + lo) < 0.005, "%.4f, %.4f", first, hi + lo);
        }
        vs::Oscillator o;
        o.setWave (vs::Oscillator::Wave::triangle);
        o.setPhase (0.25);
        expect ("a triangle a quarter in starts at 0, rising", std::abs (o.next (100, fs)) < 0.01 && o.next (100, fs) > 0.0f);
    }

    void envelope()
    {
        vs::Envelope e;
        e.prepare (fs);
        e.setSettings ({ 0.0001f, 0.2f, 0.5f, 0.1f, 2.0f, 0.0f });
        e.noteOn();
        std::vector<float> v ((size_t) (3 * fs));
        for (auto& x : v) x = e.next();
        // the first decay settles toward 0.5; after three of its time constants the second takes it to silence
        const double tau = 0.2 / 9.210340371976184;
        expect ("the first decay heads for its sustain", std::abs (v[(size_t) (2 * tau * fs)] - (0.5 + 0.5 * std::exp (-2.0))) < 0.01, "%.4f", v[(size_t) (2 * tau * fs)]);
        // the second starts after 3 x 0.2 / 9.21 s, from just above 0.5, falling with its own time constant (2 / 9.21 s)
        const double start = 3 * tau, at = start + 0.25, want = (0.5 + 0.5 * std::exp (-3.0)) * std::exp (-(at - start) / (2.0 / 9.210340371976184));
        expect ("then the second, slower decay takes over", std::abs (v[(size_t) (at * fs)] - want) < 0.01, "%.4f (%.4f)", v[(size_t) (at * fs)], want);
        expect ("and ends the note at a sustain of 0", ! e.isActive() && v.back() == 0.0f);

        vs::Envelope s;
        s.prepare (fs);
        s.setSettings ({ 0.001f, 0.1f, 1.0f, 0.1f });
        s.noteOnFrom (0.35, 0.03);
        const float first = s.next();
        for (int i = 0; i < (int) (0.015 * fs); ++i) s.next();
        expect ("a slurred start begins part-way and takes its own attack", std::abs (first - 0.35f) < 0.001f && std::abs (s.current() - 0.675f) < 0.01f, "%.3f", s.current());
    }

    void options()
    {
        // a burst: an oscillator that fades out over its time, not on a slurred start
        {
            auto s = plain (vs::Oscillator::Wave::sine);
            s.osc[0].on = false;
            s.osc[1] = s.osc[0];
            s.osc[1].on = true;
            s.osc[1].wave = vs::Oscillator::Wave::noise;
            s.osc[1].burstSeconds = 0.08f;
            const auto x = note (s, 60, 1.0f, 0.3, 0.0);
            vs::Synth::Expression slur;
            slur.slurred = true;
            const auto y = note (s, 60, 1.0f, 0.3, 0.0, slur);
            expect ("a burst sounds at the start and is gone after its time", rms (x, 0.0, 0.02) > 0.3 && rms (x, 0.09, 0.3) == 0.0 && rms (y, 0.0, 0.3) == 0.0,
                    "%.3f", rms (x, 0.0, 0.02));
        }
        // the cutoff following the note: an octave up, the cutoff (above its base) twice as high
        {
            auto s = plain (vs::Oscillator::Wave::sawUp);
            s.filter.type = vs::Synth::FilterType::lowPass;
            s.filter.hz = 20.0f;
            s.filter.noteFollow = 1.0f;
            const double lo = rms (note (s, 48, 1.0f, 0.3, 0.0), 0.1, 0.3), hi = rms (note (s, 60, 1.0f, 0.3, 0.0), 0.1, 0.3);
            // (within its 20 Hz base, which counts for more on the lower note)
            expect ("a cutoff that follows the note keeps a saw's tone the same at every pitch", std::abs (lo / hi - 1.0) < 0.08, "%.3f", lo / hi);
            s.filter.maxHz = 300.0f;
            const double capped = rms (note (s, 84, 1.0f, 0.3, 0.0), 0.1, 0.3), open = rms (note (plain (vs::Oscillator::Wave::sawUp), 84, 1.0f, 0.3, 0.0), 0.1, 0.3);
            expect ("and stops at its top", capped < 0.5 * open, "%.3f", capped / open);
        }
        // a vibrato that fades in: nothing at the start, all of it later
        {
            auto s = plain (vs::Oscillator::Wave::sine);
            s.lfo[0] = { vs::Oscillator::Wave::sine, 5.0f, 1.0f, vs::Synth::LfoTarget::pitch, 0.5f };
            vs::Synth syn;
            syn.prepare (fs);
            syn.setSettings (s);
            syn.noteOn (69, 1.0f, vs::Synth::Expression {});
            std::vector<float> l ((size_t) fs), r ((size_t) fs);
            syn.render (l.data(), r.data(), (int) fs);
            auto crossings = [&] (double a, double b)
            {
                int c = 0;
                for (size_t i = (size_t) (a * fs) + 1; i < (size_t) (b * fs); ++i) if (l[i - 1] < 0 && l[i] >= 0) ++c;
                return c / (b - a);
            };
            expect ("a vibrato that fades in starts on pitch", std::abs (crossings (0.0, 0.05) - 440.0) < 25.0, "%.0f Hz", crossings (0.0, 0.05));
        }
        // the velocity curve
        {
            auto s = plain (vs::Oscillator::Wave::sine);
            s.velocityCurve = 1.5f;
            const double half = rms (note (s, 69, 0.5f, 0.2, 0.0), 0.05, 0.2), full = rms (note (s, 69, 1.0f, 0.2, 0.0), 0.05, 0.2);
            expect ("the level follows velocity to its curve", std::abs (half / full - std::pow (0.5, 1.5)) < 0.01, "%.4f", half / full);
        }
        // a tone peak: +6 dB at its frequency
        {
            auto s = plain (vs::Oscillator::Wave::sine);
            const double flat = rms (note (s, 79, 1.0f, 0.3, 0.0), 0.1, 0.3);   // 784 Hz
            s.peak = { true, 784.0f, 1.4f, 6.0f };
            const double peaked = rms (note (s, 79, 1.0f, 0.3, 0.0), 0.1, 0.3);
            expect ("a tone peak lifts its frequency", std::abs (20 * std::log10 (peaked / flat) - 6.0) < 0.2, "%+.2f dB", 20 * std::log10 (peaked / flat));
        }
        // waves starting together add up; started apart, a sine and a triangle partly cancel
        {
            auto s = plain (vs::Oscillator::Wave::sine);
            s.osc[1] = s.osc[0];
            s.osc[1].on = true;
            s.osc[1].wave = vs::Oscillator::Wave::triangle;
            s.osc[0].startPhase = 0.0f;
            s.osc[1].startPhase = 0.25f;
            const double together = rms (note (s, 57, 1.0f, 0.3, 0.0), 0.05, 0.3);
            s.osc[1].startPhase = 0.0f;   // a quarter cycle out of step
            const double apart = rms (note (s, 57, 1.0f, 0.3, 0.0), 0.05, 0.3);
            // in step, the two add to an RMS of 1.28 (0.905 a side through the centre pan); a quarter apart, 0.91 (0.64)
            expect ("a sine and a triangle starting together add up", together > 0.89 && apart < 0.66, "%.3f / %.3f", together, apart);
        }
    }

    void expression()
    {
        auto s = plain (vs::Oscillator::Wave::sine);
        const double base = rms (note (s, 69, 1.0f, 0.5, 0.0), 0.1, 0.5);
        vs::Synth::Expression e;
        e.gain = 0.5f;
        expect ("a note's gain", std::abs (rms (note (s, 69, 1.0f, 0.5, 0.0, e), 0.1, 0.5) / base - 0.5) < 0.01);
        vs::Synth::Expression sw;
        sw.swellTo = 2.0f;
        sw.swellUntil = 1.0f;
        const auto x = note (s, 69, 1.0f, 1.0, 0.0, sw);
        expect ("a swell grows in a straight line to its level", std::abs (rms (x, 0.45, 0.55) / base - 1.5) < 0.05 && std::abs (rms (x, 0.95, 1.0) / base - 1.97) < 0.05,
                "%.3f %.3f", rms (x, 0.45, 0.55) / base, rms (x, 0.95, 1.0) / base);
        vs::Synth::Expression b;
        b.bendSemitones = 12.0f;
        b.bendFrom = 0.2f;
        b.bendTo = 0.4f;
        const auto y = note (s, 57, 1.0f, 0.6, 0.0, b);
        auto hz = [&] (double a, double c)
        {
            int n = 0;
            for (size_t i = (size_t) (a * fs) + 1; i < (size_t) (c * fs); ++i) if (y[i - 1] < 0 && y[i] >= 0) ++n;
            return n / (c - a);
        };
        expect ("a glide: on its note, then an octave up after it", std::abs (hz (0.0, 0.2) - 220.0) < 10.0 && std::abs (hz (0.45, 0.6) - 440.0) < 15.0, "%.0f -> %.0f Hz", hz (0.0, 0.2), hz (0.45, 0.6));

        // a voice ends its own note, not another at the same pitch
        vs::Synth syn;
        syn.prepare (fs);
        syn.setSettings (s);
        const int a = syn.noteOn (69, 1.0f, vs::Synth::Expression {});
        syn.noteOn (69, 1.0f, vs::Synth::Expression {});
        syn.noteOffVoice (a, 69);
        std::vector<float> l (4800), r (4800);
        syn.render (l.data(), r.data(), 4800);
        expect ("ending one voice leaves the other at the same pitch sounding", rms (l, 0.05, 0.1) > 0.4 && syn.freeVoices() == syn.getSettings().voices - 1,
                "%.3f", rms (l, 0.05, 0.1));
    }

    void families()
    {
        bool all = true, levels = true;
        for (auto* f : vs::instruments::families)
        {
            const auto s = vs::instruments::synthSound (f);
            if (! s) { all = false; continue; }
            const auto x = note (*s, 60, 90.0f / 127.0f, 1.0, 1.0);
            const double db = 20 * std::log10 (rms (x, 0.0, 1.0) * std::sqrt (2.0));
            levels = levels && db > -32.0 && db < -16.0;
            all = all && std::all_of (x.begin(), x.end(), [] (float v) { return std::isfinite (v); });
        }
        expect ("every family has a sound, and plays at a score's level", all && levels);
        expect ("an unknown family has none", ! vs::instruments::synthSound ("kazoo"));
        // the piano: a quick drop, then a long fade, silent before 2 s after a 1 s note
        const auto p = note (*vs::instruments::synthSound ("piano"), 60, 0.7f, 1.0, 1.0);
        expect ("the piano drops, then fades", rms (p, 0.0, 0.05) > 2.0 * rms (p, 0.9, 1.0) && rms (p, 1.9, 2.0) < 1e-4);
        // winds breathe at the start of a note
        const auto w = vs::instruments::synthSound ("winds");
        expect ("winds breathe as a note starts", w->osc[3].on && w->osc[3].wave == vs::Oscillator::Wave::noise && w->osc[3].burstSeconds > 0.0f);
        expect ("strings, winds and voices swell within a note; the piano doesn't",
                vs::instruments::swellsWithin ("strings") && vs::instruments::swellsWithin ("voice") && ! vs::instruments::swellsWithin ("piano"));
    }

    // ---- recorded instruments: the sampler's zones

    std::shared_ptr<const std::vector<float>> tone (int n, double hz, double rate)
    {
        auto v = std::make_shared<std::vector<float>> ((size_t) n);
        for (int i = 0; i < n; ++i) (*v)[(size_t) i] = (float) std::sin (6.283185307179586 * hz * i / rate);
        return v;
    }

    // Plays one note on a sampler for `held` seconds then `tail` more; returns left and right
    std::pair<std::vector<float>, std::vector<float>> play (vs::Sampler& s, int note, float vel, double held, double tail, const vs::NoteExpression& e = {})
    {
        s.allNotesOff (true);
        const int on = (int) (held * fs), n = on + (int) (tail * fs);
        std::vector<float> l ((size_t) n, 0.0f), r ((size_t) n, 0.0f);
        const auto id = s.noteOn (note, vel, e);
        s.render (l.data(), r.data(), on);
        s.noteOffId (id);
        s.render (l.data() + on, r.data() + on, n - on);
        return { l, r };
    }

    const char* manifest = R"({"v":1,"gain":1,"zones":[
        {"lo":0,"hi":64,"vlo":1,"vhi":63,"s":0,"root":60,"tune":0,"scale":100,"gain":0.5,"pan":0,"fc":0,"q":0,"mode":1,"excl":0,"ls":0,"le":0,
         "env":{"delay":0,"attack":0.01,"hold":0,"decay":0,"sustain":1,"release":0.2}},
        {"lo":0,"hi":64,"vlo":64,"vhi":127,"s":1,"root":60,"tune":0,"scale":100,"gain":1,"pan":0,"fc":0,"q":0,"mode":1,"excl":0,"ls":0,"le":0,
         "env":{"delay":0,"attack":0.01,"hold":0,"decay":0,"sustain":1,"release":0.2}},
        {"lo":70,"hi":80,"vlo":1,"vhi":127,"s":2,"root":72,"tune":0,"scale":100,"gain":1,"pan":-1,"fc":0,"q":0,"mode":0,"excl":7,"ls":0,"le":0,
         "env":{"delay":0,"attack":0,"hold":0.05,"decay":0.5,"sustain":0,"release":0.1}}],
      "samples":[{"c":0,"o":0,"n":1,"rate":48000,"ls":1000,"le":5800},{"c":0,"o":0,"n":1,"rate":48000,"ls":1000,"le":5800},{"c":0,"o":0,"n":1,"rate":48000}],
      "chunks":[{"file":"x","bytes":1}]})";

    void recorded()
    {
        // two velocity layers of a 480 Hz tone (looped, 10 cycles a loop) and a hard-left 576 Hz zone
        const auto quiet = tone (6000, 480.0, fs), loud = tone (6000, 480.0, fs), high = tone (24000, 576.0, fs);
        vs::Sampler s;
        s.prepare (fs);
        const int zones = vs::instruments::loadRecorded (s, manifest, [&] (int i) { return i == 0 ? quiet : i == 1 ? loud : high; });
        expect ("a manifest's zones load", zones == 3);
        expect ("a manifest it can't read is refused", vs::instruments::loadRecorded (s, R"({"v":2,"zones":[]})", nullptr) == -1);
        vs::instruments::loadRecorded (s, manifest, [&] (int i) { return i == 0 ? quiet : i == 1 ? loud : high; });

        const auto [ql, qr] = play (s, 60, 50.0f / 127, 0.5, 0.0);
        const auto [ll, lr] = play (s, 60, 100.0f / 127, 0.5, 0.0);
        const double q = rms (ql, 0.1, 0.5), l = rms (ll, 0.1, 0.5);
        const double want = 0.5 * std::pow (50.0 / 100.0, 1.7);   // the quiet layer's gain, and the velocity curve
        expect ("a velocity picks its layer", std::abs (q / l - want) < 0.01, "%.4f (%.4f)", q / l, want);
        // the recording is 0.125 s; its loop (1000 to 5800) keeps the note at its level
        expect ("a looped zone keeps sounding past its recording's length", std::abs (rms (ll, 0.3, 0.5) / rms (ll, 0.05, 0.1) - 1.0) < 0.02, "%.3f", rms (ll, 0.3, 0.5) / rms (ll, 0.05, 0.1));
        expect ("a zone at the centre plays the same on both sides", ql == qr);

        const auto [hl, hr] = play (s, 72, 0.8f, 1.0, 0.0);
        expect ("a zone panned hard left is silent on the right", rms (hl, 0.0, 0.2) > 0.1 && rms (hr, 0.0, 1.0) == 0.0);
        // held 0.05 s, then falling with a time constant of 0.5 / 11.5 s
        const double heldRms = rms (hl, 0.0, 0.04), later = rms (hl, 0.1, 0.11), expected = heldRms * std::exp (-(0.105 - 0.05) / (0.5 / 11.512925464970229));
        expect ("its hold, then its decay", std::abs (later / expected - 1.0) < 0.05, "%.4f (%.4f)", later, expected);
        expect ("a key out of every zone plays the nearest", s.ready (90, 0.5f) && s.ready (66, 0.5f) && s.ready (0, 0.5f));

        // an exclusive group: a second note cuts the first (both in group 7)
        {
            s.allNotesOff (true);
            std::vector<float> l1 ((size_t) fs), r1 ((size_t) fs), l2 ((size_t) fs), r2 ((size_t) fs);
            s.noteOn (72, 0.8f, vs::NoteExpression {});
            s.render (l1.data(), r1.data(), 4800);
            s.render (l1.data() + 4800, r1.data() + 4800, 43200);
            s.allNotesOff (true);
            s.noteOn (72, 0.8f, vs::NoteExpression {});
            s.render (l2.data(), r2.data(), 4800);
            s.noteOn (74, 0.8f, vs::NoteExpression {});
            std::vector<float> only ((size_t) fs), rOnly ((size_t) fs);
            s.render (l2.data() + 4800, r2.data() + 4800, 43200);
            // after the cut, only the second note is left: its own 576 * 2^(2/12) Hz, nothing at 576 Hz
            vs::Sampler one;
            one.prepare (fs);
            vs::instruments::loadRecorded (one, manifest, [&] (int i) { return i == 0 ? quiet : i == 1 ? loud : high; });
            one.noteOn (74, 0.8f, vs::NoteExpression {});
            one.render (only.data() + 4800, rOnly.data() + 4800, 43200);
            double d = 0;
            for (int k = (int) (0.25 * fs); k < (int) (0.3 * fs); ++k) d = std::max (d, (double) std::abs (l2[(size_t) k] - only[(size_t) k]));
            expect ("a note in an exclusive group cuts the group's earlier note", d < 1e-5 && rms (l1, 0.25, 0.3) > 1e-3, "%.2e", d);
        }

        // not every recording here yet: the note isn't ready, and plays nothing
        vs::Sampler partial;
        partial.prepare (fs);
        vs::instruments::loadRecorded (partial, manifest, [&] (int i) { return i == 1 ? loud : nullptr; });
        expect ("a note whose recording hasn't come isn't ready", ! partial.ready (60, 50.0f / 127) && partial.ready (60, 100.0f / 127)
                && partial.noteOn (60, 50.0f / 127, vs::NoteExpression {}) == 0);

        // a slurred start, the low-pass and the release
        vs::NoteExpression slur;
        slur.slurred = true;
        slur.slurLevel = 0.3f;
        const auto [sl, sr] = play (s, 60, 100.0f / 127, 0.5, 0.0, slur);
        const double slurStart = rms (sl, 0.0, 0.002) / rms (sl, 0.1, 0.2);
        expect ("a slurred note starts part-way up", slurStart > 0.25 && slurStart < 0.4, "%.3f", slurStart);
        vs::NoteExpression dark;
        dark.brightness = 0.005f;   // the open filter (20 kHz) shaded down to 100 Hz at full velocity
        const auto [dl, dr] = play (s, 60, 1.0f, 0.5, 0.0, dark);
        const auto [fl, fr] = play (s, 60, 1.0f, 0.5, 0.0);
        expect ("a darker note goes through the low-pass", rms (dl, 0.1, 0.5) < 0.1 * rms (fl, 0.1, 0.5), "%.3f", rms (dl, 0.1, 0.5) / rms (fl, 0.1, 0.5));
        const auto [rl, rr] = play (s, 60, 100.0f / 127, 0.3, 0.5);
        expect ("a released note fades over its release, then stops", rms (rl, 0.32, 0.34) > rms (rl, 0.45, 0.47) && rms (rl, 0.7, 0.8) == 0.0);
    }
}

int main()
{
    triangle();
    envelope();
    options();
    expression();
    families();
    recorded();
    std::printf ("%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
