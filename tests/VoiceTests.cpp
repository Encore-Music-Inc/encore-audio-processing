// The pitch detector's three settings (dsp/Yin.h), the voice tracker and what
// a pitch track measures (dsp/VoiceTracker.h), built without JUCE as the
// browser build is. Each check prints PASS or FAIL; the suite exits non-zero
// on any failure.
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>
#include "dsp/PitchDetector.h"
#include "dsp/TunerPitch.h"
#include "dsp/VoiceTracker.h"
#include "dsp/Yin.h"

namespace
{
    int failures = 0;
    constexpr double twoPi = 6.283185307179586;

    void expect (const char* name, bool ok, const char* fmt = "", double a = 0, double b = 0, double c = 0)
    {
        std::printf ("%s %-62s ", ok ? "PASS" : "FAIL", name);
        std::printf (fmt, a, b, c);
        std::printf ("\n");
        if (! ok) ++failures;
    }

    std::vector<float> wave (int n, double fs, const std::function<double (double)>& f)
    {
        std::vector<float> x ((size_t) n);
        for (int i = 0; i < n; ++i) x[(size_t) i] = (float) f (i / fs);
        return x;
    }

    double cents (double got, double want) { return got > 0 ? 1200.0 * std::log2 (got / want) : 9999.0; }

    using vs::voice::PitchFrame;
    constexpr double none = std::numeric_limits<double>::quiet_NaN();

    // a pitch track at 100 frames a second from t0 to t1, midi (t)
    std::vector<PitchFrame> track (double t0, double t1, const std::function<double (double)>& midi)
    {
        std::vector<PitchFrame> f;
        for (double t = t0; t <= t1 + 1e-9; t += 0.01) f.push_back ({ t, midi (t) });
        return f;
    }

    void append (std::vector<PitchFrame>& a, const std::vector<PitchFrame>& b) { a.insert (a.end(), b.begin(), b.end()); }
}

