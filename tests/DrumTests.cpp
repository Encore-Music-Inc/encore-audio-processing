// Tuning drums (dsp/DrumPitch.h), built without JUCE as the browser build
// is. Each check prints PASS or FAIL; the suite exits non-zero on any failure.
#include <cmath>
#include <cstdio>
#include <vector>
#include "dsp/DrumPitch.h"

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

    // a struck drum: its partials (ratio, level), each dying away faster than the one below
    std::vector<float> hit (double f0, double fs, std::vector<std::pair<double, double>> partials)
    {
        std::vector<float> x ((size_t) (fs * 0.6));
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = i / fs;
            double v = 0;
            for (size_t k = 0; k < partials.size(); ++k)
                v += partials[k].second * std::sin (twoPi * f0 * partials[k].first * t) * std::exp (-t * (4.0 + 5.0 * k));
            x[i] = (float) (0.4 * v);
        }
        return x;
    }
}

int main()
{
    const double fs = 48000.0;
    const auto tom = hit (147, fs, { { 1, 1 }, { 1.59, 0.5 }, { 2.14, 0.3 } });
    const double f = vs::drums::drumPeak (tom.data(), (int) tom.size(), fs, 50, 1500);
    expect ("finds a tom's 147 Hz", std::abs (f - 147) < 0.5, "%.2f Hz", f);

    // hit near a rod, the first overtone rings loudest; in the middle, the lowest peak is the drum's note
    const auto edge = hit (110, fs, { { 1, 0.6 }, { 1.59, 2.5 }, { 2.14, 0.4 } });
    const double strongest = vs::drums::drumPeak (edge.data(), (int) edge.size(), fs, 30, 500);
    const double lowest = vs::drums::drumPeak (edge.data(), (int) edge.size(), fs, 30, 500, true);
    expect ("the strongest peak, or the lowest clear one", std::abs (strongest - 174.9) < 1 && std::abs (lowest - 110) < 0.5, "%.1f / %.1f Hz", strongest, lowest);

    const std::vector<float> quiet (20000, 0.0005f);
    expect ("quieter than -60 dB is no note", vs::drums::drumPeak (quiet.data(), (int) quiet.size(), fs, 50, 1500) == 0);
    const auto shortHit = hit (300, fs, { { 1, 1 } });
    const double s = vs::drums::drumPeak (shortHit.data(), 4096, fs, 50, 1500, false, 4096);
    expect ("a smaller transform still finds the note", std::abs (s - 300) < 3, "%.1f Hz", s);

    const auto rods = vs::drums::rodSpread ({ 200, 0, 202, 198 });
    expect ("rods: the geometric mean of those measured", std::abs (rods.mean - std::cbrt (200.0 * 202 * 198)) < 1e-9, "%.3f Hz", rods.mean);
    expect ("rods: each one's cents from it, nothing for one not measured",
            std::isnan (rods.cents[1]) && std::abs (rods.cents[2] - 1200 * std::log2 (202 / rods.mean)) < 1e-9 && rods.cents[2] > 0 && rods.cents[3] < 0);
    expect ("rods: none measured", vs::drums::rodSpread ({ 0, 0 }).mean == 0 && std::isnan (vs::drums::rodSpread ({ 0 }).cents[0]));
    expect ("toms in steps down from the top", vs::drums::tomSteps (55, 4, 3) == std::vector<double> { 55, 52, 49, 46 });

    std::printf ("%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
