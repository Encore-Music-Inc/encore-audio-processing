// A pad player, built without JUCE as the browser build is: the sampler's pad
// options (the region played, the end fade, a tuning in semitones, a pad
// cutting itself off, the velocity curve), a loop rendered through it with its
// tails folded back into the start, the safety curve on the bus, where a sound
// first gets loud, and a loop recorder's timing on the beat grid. Each check
// prints PASS or FAIL; the suite exits non-zero on any failure.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "dsp/Channels.h"
#include "dsp/NoteEdit.h"
#include "dsp/Sampler.h"
#include "dsp/Saturator.h"

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

    // A ramp 0, 1, 2 ... (in 1/1000 steps) so a sample's value says where in the recording it came from
    vs::Sampler::Sound ramp (int n, int note)
    {
        vs::Sampler::Sound s;
        for (int i = 0; i < n; ++i) s.left.push_back ((float) i / 1000.0f);
        s.sampleRate = fs;
        s.rootNote = s.lowNote = s.highNote = note;
        return s;
    }

    std::vector<float> play (vs::Sampler& s, int note, float velocity, int n)
    {
        std::vector<float> l ((size_t) n, 0.0f), r ((size_t) n, 0.0f);
        s.noteOn (note, velocity);
        s.render (l.data(), r.data(), n);
        return l;
    }

    void pads()
    {
        // the region: from 0.01 s to 0.02 s of a 1000-sample ramp (samples 480 to 960)
        {
            vs::Sampler s;
            s.prepare (fs);
            auto snd = ramp (1000, 36);
            snd.startSeconds = 0.01;
            snd.endSeconds = 0.02;
            s.addSound (snd);
            const auto x = play (s, 36, 1.0f, 1000);
            const float g = x[0] / 0.480f;   // the pan law's gain at the centre, as a ratio
            int last = 0;
            for (int i = 0; i < 1000; ++i) if (x[(size_t) i] != 0.0f) last = i;
            expect ("a pad plays its region: it starts at its start", std::abs (x[0] - 0.480f * g) < 1e-6 && std::abs (x[10] / g - 0.490f) < 1e-5, "%.4f", x[10] / g);
            expect ("and stops at its end", last == 479, "last sample %.0f", (double) last);
        }
        // the end fade: the last 0.005 s of what's heard fall in a straight line to silence
        {
            vs::Sampler s;
            s.prepare (fs);
            vs::Sampler::Sound snd;
            snd.left.assign (2400, 1.0f);
            snd.sampleRate = fs;
            snd.rootNote = snd.lowNote = snd.highNote = 36;
            snd.fadeSeconds = 0.005;
            s.addSound (snd);
            const auto x = play (s, 36, 1.0f, 2400);
            const float full = x[100];
            expect ("an end fade: full level before it", std::abs (x[2400 - 241] - full) < 1e-6);
            expect ("halfway through it, half the level", std::abs (x[2400 - 120] / full - 0.5f) < 0.01f, "%.4f", x[2400 - 120] / full);
            expect ("silent at the end", std::abs (x[2399] / full) < 0.01f, "%.4f", x[2399] / full);
        }
        // a tuning of 12 semitones plays the recording twice as fast
        {
            vs::Sampler s;
            s.prepare (fs);
            auto snd = ramp (1000, 36);
            snd.pitchSemitones = 12;
            s.addSound (snd);
            const auto x = play (s, 36, 1.0f, 1000);
            int last = 0;
            for (int i = 1; i < 1000; ++i) if (x[(size_t) i] != 0.0f) last = i;
            expect ("12 semitones up: twice as fast, half as long", std::abs (x[100] / x[50] - 2.0f) < 1e-3 && last == 499, "last %.0f", (double) last);
        }
        // retrigger: a second hit cuts the first; without it the two ring together
        for (bool retrigger : { true, false })
        {
            vs::Sampler s;
            s.prepare (fs);
            vs::Sampler::Sound snd;
            snd.left.assign (48000, 0.5f);
            snd.sampleRate = fs;
            snd.rootNote = snd.lowNote = snd.highNote = 36;
            snd.retrigger = retrigger;
            snd.releaseSeconds = 0.004f;
            s.addSound (snd);
            std::vector<float> l (9600, 0.0f), r (9600, 0.0f);
            s.noteOn (36, 1.0f);
            s.render (l.data(), r.data(), 4800);
            s.noteOn (36, 1.0f);
            s.render (l.data() + 4800, r.data() + 4800, 4800);
            const double ratio = l[9000] / l[100];
            expect (retrigger ? "retrigger: a second hit cuts the first" : "without retrigger, two hits ring together", retrigger ? std::abs (ratio - 1.0) < 1e-4 : std::abs (ratio - 2.0) < 1e-4,
                    "%.4f", ratio);
        }
        // the velocity curve
        {
            vs::Sampler s;
            s.prepare (fs);
            vs::Sampler::Sound snd;
            snd.left.assign (100, 0.5f);
            snd.sampleRate = fs;
            snd.rootNote = snd.lowNote = snd.highNote = 36;
            s.addSound (snd);
            const float straight = play (s, 36, 0.5f, 10)[5];
            s.setVelocityCurve (1.6f);
            s.allNotesOff (true);
            const float curved = play (s, 36, 0.5f, 10)[5];
            expect ("velocity to the power of the curve", std::abs (curved / straight - std::pow (0.5f, 0.6f)) < 1e-5, "%.5f", curved / straight);
        }
    }

    void loops()
    {
        // a two-beat loop at 120 BPM (1 s): a 1.5 s tone hit at beat 1 rings 0.5 s past the end, folded back into the start
        vs::Sampler s;
        s.prepare (fs);
        vs::Sampler::Sound snd;
        snd.left.assign (72000, 0.2f);
        snd.sampleRate = fs;
        snd.rootNote = snd.lowNote = snd.highNote = 36;
        s.addSound (snd);
        auto [l, r] = vs::sampler::renderLoop (s, { { 36, 1.0, 1.0f }, { 36, 2.0, 1.0f } }, 120, 2, fs, 3.0, 0.85f);
        expect ("a loop is its length", l.size() == 48000 && r.size() == 48000, "%.0f frames", (double) l.size());
        const float one = vs::softClip (0.2f * 0.85f * (float) (1.4142135623730951 * std::sqrt (0.5)));
        expect ("before the hit: the tail folded back from the end", std::abs (l[100] - one) < 1e-5, "%.5f", l[100]);
        // at 0.625 s the hit sounds, and so does its own tail from 1.625 s, folded back
        expect ("the hit at beat 1, over its own folded tail", std::abs (l[30000] - vs::softClip (2 * 0.2f * 0.85f)) < 1e-5, "%.5f", l[30000]);
        expect ("a hit at the loop's end is left out", std::abs (l[20000] - one) < 1e-5);
        // four loud hits on top of each other stay under full scale, through the safety curve
        auto [L, R] = vs::sampler::renderLoop (s, { { 36, 0, 1 }, { 36, 0, 1 }, { 36, 0, 1 }, { 36, 0, 1 }, { 36, 0, 1 }, { 36, 0, 1 } }, 120, 2, fs);
        float peak = 0;
        for (float v : L) peak = std::max (peak, std::abs (v));
        expect ("a pile-up stays under full scale", peak < 1.0f && peak > 0.7f, "%.4f", peak);

        expect ("the safety curve is straight to 0.7", vs::softClip (0.5f) == 0.5f && vs::softClip (-0.69f) == -0.69f);
        expect ("then bends toward full scale, never past it", vs::softClip (0.9f) < 0.9f && vs::softClip (5.0f) <= 1.0f && vs::softClip (-5.0f) >= -1.0f
                && vs::softClip (1.0f) < 0.95f
                && std::abs (vs::softClip (0.7001f) - 0.7001f) < 1e-6);
    }

    void onsets()
    {
        std::vector<float> a (1000, 0.0f), b (1000, 0.0f);
        a[500] = 1.0f;
        b[300] = 0.02f;
        b[400] = 0.2f;
        const float* ch[2] { a.data(), b.data() };
        expect ("onset: the first sample in any channel at the fraction of the peak", vs::channels::onsetIndex (ch, 2, 1000, 0.1) == 400
                && vs::channels::onsetIndex (ch, 2, 1000, 0.01) == 300 && vs::channels::onsetIndex (ch, 1, 1000, 0.5) == 500);
        const std::vector<float> z (10, 0.0f);
        const float* zc[1] { z.data() };
        expect ("onset: silence starts at once", vs::channels::onsetIndex (zc, 1, 10, 0.3) == 0);
        std::vector<float> hit (20, 0.0f);
        hit[10] = 0.5f;
        expect ("a synthesized hit from its onset keeps the two samples before", vs::channels::fromOnset (hit).size() == 12);
    }

    void timing()
    {
        using namespace vs::noteedit::loop;
        expect ("a time's place in the loop, wrapping", std::abs (beatAt (5.25, 0.25, 120, 8) - 2.0) < 1e-12 && std::abs (beatAt (0.0, 0.5, 120, 4) - 3.0) < 1e-12);
        expect ("snapped to sixteenths, the loop's end wrapping to 0", snap (1.13, 4, 8) == 1.25 && snap (7.9, 4, 8) == 0.0 && snap (1.13, 0, 8) == 1.13);
        expect ("snapped to eighth triplets", std::abs (snap (0.3, 3, 4) - 1.0 / 3.0) < 1e-12);
        const double beats[] { 0.0, 1.5, 3.0 };
        const double notBefore[] { 0.0, 0.0, 10.0 };
        const auto due = hitsBetween (beats, notBefore, 3, 1.9, 4.1, 0.0, 120, 4);
        // passes start at 0, 2 and 4 s: hits at 0, 0.75, 1.5, then 2, 2.75, 3.5, then 4 s...; the last hit waits until 10 s
        expect ("the hits due in a window, over passes, in time order", due.size() == 3 && due[0].time == 2.0 && due[0].hit == 0 && due[1].time == 2.75 && due[1].hit == 1
                && due[2].time == 4.0 && due[2].hit == 0, "%.0f due", (double) due.size());
        expect ("nothing before the loop starts", hitsBetween (beats, nullptr, 3, -5, 0, 0, 120, 4).empty());
        const auto clicks = clicksBetween (0.9, 2.6, 0.0, 120, 4);
        expect ("the clicks in a window, the bar's first marked", clicks.size() == 4 && clicks[0].time == 1.0 && clicks[0].beat == 2 && ! clicks[0].down && clicks[2].down,
                "%.0f clicks", (double) clicks.size());
        expect ("the next time on a grid, at or after t", nextGridTime (1.0, 0.1, 0.25) == 1.1 && std::abs (nextGridTime (1.1, 0.1, 0.25) - 1.1) < 1e-12);
    }
}

int main()
{
    pads();
    loops();
    onsets();
    timing();
    std::printf ("%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
