// The shared processing parts: the gain curve, the cut glide and the dynamic
// bell (dsp/Dynamics.h), the delay line (dsp/DelayLine.h), the dB and
// mid/side helpers (dsp/Util.h), and Space (dsp/Space.h) on ReverbEngine's
// Space setting. Built without JUCE, as the browser build is. Each check
// prints PASS or FAIL; the suite exits non-zero on any failure.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include "dsp/DelayLine.h"
#include "dsp/Dynamics.h"
#include "dsp/Space.h"
#include "dsp/Util.h"

namespace
{
    int failures = 0;
    constexpr double pi = 3.141592653589793;

    void expect (const char* name, bool ok, double value = 0.0)
    {
        std::printf ("%s %-70s %g\n", ok ? "PASS" : "FAIL", name, value);
        if (! ok) ++failures;
    }

    void curveTests()
    {
        using vs::dynamics::cutDb;
        using vs::dynamics::compressedDb;
        expect ("cutDb: hard knee, nothing at or below the threshold", cutDb (0.0f, 0.5f) == 0.0f && cutDb (-3.0f, 0.5f) == 0.0f);
        expect ("cutDb: hard knee, slope x overshoot above it", cutDb (6.0f, 0.5f) == 3.0f);
        expect ("cutDb: never more than the limit", cutDb (20.0f, 1.5f, 0.0f, 14.0f) == 14.0f);
        // the soft knee meets the straight part at its edges
        const float slope = 1.0f - 1.0f / 4.0f;
        const float in = cutDb (2.999f, slope, 6.0f), out = cutDb (3.001f, slope, 6.0f);
        expect ("cutDb: the soft knee joins the straight line at +knee/2", std::abs (in - out) < 2e-3f, in - out);
        expect ("cutDb: the soft knee starts at -knee/2", cutDb (-3.0f, slope, 6.0f) == 0.0f && cutDb (-2.9f, slope, 6.0f) > 0.0f);
        // the compressor's form is the same curve
        float worst = 0.0f;
        for (float x = -40.0f; x <= 10.0f; x += 0.01f)
        {
            const float threshold = -18.0f, ratio = 3.0f;
            const float a = compressedDb (x, threshold, ratio, 6.0f) - x, b = -cutDb (x - threshold, 1.0f - 1.0f / ratio, 6.0f);
            worst = std::max (worst, std::abs (a - b));
        }
        expect ("compressedDb and cutDb are the same curve (largest difference, dB)", worst < 1e-4f, worst);

        float cut = 0.0f;
        for (int k = 0; k < 2000; ++k) vs::dynamics::glideCut (cut, 6.0f, 0.001f, 0.003f, 0.12f);
        expect ("glideCut: reaches its target", std::abs (cut - 6.0f) < 1e-3f, cut);
        for (int k = 0; k < 20000; ++k) vs::dynamics::glideCut (cut, 0.0f, 0.001f, 0.003f, 0.12f);
        expect ("glideCut: lets go to exactly none", cut == 0.0f, cut);
        cut = 0.0f;
        vs::dynamics::glideCut (cut, 6.0f, 0.003f, 0.003f, 0.12f);
        expect ("glideCut: one attack time takes 63% of the way", std::abs (cut - 6.0f * (1.0f - std::exp (-1.0f))) < 1e-4f, cut);
    }

    void bellTests()
    {
        const double fs = 48000.0;
        auto sine = [fs] (double hz, int n) { std::vector<float> x ((size_t) n); for (int i = 0; i < n; ++i) x[(size_t) i] = (float) std::sin (2 * pi * hz * i / fs); return x; };
        // no cut: exactly the input
        {
            vs::DynamicBell b; b.prepare (fs, 3000.0f, 2.0);
            auto x = sine (3000.0, 4800), y = x;
            float* ch[1] { y.data() };
            b.process (ch, 1, 0, (int) y.size(), 0.0f);
            expect ("DynamicBell: no cut is exact pass-through", x == y);
        }
        // a 6 dB cut at the centre
        {
            vs::DynamicBell b; b.prepare (fs, 3000.0f, 2.0);
            auto y = sine (3000.0, 48000);
            float* ch[1] { y.data() };
            b.process (ch, 1, 0, (int) y.size(), 6.0f);
            double e = 0.0; for (size_t i = 24000; i < y.size(); ++i) e += (double) y[i] * y[i];
            const double db = 10 * std::log10 (e / 24000 / 0.5);
            expect ("DynamicBell: a 6 dB cut takes 6 dB off its centre", std::abs (db + 6.0) < 0.05, db);
        }
        // redesigned only when the cut moves by more than 0.05 dB
        {
            vs::DynamicBell b; b.prepare (fs, 1000.0f, 1.0);
            b.setCut (3.0f);
            b.setCut (3.04f);
            const bool kept = b.appliedCutDb() == 3.0f;
            b.setCut (3.1f);
            expect ("DynamicBell: redesigns on moves over 0.05 dB", kept && b.appliedCutDb() == 3.1f);
        }
    }