int main()
{
    // ---- the detector
    {
        const double fs = 24000.0;
        double worst = 0;
        for (double hz : { 82.4, 110.0, 196.0, 261.6, 440.0, 659.3, 987.8 })
        {
            const auto x = wave (1080, fs, [&] (double t) { return 0.4 * std::sin (twoPi * hz * t) + 0.25 * std::sin (2 * twoPi * hz * t) + 0.1 * std::sin (3 * twoPi * hz * t); });
            vs::Yin yin (vs::Yin::voice());
            worst = std::max (worst, std::abs (cents (yin.analyse (x.data(), (int) x.size(), fs).hz, hz)));
        }
        expect ("voice: sung notes from E2 to B5 within 2 cents (45 ms)", worst < 2, "worst %.2f cents", worst);

        // "ee" on a low note: a weak fundamental under a strong 2nd harmonic
        const auto ee = wave (1080, fs, [] (double t) { return 0.1 * std::sin (twoPi * 98 * t) + 0.6 * std::sin (twoPi * 196 * t) + 0.2 * std::sin (twoPi * 294 * t); });
        vs::Yin yin (vs::Yin::voice());
        const auto& r = yin.analyse (ee.data(), (int) ee.size(), fs);
        expect ("voice: a weak fundamental is still the note (no octave-up slip)", std::abs (cents (r.hz, 98)) < 5, "%.1f Hz", r.hz);
        expect ("voice: every dip is kept as a candidate period", r.candidates.size() >= 2, "%.0f candidates", (double) r.candidates.size());

        const std::vector<float> quiet (1080, 0.0005f);
        const auto& s = yin.analyse (quiet.data(), (int) quiet.size(), fs);
        expect ("voice: quieter than -60 dB is no note, with its level", s.hz == 0 && ! s.voiced && std::abs (s.rms - 0.0005) < 1e-6, "rms %.6f", s.rms);
        expect ("voice: a window too short for the lowest note is no note", yin.analyse (quiet.data(), 2, fs).hz == 0);
    }
    {
        // the pitch engine's detector is Yin::follow, the tuner's Yin::tuner: one detector
        const double fs = 48000.0;
        const auto x = wave (2048, fs, [] (double t) { return 0.5 * std::sin (twoPi * 233.1 * t) + 0.2 * std::sin (2 * twoPi * 233.1 * t); });
        vs::PitchDetector pd;
        pd.prepare (fs);
        const auto a = pd.analyse (x.data());
        vs::Yin follow (vs::Yin::follow());
        const auto& b = follow.analyse (x.data(), 2048, fs);
        expect ("follow: the pitch engine's detector hears 233.1 Hz", a.voiced && std::abs (cents (a.f0, 233.1)) < 2 && a.confidence > 0.9, "%.2f Hz", a.f0);
        expect ("follow: it is Yin::follow, number for number", a.f0 == (float) fs / (float) b.period && a.confidence == 1.0f - b.dip);
        expect ("follow: 80 Hz to 1200 Hz at 48 kHz", pd.minPeriod() == 40 && pd.maxPeriod() == 600, "lags %.0f to %.0f", pd.minPeriod(), pd.maxPeriod());
        vs::Yin tuner (vs::Yin::tuner());
        expect ("tuner: TunerPitch is Yin::tuner", vs::TunerPitch::estimate (x.data(), 2048, fs) == (float) tuner.analyse (x.data(), 2048, fs).hz);
    }

    // ---- the tracker
    {
        const double fs = 48000.0;
        vs::VoiceTracker tr;
        const auto x = wave (48000, fs, [] (double t) { return t < 0.5 ? 0.3 * std::sin (twoPi * 220 * t) + 0.1 * std::sin (twoPi * 440 * t) : 0.0; });
        const int win = 2880;
        double worst = 0;
        bool voicedAll = true, silentAfter = true;
        for (int end = win; end <= (int) x.size(); end += 480)
        {
            const double t = end / fs;
            const auto f = tr.feed (x.data() + end - win, win, fs, t);
            if (t > 0.1 && t < 0.49) { voicedAll = voicedAll && f.voiced(); if (f.voiced()) worst = std::max (worst, std::abs (f.midi - 57)); }
            if (t > 0.6) silentAfter = silentAfter && ! f.voiced();
        }
        expect ("tracker: a sung A3 reads MIDI 57 throughout", voicedAll && worst < 0.05, "worst %.3f semitones", worst);
        expect ("tracker: silence after it reads no voice", silentAfter);

        // an octave slip for a moment is not shown; a real leap is believed once it lasts 0.1 s
        vs::VoiceTracker t2;
        auto det = [] (double hz, double other = 0) {
            vs::Yin::Result r;
            r.voiced = true; r.hz = hz; r.clarity = 0.95; r.rms = 0.1;
            r.candidates.push_back ({ hz, 0.05f });
            if (other > 0) r.candidates.push_back ({ other, 0.2f });
            return r;
        };
        double t = 0;
        for (int i = 0; i < 10; ++i, t += 0.01) t2.push (det (220), t);
        const auto slip = t2.push (det (440, 220), t); t += 0.01;
        expect ("tracker: an octave slip with the old note still a candidate keeps the note", std::abs (slip.midi - 57) < 0.01, "%.2f", slip.midi);
        const auto unsure = t2.push (det (440), t); t += 0.01;
        expect ("tracker: a sudden leap shows nothing until it lasts", ! unsure.voiced());
        vs::VoiceTracker::Frame last;
        for (int i = 0; i < 15; ++i, t += 0.01) last = t2.push (det (440), t);
        expect ("tracker: ... and is believed after 0.1 s", std::abs (last.midi - 69) < 0.01, "%.2f", last.midi);
    }

    // ---- what a pitch track shows
    {
        const auto siren = track (0, 4, [] (double t) { return 48 + 24 * (t <= 2 ? t / 2 : (4 - t) / 2); });
        const auto range = vs::voice::analyzeRange (siren);
        expect ("range: a siren from C3 to C5 is the range", range && range->low == 48 && range->high == 72 && ! range->tessMeasured,
                "%.0f to %.0f", range ? range->low : 0, range ? range->high : 0);
        expect ("range: the comfortable middle by rule of thumb", range && range->tessLo == 51 && range->tessHi == 68);
        auto song = track (0, 2, [] (double) { return 60.0; });
        append (song, track (2.5, 4.5, [] (double) { return 64.0; }));
        append (song, track (5, 7, [] (double) { return 62.0; }));
        const auto tess = vs::voice::tessitura (song);
        expect ("tessitura: from steady singing", tess && tess->lo == 60 && tess->hi == 64 && tess->seconds > 3, "%.0f to %.0f", tess ? tess->lo : 0, tess ? tess->hi : 0);
        const auto withSong = vs::voice::analyzeRange (siren, &song);
        expect ("range: the comfortable middle from a song sung comfortably", withSong && withSong->tessMeasured && withSong->tessLo == 60 && withSong->tessHi == 64);

        const auto held = track (0, 4, [] (double t) { return 69 + 0.5 * std::sin (twoPi * 5.5 * t) + 0.1 * t; });
        const auto s = vs::voice::analyzeSustain (held, 69.0);
        expect ("sustain: a 5.5 Hz vibrato is found", s && s->vibrato && std::abs (s->vibrato->rate - 5.5) < 0.2 && s->vibrato->regularity > 0.8,
                "%.2f Hz", s && s->vibrato ? s->vibrato->rate : 0);
        expect ("sustain: its width (100 cents peak to peak, as sung)", s && s->vibrato && s->vibrato->width > 95 && s->vibrato->width < 130,
                "%.0f cents", s && s->vibrato ? s->vibrato->width : 0);
        expect ("sustain: the drift, and the cents from the target", s && std::abs (s->drift - 35) < 6 && s->cents && std::abs (*s->cents - 20) < 3,
                "drift %.1f, %.1f cents", s ? s->drift : 0, s && s->cents ? *s->cents : 0);
        const auto straight = vs::voice::analyzeSustain (track (0, 3, [] (double) { return 60.0; }));
        expect ("sustain: a straight tone has no vibrato and no wobble", straight && ! straight->vibrato && straight->wobble < 0.01);

        auto three = track (0, 0.5, [] (double) { return 60.0; });
        append (three, track (0.51, 0.6, [] (double t) { return 60 + (t - 0.5) * 40; })); // a slide between
        append (three, track (0.61, 1.1, [] (double) { return 64.0; }));
        append (three, track (1.3, 1.8, [] (double) { return 67.2; }));
        const auto notes = vs::voice::segmentNotes (three);
        expect ("notes: three notes sung, the slide between left out", notes.size() == 3 && notes[0].midi == 60 && notes[1].midi == 64 && std::abs (notes[2].midi - 67.2) < 1e-9,
                "%.0f notes", (double) notes.size());
        const auto back = vs::voice::scoreSingBack (three, { 60, 64, 67 });
        expect ("sing back: the notes and jumps right score high", back.score >= 95 && back.notes[2].cents && std::abs (*back.notes[2].cents - 20) < 1e-6, "score %.0f", back.score);
        const auto backUp = vs::voice::scoreSingBack (three, { 48, 52, 55 });
        expect ("sing back: an octave up still counts", backUp.octaves == 1 && backUp.score == back.score);

        auto match = track (0, 0.15, [] (double t) { return 57 + 2 * (1 - t / 0.15); });
        append (match, track (0.16, 1.5, [] (double) { return 57.1; }));
        const auto m = vs::voice::scoreMatch (match, 57);
        expect ("match: 10 cents sharp once settled, settling fast, scores high", m.heard && std::abs (m.cents - 10) < 1e-6 && m.settle && *m.settle < 0.3 && m.score >= 95,
                "score %.0f", m.score);
        const auto low = vs::voice::scoreMatch (track (0, 1, [] (double) { return 45.0; }), 57);
        expect ("match: an octave lower counts as the note", low.heard && low.octaves == -1 && std::abs (low.cents) < 1e-9);
        expect ("match: nothing sung is not heard", ! vs::voice::scoreMatch ({}, 57).heard);

        std::vector<vs::voice::PatternNote> pattern { { 60, 0, 0.5 }, { 62, 0.5, 0.5 }, { 64, 1.0, 0.5 } };
        const auto late = track (0, 1.8, [] (double t) { return t < 0.2 ? none : t < 0.7 ? 60 : t < 1.2 ? 62 : t < 1.7 ? 64 : none; });
        const auto pat = vs::voice::scorePattern (late, pattern);
        expect ("pattern: singing 0.2 s late is forgiven", std::abs (pat.lag - 0.2) < 0.03 && pat.notes[2].cents && std::abs (*pat.notes[2].cents) < 1e-9, "lag %.3f s", pat.lag);
        const auto sum = vs::voice::notesSummary (pat.notes);
        expect ("pattern: all three notes heard and in tune", sum.heard == 3 && sum.good == 3 && sum.all == 0);

        const auto cover = vs::voice::scoreSiren (siren, { 48, 72, 2, 2 });
        expect ("siren: a smooth slide covers it all with no jump", cover.coverage == 1 && cover.lowest == 48 && cover.highest == 72 && ! cover.jumpAt);
        auto crack = track (0, 1, [] (double t) { return t < 0.5 ? 55 + 4 * t : 65 + 4 * t; });
        const auto cracked = vs::voice::scoreSiren (crack, { 55, 70, 1, 1 });
        expect ("siren: a flip is found, and where", cracked.jumpAt && std::abs (cracked.jump - 10) < 0.1 && *cracked.jumpAt == 62, "jump %.1f at %.0f", cracked.jump, cracked.jumpAt ? *cracked.jumpAt : 0);
    }

    // ---- notes played, and their timing
    {
        vs::NoteFinder finder;
        std::vector<vs::NoteFinder::Note> found;
        auto feed = [&] (double t, double midi, double level) {
            const vs::NoteFinder::Reading r { t, midi, level };
            if (auto n = finder.feed (&r)) found.push_back (*n);
        };
        double t = 0;
        for (int i = 0; i < 5; ++i, t += 0.01) feed (t, 60, 0.1);
        for (int i = 0; i < 2; ++i, t += 0.01) feed (t, 62, 0.1); // too short to count
        for (int i = 0; i < 4; ++i, t += 0.01) feed (t, 60, 0.1);
        feed (t, 60, 0.5); t += 0.01; // plucked again
        for (int i = 0; i < 3; ++i, t += 0.01) feed (t, 60, 0.4);
        expect ("note finder: a held note once, a re-pluck again", found.size() == 2 && found[0].midi == 60 && found[0].t == 0 && std::abs (found[1].t - 0.11) < 1e-9,
                "%.0f notes", (double) found.size());
        const auto rep = vs::timingReport ({ 0.01, 0.52, none, 1.4 }, { 0, 0.5, 1.0, 1.5 });
        expect ("timing: offsets, mean, spread and on time", rep && rep->offsets == std::vector<double> { 10, 20, -100 } && rep->mean == -23 && rep->onTime == 67,
                "mean %.0f ms, %.0f%% on time", rep ? rep->mean : 0, rep ? rep->onTime : 0);
        expect ("timing: nothing played, no report", ! vs::timingReport ({ none }, { 0.0 }));
    }

    std::printf ("%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
