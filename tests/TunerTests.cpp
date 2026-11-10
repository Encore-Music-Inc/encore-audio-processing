// The tuner (dsp/TunerPitch.h) and its instrument list (dsp/Tunings.h, made by
// tools/tunings.py), built without JUCE as the browser build is. Each check
// prints PASS or FAIL; the suite exits non-zero on any failure.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include "dsp/TunerPitch.h"
#include "dsp/Tunings.h"

namespace
{
    int failures = 0;
    constexpr double twoPi = 6.283185307179586;

    void expect (const char* name, bool ok, const char* fmt = "", double a = 0, double b = 0, double c = 0)
    {
        std::printf ("%s %-58s ", ok ? "PASS" : "FAIL", name);
        std::printf (fmt, a, b, c);
        std::printf ("\n");
        if (! ok) ++failures;
    }

    const vs::tunings::Instrument* instrument (const char* name)
    {
        for (auto& f : vs::tunings::families())
            for (auto& i : f.instruments)
                if (std::strcmp (i.name, name) == 0) return &i;
        return nullptr;
    }

    // how far the detector is from hz, in cents, on 2048 samples of wave at 24 kHz
    double centsOff (double hz, const std::function<float (double)>& wave)
    {
        const double fs = 24000.0;
        std::vector<float> x (2048);
        for (size_t i = 0; i < x.size(); ++i) x[i] = wave ((double) i / fs);
        const float f = vs::TunerPitch::estimate (x.data(), (int) x.size(), fs);
        return f > 0 ? 1200.0 * std::log2 (f / hz) : 9999.0;
    }
}

int main()
{
    // ---- the detector ----
    {
        bool all = true;
        double worst = 0;
        for (float f : { 27.5f, 41.2f, 98.0f, 659.26f, 1760.0f })
        {
            std::vector<float> x (4096);
            for (size_t i = 0; i < x.size(); ++i)
                x[i] = 0.5f * (float) std::sin (twoPi * f * (double) i / 24000.0) + 0.2f * (float) std::sin (twoPi * 2 * f * (double) i / 24000.0);
            const float got = vs::TunerPitch::estimate (x.data(), (int) x.size(), 24000.0);
            all = all && std::abs (got / f - 1.0f) < 0.003f;
            worst = std::max (worst, (double) std::abs (got / f - 1.0f));
        }
        expect ("hears 27.5 Hz (a low A) to 1.76 kHz within 0.3%", all, "worst %.4f%%", worst * 100);
    }
    {
        const double e2 = centsOff (82.41, [] (double t) { return (float) (0.3 * std::sin (twoPi * 82.41 * t)); });
        const double e1 = centsOff (41.20, [] (double t) { return (float) (0.3 * std::sin (twoPi * 41.20 * t)); });
        const double a4 = centsOff (440.0, [] (double t) { return (float) (0.2 * std::sin (twoPi * 440 * t) + 0.12 * std::sin (twoPi * 880 * t) + 0.08 * std::sin (twoPi * 1320 * t)); });
        expect ("reads guitar and bass low E and a bright A4 within 2 cents", std::abs (e2) < 2 && std::abs (e1) < 2 && std::abs (a4) < 2,
                "%.2f / %.2f / %.2f cents", e2, e1, a4);
    }
    {
        std::vector<float> silence (4096, 0.0f), noise (4096);
        std::mt19937 rng (5);
        std::uniform_real_distribution<float> u (-0.3f, 0.3f);
        for (auto& v : noise) v = u (rng);
        const float s = vs::TunerPitch::estimate (silence.data(), 4096, 24000.0);
        const float n = vs::TunerPitch::estimate (noise.data(), 4096, 24000.0);
        expect ("hears no note in silence or noise", s == 0 && n == 0, "%.2f / %.2f Hz", s, n);
    }
    {
        std::vector<float> x (100, 0.5f);
        expect ("a block too short for the lowest note gives no note", vs::TunerPitch::estimate (x.data(), 100, 48000.0) == 0);
    }

    // ---- the instrument list ----
    int instruments = 0;
    bool coursesAddUp = true, notesInRange = true;
    for (auto& f : vs::tunings::families())
        for (auto& i : f.instruments)
        {
            ++instruments;
            for (auto& t : i.tunings)
            {
                int sum = 0;
                for (auto c : t.courses) sum += c;
                coursesAddUp = coursesAddUp && (t.courses.empty() || sum == (int) t.strings.size());
                for (auto n : t.strings) notesInRange = notesInRange && n >= 20 && n <= 100;
            }
        }
    const auto* guitar = instrument ("Guitar (6-string)");
    const auto* trumpet = instrument ("Trumpet / cornet (Bb)");
    const auto* twelve = instrument ("Guitar (12-string)");
    expect ("the instrument list has guitar, trumpet and bouzouki", instruments >= 80 && guitar && trumpet && instrument ("Bouzouki (Irish)"),
            "%.0f instruments", instruments);
    expect ("every course adds up to its strings, every note in range", coursesAddUp && notesInRange);
    expect ("a 12-string has all 12 strings, in 6 pairs", twelve && twelve->tunings[0].strings.size() == 12
                                                              && twelve->tunings[0].courses.size() == 6 && twelve->tunings[0].strings[1] == 52);
    expect ("standard guitar tuning is E2 A2 D3 G3 B3 E4", guitar && guitar->tunings[0].strings == std::vector<int> { 40, 45, 50, 55, 59, 64 });
    expect ("seven temperaments, equal first; 440 Hz the first reference",
            vs::tunings::temperaments().size() == 7 && vs::tunings::temperaments()[0].cents == std::array<float, 12> {}
                && vs::tunings::references()[0].hz == 440);

    // ---- what a pitch means ----
    if (guitar && trumpet)
    {
        const std::array<float, 12> equal {};
        const auto& strings = guitar->tunings[0].strings;
        const auto a = vs::TunerReading::of (110.0f, 440.0, strings, equal, 0);
        const auto sharp = vs::TunerReading::of (112.0f, 440.0, strings, equal, 0);
        expect ("finds the guitar string, and how sharp it is", a.string == 1 && a.note == 45 && std::abs (a.cents) < 0.5f
                                                                   && sharp.string == 1 && std::abs (sharp.cents - 31.2f) < 1.0f, "%.1f cents", sharp.cents);
        const auto far = vs::TunerReading::of (1000.0f, 440.0, strings, equal, 0);
        expect ("a note far from every string reads chromatically", far.string == -1 && far.note == 83, "note %.0f", far.note);
        const auto horn = vs::TunerReading::of (233.08f, 440.0, {}, equal, 0);
        expect ("a Bb trumpet's concert Bb3 is its written C4", trumpet->transpose == 2 && horn.note + trumpet->transpose == 60);
        const auto e = vs::TunerReading::of (329.63f, 440.0, {}, vs::tunings::temperaments()[3].cents, 0);
        expect ("quarter-comma meantone wants a lower E", e.note == 64 && std::abs (e.cents - 13.7f) < 0.6f, "%.1f cents", e.cents);
        const auto a415 = vs::TunerReading::of (415.0f, 415.0, {}, equal, 0);
        expect ("A at a baroque 415 Hz reference reads as A in tune", a415.note == 69 && std::abs (a415.cents) < 0.01f && std::abs (a415.targetHz - 415.0f) < 0.01f);
        const auto none = vs::TunerReading::of (0.0f, 440.0, strings, equal, 0);
        expect ("no pitch, no reading", none.note == -1 && none.string == -1);
    }
    else expect ("guitar and trumpet are in the list", false);

    std::printf ("%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