    void delayTests()
    {
        vs::DelayLine d;
        d.prepare (100);
        for (int i = 0; i < 50; ++i) d.write ((float) i);
        expect ("DelayLine: at (1) is the newest, at (10) ten back", d.at (1) == 49.0f && d.at (10) == 40.0f);
        expect ("DelayLine: read (1.5) halfway between", std::abs (d.read (1.5f) - 48.5f) < 1e-5f && std::abs (d.read (1.5) - 48.5f) < 1e-5f);
        expect ("DelayLine: Hermite reads a straight line exactly", std::abs (d.readHermite (3.25) - 46.75f) < 1e-5f, d.readHermite (3.25));
        d.put (50.0f);
        expect ("DelayLine: a put sample reads at delay 0, before advance", d.read (0.0f) == 50.0f && d.at (0) == 50.0f);
        d.advance();
        expect ("DelayLine: ... and at delay 1 after", d.at (1) == 50.0f);
        // a long delay read in double keeps its fraction
        vs::DelayLine big;
        big.prepare (120000);
        for (int i = 0; i < 130000; ++i) big.write ((float) i * 0.001f);
        const float got = big.read (100000.25);
        expect ("DelayLine: double positions keep the fraction 100000 samples back", std::abs (got - 29.99975f) < 4e-6f, got);
        d.clear (1.0f);
        expect ("DelayLine: clear (v) fills with v", d.at (7) == 1.0f && d.at (77) == 1.0f);
    }

    void helperTests()
    {
        expect ("dbToGain: -6 dB is 0.501", std::abs (vs::dbToGain (-6.0f) - 0.501187f) < 1e-5f);
        expect ("gainFromDb: 20 dB is 10", std::abs (vs::gainFromDb (20.0) - 10.0) < 1e-12);
        expect ("powerToDb, ampToDb: their floors (-120 and -180 dB)", std::abs (vs::powerToDb (0.0f) + 120.0f) < 1e-3f && std::abs (vs::ampToDb (0.0f) + 180.0f) < 1e-3f);
        expect ("ampToDb with no floor: silence is -inf", std::isinf (vs::ampToDb (0.0f, 0.0f)));
        expect ("db10, db20: their floors (-200 and -240 dB)", std::abs (vs::db10 (0.0) + 200.0) < 1e-9 && std::abs (vs::db20 (0.0) + 240.0) < 1e-9);
        const float l = 0.3f, r = -0.7f, m = vs::midOf (l, r), s = vs::sideOf (l, r);
        expect ("midOf, sideOf: mid + side and mid - side give left and right back", std::abs (m + s - l) < 1e-7f && std::abs (m - s - r) < 1e-7f);
        expect ("roundHalfUp: halves up, as JavaScript's Math.round", vs::roundHalfUp (2.5) == 3.0 && vs::roundHalfUp (-2.5) == -2.0 && vs::roundHalfUp (0.49999999999999994) == 0.0);
        expect ("lerp: a quarter of the way", vs::lerp (2.0f, 6.0f, 0.25f) == 3.0f);
        float env = 0.0f;
        vs::followEnvelope (env, 1.0f, 0.5f, 0.1f);
        const float up = env;
        vs::followEnvelope (env, 0.0f, 0.5f, 0.1f);
        expect ("followEnvelope: rises by `up`, falls by `down`", std::abs (up - 0.5f) < 1e-7f && std::abs (env - 0.45f) < 1e-7f);
    }

    void spaceTests()
    {
        const double fs = 48000.0;
        const int n = (int) (fs * 4);
        // echo only: a click comes back after the delay time, darker and quieter
        {
            vs::Space sp; sp.prepare ({ fs, 512, 2 });
            std::vector<float> l ((size_t) n, 0.0f), r ((size_t) n, 0.0f);
            l[100] = r[100] = 1.0f;
            for (int at = 0; at < n; at += 512)
            {
                sp.setParams (0.0f, 0.5f, 1.0f, 0.25f, 0.3f);
                float* ch[2] { l.data() + at, r.data() + at };
                sp.process (ch, std::min (512, n - at));
            }
            int peakAt = 0; float peak = 0.0f;
            for (int i = 200; i < 20000; ++i) if (std::abs (l[(size_t) i]) > peak) { peak = std::abs (l[(size_t) i]); peakAt = i; }
            expect ("Space: the echo comes back a quarter second later (samples)", std::abs (peakAt - (100 + 12000)) < 40, peakAt - 100);
        }
        // reverb only: a longer tail as the size grows
        double last = 0.0;
        bool longer = true;
        for (float size : { 0.0f, 0.5f, 1.0f })
        {
            vs::Space sp; sp.prepare ({ fs, 512, 2 });
            std::vector<float> l ((size_t) n, 0.0f), r ((size_t) n, 0.0f);
            for (int i = 0; i < 2400; ++i) l[(size_t) i] = r[(size_t) i] = (float) (0.5 * std::sin (2 * pi * 440 * i / fs));
            for (int at = 0; at < n; at += 512)
            {
                sp.setParams (1.0f, size, 0.0f, 0.25f, 0.3f);
                float* ch[2] { l.data() + at, r.data() + at };
                sp.process (ch, std::min (512, n - at));
            }
            double tail = 0.0; for (int i = (int) fs; i < n; ++i) tail += (double) l[(size_t) i] * l[(size_t) i];
            longer = longer && tail > last;
            last = tail;
        }
        expect ("Space: the reverb rings longer as Size grows", longer);
        const auto s = vs::ReverbEngine::space (0.5f);
        expect ("ReverbEngine::space: a room, decay 0.9 s at size 0 to 6.3 s at size 1",
                s.mode == vs::ReverbEngine::room && std::abs (vs::ReverbEngine::space (0.0f).decaySec - 0.895f) < 0.01f && std::abs (vs::ReverbEngine::space (1.0f).decaySec - 6.30f) < 0.02f,
                s.decaySec);
    }
}

int main()
{
    curveTests();
    bellTests();
    delayTests();
    helperTests();
    spaceTests();
    std::printf ("\n%s (%d failed)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
